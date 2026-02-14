// Copyright Epic Games, Inc. All Rights Reserved.
// UnrealAgentLink - Widget Commands

#include "UAL_WidgetCommands.h"
#include "UAL_CommandUtils.h"

// UMG & Widget Blueprint
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Border.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/ProgressBar.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/SpinBox.h"
#include "Components/RichTextBlock.h"
#include "Components/GridPanel.h"
#include "Components/GridSlot.h"
#include "Components/WrapBox.h"
#include "Components/UniformGridPanel.h"

// Editor utilities
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// Rendering (for Preview)
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

// 仅编辑器模式 (UMGEditor 模块)
#if WITH_EDITOR
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "FileHelpers.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALWidget, Log, All);

// ============================================================================
// 辅助：生成唯一 Widget 名称（防止重名崩溃）
// ============================================================================

static FName MakeUniqueWidgetName(UWidgetTree* WidgetTree, const FString& DesiredName)
{
	if (DesiredName.IsEmpty())
	{
		return NAME_None; // 让 ConstructWidget 自动生成
	}

	FName TestName(*DesiredName);
	if (!WidgetTree->FindWidget(TestName))
	{
		return TestName;
	}

	// 名称冲突，追加后缀
	for (int32 i = 1; i < 1000; ++i)
	{
		FString SuffixedName = FString::Printf(TEXT("%s_%d"), *DesiredName, i);
		TestName = FName(*SuffixedName);
		if (!WidgetTree->FindWidget(TestName))
		{
			UE_LOG(LogUALWidget, Warning, TEXT("Widget name '%s' already exists, renamed to '%s'"),
				*DesiredName, *SuffixedName);
			return TestName;
		}
	}

	// 极端情况，回退到自动命名
	UE_LOG(LogUALWidget, Error, TEXT("Failed to generate unique name for '%s' after 999 attempts"), *DesiredName);
	return NAME_None;
}

// ============================================================================
// 命令注册
// ============================================================================

void FUAL_WidgetCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	// Phase 1: 只读能力
	CommandMap.Add(TEXT("widget.get_hierarchy"), &Handle_GetHierarchy);
	
	// Phase 2: 创建 + CanvasPanel 布局
	CommandMap.Add(TEXT("widget.create"), &Handle_Create);
	CommandMap.Add(TEXT("widget.add_control"), &Handle_AddControl);
	CommandMap.Add(TEXT("widget.set_canvas_slot"), &Handle_SetCanvasSlot);
	
	// Phase 3: 通用子控件添加
	CommandMap.Add(TEXT("widget.add_child"), &Handle_AddChild);
	// 向后兼容
	CommandMap.Add(TEXT("widget.add_to_vertical"), &Handle_AddToVertical);
	CommandMap.Add(TEXT("widget.add_to_horizontal"), &Handle_AddToHorizontal);
	CommandMap.Add(TEXT("widget.set_vertical_slot"), &Handle_SetVerticalSlot);
	
	// Phase 4: 预览渲染
	CommandMap.Add(TEXT("widget.preview"), &Handle_Preview);
	
	// Phase 5: 事件绑定
	CommandMap.Add(TEXT("widget.make_variable"), &Handle_MakeVariable);
	CommandMap.Add(TEXT("widget.set_property"), &Handle_SetProperty);
	
	UE_LOG(LogUALWidget, Log, TEXT("FUAL_WidgetCommands: Registered %d widget commands"), 11);
}

// ============================================================================
// Phase 1: 只读能力 - widget.get_hierarchy
// ============================================================================

void FUAL_WidgetCommands::Handle_GetHierarchy(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}

	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}

	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (!WidgetTree)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("WidgetBlueprint has no WidgetTree"));
		return;
	}

	UWidget* RootWidget = WidgetTree->RootWidget;
	if (!RootWidget)
	{
		// 空 Widget，返回空结构
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), Path);
		Result->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
		Result->SetNumberField(TEXT("widget_count"), 0);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}

	// 递归构建 Widget 层级
	TSharedPtr<FJsonObject> RootJson = BuildWidgetJson(RootWidget);
	
	// 统计控件数量
	int32 WidgetCount = 0;
	WidgetTree->ForEachWidget([&WidgetCount](UWidget* Widget)
	{
		WidgetCount++;
	});

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("name"), WidgetBP->GetName());
	Result->SetObjectField(TEXT("root"), RootJson);
	Result->SetNumberField(TEXT("widget_count"), WidgetCount);
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.get_hierarchy: path=%s, widget_count=%d"), *Path, WidgetCount);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.get_hierarchy is only available in editor mode"));
#endif
}

// ============================================================================
// Phase 2: 创建 + CanvasPanel 布局
// ============================================================================

void FUAL_WidgetCommands::Handle_Create(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 解析参数
	FString Name;
	if (!Payload->TryGetStringField(TEXT("name"), Name))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: name"));
		return;
	}
	
	FString Folder = TEXT("/Game/UI");
	Payload->TryGetStringField(TEXT("folder"), Folder);
	
	FString RootType = TEXT("CanvasPanel");
	Payload->TryGetStringField(TEXT("root_type"), RootType);
	
	// 确保文件夹路径格式正确
	if (!Folder.StartsWith(TEXT("/")))
	{
		Folder = TEXT("/") + Folder;
	}
	
	// 检查资产是否已存在（防止重名崩溃）
	FString AssetPath = Folder / Name;
	if (StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath))
	{
		UAL_CommandUtils::SendError(RequestId, 409,
			FString::Printf(TEXT("Widget Blueprint already exists: %s"), *AssetPath));
		return;
	}
	
	// 事务包裹
	const FScopedTransaction Transaction(NSLOCTEXT("UAL", "CreateWidget", "Agent Create Widget"));
	
	// 创建 Widget Blueprint
	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
	Factory->ParentClass = UUserWidget::StaticClass();
	
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UObject* NewAsset = AssetToolsModule.Get().CreateAsset(Name, Folder, UWidgetBlueprint::StaticClass(), Factory);
	
	if (!NewAsset)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create Widget Blueprint asset"));
		return;
	}
	
	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(NewAsset);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Created asset is not a Widget Blueprint"));
		return;
	}
	
	// 标记蓝图被修改
	WidgetBP->Modify();
	
	// 设置根控件
	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (WidgetTree)
	{
		// 创建根控件
		UClass* RootClass = FindWidgetClass(RootType);
		if (!RootClass)
		{
			RootClass = UCanvasPanel::StaticClass(); // 默认 CanvasPanel
		}
		
		UWidget* RootWidget = WidgetTree->ConstructWidget<UWidget>(RootClass);
		if (RootWidget)
		{
			RootWidget->SetDesignerFlags(EWidgetDesignFlags::Designing);
			WidgetTree->RootWidget = RootWidget;
		}
	}
	
	// 标记结构修改并保存
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	// 保存资产
	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(WidgetBP->GetOutermost());
	FEditorFileUtils::PromptForCheckoutAndSave(PackagesToSave, false, false);
	
	// 构建响应
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), WidgetBP->GetName());
	Result->SetStringField(TEXT("path"), WidgetBP->GetPathName());
	Result->SetStringField(TEXT("root_type"), RootType);
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.create: name=%s, path=%s"), *Name, *WidgetBP->GetPathName());
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.create is only available in editor mode"));
#endif
}

void FUAL_WidgetCommands::Handle_AddControl(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 解析参数
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString ControlType;
	if (!Payload->TryGetStringField(TEXT("control_type"), ControlType))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: control_type"));
		return;
	}
	
	FString WidgetName;
	Payload->TryGetStringField(TEXT("name"), WidgetName);
	
	FString ParentName;
	Payload->TryGetStringField(TEXT("parent"), ParentName);
	
	// 加载 WidgetBlueprint
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	// 事务包裹
	const FScopedTransaction Transaction(NSLOCTEXT("UAL", "AddControl", "Agent Add Control"));
	WidgetBP->Modify();
	
	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (!WidgetTree)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("WidgetBlueprint has no WidgetTree"));
		return;
	}
	
	// 查找父控件
	UWidget* Parent = FindWidgetByName(WidgetTree, ParentName);
	if (!Parent)
	{
		Parent = WidgetTree->RootWidget;
	}
	
	// 验证父控件是 CanvasPanel
	UCanvasPanel* Canvas = Cast<UCanvasPanel>(Parent);
	if (!Canvas)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Parent '%s' is not a CanvasPanel. Use widget.add_to_vertical or similar for other containers."), 
			Parent ? *Parent->GetName() : TEXT("root")));
		return;
	}
	
	// 查找控件类
	UClass* WidgetClass = FindWidgetClass(ControlType);
	if (!WidgetClass)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Unknown control type: %s"), *ControlType));
		return;
	}
	
	// 生成唯一名称（防止重名崩溃）
	FName UniqueName = NAME_None;
	if (!WidgetName.IsEmpty())
	{
		UniqueName = MakeUniqueWidgetName(WidgetTree, WidgetName);
	}
	
	// 创建控件
	UWidget* NewWidget = WidgetTree->ConstructWidget<UWidget>(WidgetClass, UniqueName);
	if (!NewWidget)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to construct widget"));
		return;
	}
	
	// 设置设计器标记（避坑1修复）
	NewWidget->SetDesignerFlags(EWidgetDesignFlags::Designing);
	
	// 添加到 Canvas
	UCanvasPanelSlot* Slot = Canvas->AddChildToCanvas(NewWidget);
	if (!Slot)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to canvas"));
		return;
	}
	
	// 设置 Slot 属性
	FString AnchorsStr;
	if (Payload->TryGetStringField(TEXT("anchors"), AnchorsStr))
	{
		Slot->SetAnchors(ParseAnchors(AnchorsStr));
	}
	
	const TSharedPtr<FJsonObject>* PosObj;
	if (Payload->TryGetObjectField(TEXT("position"), PosObj))
	{
		Slot->SetPosition(ParseVector2D(*PosObj));
	}
	
	const TSharedPtr<FJsonObject>* SizeObj;
	if (Payload->TryGetObjectField(TEXT("size"), SizeObj))
	{
		Slot->SetSize(ParseVector2D(*SizeObj));
	}
	
	// 标记蓝图结构修改（避坑4修复）
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	// 构建响应
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), NewWidget->GetName());
	Result->SetStringField(TEXT("class"), NewWidget->GetClass()->GetName());
	Result->SetStringField(TEXT("parent"), Canvas->GetName());
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.add_control: type=%s, name=%s, parent=%s"), 
		*ControlType, *NewWidget->GetName(), *Canvas->GetName());
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.add_control is only available in editor mode"));
#endif
}

void FUAL_WidgetCommands::Handle_SetCanvasSlot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 解析参数
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString WidgetName;
	if (!Payload->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: widget_name"));
		return;
	}
	
	// 加载 WidgetBlueprint
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	// 查找控件
	UWidget* Widget = FindWidgetByName(WidgetBP->WidgetTree, WidgetName);
	if (!Widget)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
		return;
	}
	
	// 获取 CanvasPanelSlot
	UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Widget->Slot);
	if (!Slot)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Widget '%s' is not in a CanvasPanel"), *WidgetName));
		return;
	}
	
	// 事务包裹
	const FScopedTransaction Transaction(NSLOCTEXT("UAL", "SetCanvasSlot", "Agent Set Canvas Slot"));
	WidgetBP->Modify();
	
	// 更新 Slot 属性
	FString AnchorsStr;
	if (Payload->TryGetStringField(TEXT("anchors"), AnchorsStr))
	{
		Slot->SetAnchors(ParseAnchors(AnchorsStr));
	}
	
	const TSharedPtr<FJsonObject>* PosObj;
	if (Payload->TryGetObjectField(TEXT("position"), PosObj))
	{
		Slot->SetPosition(ParseVector2D(*PosObj));
	}
	
	const TSharedPtr<FJsonObject>* SizeObj;
	if (Payload->TryGetObjectField(TEXT("size"), SizeObj))
	{
		Slot->SetSize(ParseVector2D(*SizeObj));
	}
	
	const TSharedPtr<FJsonObject>* AlignObj;
	if (Payload->TryGetObjectField(TEXT("alignment"), AlignObj))
	{
		Slot->SetAlignment(ParseVector2D(*AlignObj));
	}
	
	int32 ZOrder;
	if (Payload->TryGetNumberField(TEXT("z_order"), ZOrder))
	{
		Slot->SetZOrder(ZOrder);
	}
	
	// 标记修改
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	// 构建响应
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("widget_name"), WidgetName);
	Result->SetObjectField(TEXT("slot_data"), BuildCanvasSlotJson(Slot));
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.set_canvas_slot: widget=%s"), *WidgetName);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.set_canvas_slot is only available in editor mode"));
#endif
}

// ============================================================================
// Phase 3: 其他布局容器
// ============================================================================

void FUAL_WidgetCommands::Handle_AddToVertical(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 解析参数
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString ControlType;
	if (!Payload->TryGetStringField(TEXT("control_type"), ControlType))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: control_type"));
		return;
	}
	
	FString WidgetName;
	Payload->TryGetStringField(TEXT("name"), WidgetName);
	
	FString ParentName;
	Payload->TryGetStringField(TEXT("parent"), ParentName);
	
	// 加载 WidgetBlueprint
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	// 事务包裹
	const FScopedTransaction Transaction(NSLOCTEXT("UAL", "AddToVertical", "Agent Add To VerticalBox"));
	WidgetBP->Modify();
	
	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (!WidgetTree)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("WidgetBlueprint has no WidgetTree"));
		return;
	}
	
	// 查找父控件
	UWidget* Parent = FindWidgetByName(WidgetTree, ParentName);
	if (!Parent)
	{
		Parent = WidgetTree->RootWidget;
	}
	
	// 验证父控件是 VerticalBox
	UVerticalBox* VBox = Cast<UVerticalBox>(Parent);
	if (!VBox)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Parent '%s' is not a VerticalBox"), 
			Parent ? *Parent->GetName() : TEXT("root")));
		return;
	}
	
	// 查找控件类
	UClass* WidgetClass = FindWidgetClass(ControlType);
	if (!WidgetClass)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Unknown control type: %s"), *ControlType));
		return;
	}
	
	// 生成唯一名称（防止重名崩溃）
	FName UniqueName = NAME_None;
	if (!WidgetName.IsEmpty())
	{
		UniqueName = MakeUniqueWidgetName(WidgetTree, WidgetName);
	}
	
	// 创建控件
	UWidget* NewWidget = WidgetTree->ConstructWidget<UWidget>(WidgetClass, UniqueName);
	if (!NewWidget)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to construct widget"));
		return;
	}
	
	NewWidget->SetDesignerFlags(EWidgetDesignFlags::Designing);
	
	// 添加到 VerticalBox
	UVerticalBoxSlot* Slot = Cast<UVerticalBoxSlot>(VBox->AddChild(NewWidget));
	if (!Slot)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to VerticalBox"));
		return;
	}
	
	// 设置 Slot 属性
	FString SizeRule;
	if (Payload->TryGetStringField(TEXT("size_rule"), SizeRule))
	{
		if (SizeRule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
		{
			Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		else
		{
			Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
		}
	}
	
	const TSharedPtr<FJsonObject>* PaddingObj;
	if (Payload->TryGetObjectField(TEXT("padding"), PaddingObj))
	{
		double Left = 0, Top = 0, Right = 0, Bottom = 0;
		(*PaddingObj)->TryGetNumberField(TEXT("left"), Left);
		(*PaddingObj)->TryGetNumberField(TEXT("top"), Top);
		(*PaddingObj)->TryGetNumberField(TEXT("right"), Right);
		(*PaddingObj)->TryGetNumberField(TEXT("bottom"), Bottom);
		Slot->SetPadding(FMargin(Left, Top, Right, Bottom));
	}
	
	FString HAlign;
	if (Payload->TryGetStringField(TEXT("h_align"), HAlign))
	{
		if (HAlign.Contains(TEXT("Left"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Left);
		else if (HAlign.Contains(TEXT("Center"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
		else if (HAlign.Contains(TEXT("Right"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Right);
		else if (HAlign.Contains(TEXT("Fill"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
	}
	
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), NewWidget->GetName());
	Result->SetStringField(TEXT("class"), NewWidget->GetClass()->GetName());
	Result->SetStringField(TEXT("parent"), VBox->GetName());
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.add_to_vertical: type=%s, name=%s, parent=%s"), 
		*ControlType, *NewWidget->GetName(), *VBox->GetName());
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.add_to_vertical is only available in editor mode"));
#endif
}

void FUAL_WidgetCommands::Handle_AddToHorizontal(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 解析参数
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString ControlType;
	if (!Payload->TryGetStringField(TEXT("control_type"), ControlType))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: control_type"));
		return;
	}
	
	FString WidgetName;
	Payload->TryGetStringField(TEXT("name"), WidgetName);
	
	FString ParentName;
	Payload->TryGetStringField(TEXT("parent"), ParentName);
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	const FScopedTransaction Transaction(NSLOCTEXT("UAL", "AddToHorizontal", "Agent Add To HorizontalBox"));
	WidgetBP->Modify();
	
	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (!WidgetTree)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("WidgetBlueprint has no WidgetTree"));
		return;
	}
	
	UWidget* Parent = FindWidgetByName(WidgetTree, ParentName);
	if (!Parent)
	{
		Parent = WidgetTree->RootWidget;
	}
	
	UHorizontalBox* HBox = Cast<UHorizontalBox>(Parent);
	if (!HBox)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Parent '%s' is not a HorizontalBox"), 
			Parent ? *Parent->GetName() : TEXT("root")));
		return;
	}
	
	UClass* WidgetClass = FindWidgetClass(ControlType);
	if (!WidgetClass)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Unknown control type: %s"), *ControlType));
		return;
	}
	
	// 生成唯一名称（防止重名崩溃）
	FName UniqueName = NAME_None;
	if (!WidgetName.IsEmpty())
	{
		UniqueName = MakeUniqueWidgetName(WidgetTree, WidgetName);
	}
	
	UWidget* NewWidget = WidgetTree->ConstructWidget<UWidget>(WidgetClass, UniqueName);
	if (!NewWidget)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to construct widget"));
		return;
	}
	
	NewWidget->SetDesignerFlags(EWidgetDesignFlags::Designing);
	
	UHorizontalBoxSlot* Slot = Cast<UHorizontalBoxSlot>(HBox->AddChild(NewWidget));
	if (!Slot)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to HorizontalBox"));
		return;
	}
	
	// 设置 Slot 属性
	FString SizeRule;
	if (Payload->TryGetStringField(TEXT("size_rule"), SizeRule))
	{
		if (SizeRule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
		{
			Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		else
		{
			Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
		}
	}
	
	const TSharedPtr<FJsonObject>* PaddingObj;
	if (Payload->TryGetObjectField(TEXT("padding"), PaddingObj))
	{
		double Left = 0, Top = 0, Right = 0, Bottom = 0;
		(*PaddingObj)->TryGetNumberField(TEXT("left"), Left);
		(*PaddingObj)->TryGetNumberField(TEXT("top"), Top);
		(*PaddingObj)->TryGetNumberField(TEXT("right"), Right);
		(*PaddingObj)->TryGetNumberField(TEXT("bottom"), Bottom);
		Slot->SetPadding(FMargin(Left, Top, Right, Bottom));
	}
	
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), NewWidget->GetName());
	Result->SetStringField(TEXT("class"), NewWidget->GetClass()->GetName());
	Result->SetStringField(TEXT("parent"), HBox->GetName());
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.add_to_horizontal: type=%s, name=%s, parent=%s"), 
		*ControlType, *NewWidget->GetName(), *HBox->GetName());
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.add_to_horizontal is only available in editor mode"));
#endif
}

void FUAL_WidgetCommands::Handle_SetVerticalSlot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString WidgetName;
	if (!Payload->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: widget_name"));
		return;
	}
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	UWidget* Widget = FindWidgetByName(WidgetBP->WidgetTree, WidgetName);
	if (!Widget)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
		return;
	}
	
	UVerticalBoxSlot* Slot = Cast<UVerticalBoxSlot>(Widget->Slot);
	if (!Slot)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Widget '%s' is not in a VerticalBox"), *WidgetName));
		return;
	}
	
	const FScopedTransaction Transaction(NSLOCTEXT("UAL", "SetVerticalSlot", "Agent Set Vertical Slot"));
	WidgetBP->Modify();
	
	FString SizeRule;
	if (Payload->TryGetStringField(TEXT("size_rule"), SizeRule))
	{
		if (SizeRule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
		{
			Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		else
		{
			Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
		}
	}
	
	const TSharedPtr<FJsonObject>* PaddingObj;
	if (Payload->TryGetObjectField(TEXT("padding"), PaddingObj))
	{
		double Left = 0, Top = 0, Right = 0, Bottom = 0;
		(*PaddingObj)->TryGetNumberField(TEXT("left"), Left);
		(*PaddingObj)->TryGetNumberField(TEXT("top"), Top);
		(*PaddingObj)->TryGetNumberField(TEXT("right"), Right);
		(*PaddingObj)->TryGetNumberField(TEXT("bottom"), Bottom);
		Slot->SetPadding(FMargin(Left, Top, Right, Bottom));
	}
	
	FString HAlign;
	if (Payload->TryGetStringField(TEXT("h_align"), HAlign))
	{
		if (HAlign.Contains(TEXT("Left"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Left);
		else if (HAlign.Contains(TEXT("Center"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
		else if (HAlign.Contains(TEXT("Right"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Right);
		else if (HAlign.Contains(TEXT("Fill"))) Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
	}
	
	FString VAlign;
	if (Payload->TryGetStringField(TEXT("v_align"), VAlign))
	{
		if (VAlign.Contains(TEXT("Top"))) Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Top);
		else if (VAlign.Contains(TEXT("Center"))) Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Center);
		else if (VAlign.Contains(TEXT("Bottom"))) Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Bottom);
		else if (VAlign.Contains(TEXT("Fill"))) Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Fill);
	}
	
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("widget_name"), WidgetName);
	Result->SetObjectField(TEXT("slot_data"), BuildVerticalSlotJson(Slot));
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.set_vertical_slot: widget=%s"), *WidgetName);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.set_vertical_slot is only available in editor mode"));
#endif
}

// ============================================================================
// Phase 3: 通用子控件添加 - widget.add_child
// ============================================================================

void FUAL_WidgetCommands::Handle_AddChild(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString ParentName;
	if (!Payload->TryGetStringField(TEXT("parent_name"), ParentName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: parent_name"));
		return;
	}
	
	FString ControlType;
	if (!Payload->TryGetStringField(TEXT("control_type"), ControlType))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: control_type"));
		return;
	}
	
	FString WidgetName;
	Payload->TryGetStringField(TEXT("name"), WidgetName);
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
	if (!WidgetTree)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("WidgetBlueprint has no WidgetTree"));
		return;
	}
	
	// 查找父容器
	UWidget* Parent = nullptr;
	FString ParentType;
	
	if (ParentName.Equals(TEXT("root"), ESearchCase::IgnoreCase))
	{
		Parent = WidgetTree->RootWidget;
		if (!Parent)
		{
			UAL_CommandUtils::SendError(RequestId, 404, TEXT("Widget has no root"));
			return;
		}
		ParentType = Parent->GetClass()->GetName();
	}
	else
	{
		Parent = FindWidgetByName(WidgetTree, ParentName);
		if (!Parent)
		{
			UAL_CommandUtils::SendError(RequestId, 404, 
				FString::Printf(TEXT("Parent not found: %s"), *ParentName));
			return;
		}
		ParentType = Parent->GetClass()->GetName();
	}
	
	// 创建新控件
	UClass* WidgetClass = FindWidgetClass(ControlType);
	if (!WidgetClass)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Unknown control type: %s"), *ControlType));
		return;
	}
	
	// 生成唯一名称（防止重名崩溃）
	FName UniqueName = NAME_None;
	if (!WidgetName.IsEmpty())
	{
		UniqueName = MakeUniqueWidgetName(WidgetTree, WidgetName);
	}
	
	UWidget* NewWidget = WidgetTree->ConstructWidget<UWidget>(WidgetClass, UniqueName);
	if (!NewWidget)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to construct widget"));
		return;
	}
	
	NewWidget->SetDesignerFlags(EWidgetDesignFlags::Designing);
	
	// 根据父容器类型选择添加方式
	FString SlotType;
	TSharedPtr<FJsonObject> SlotData = MakeShared<FJsonObject>();
	
	// CanvasPanel
	if (UCanvasPanel* Canvas = Cast<UCanvasPanel>(Parent))
	{
		UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Canvas->AddChild(NewWidget));
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to CanvasPanel"));
			return;
		}
		
		// 设置 Canvas Slot 属性
		FString Anchors;
		if (Payload->TryGetStringField(TEXT("anchors"), Anchors))
		{
			Slot->SetAnchors(ParseAnchors(Anchors));
		}
		
		const TSharedPtr<FJsonObject>* PosObj;
		if (Payload->TryGetObjectField(TEXT("position"), PosObj))
		{
			double X = 0, Y = 0;
			(*PosObj)->TryGetNumberField(TEXT("x"), X);
			(*PosObj)->TryGetNumberField(TEXT("y"), Y);
			Slot->SetPosition(FVector2D(X, Y));
		}
		
		const TSharedPtr<FJsonObject>* SizeObj;
		if (Payload->TryGetObjectField(TEXT("size"), SizeObj))
		{
			double W = 100, H = 40;
			(*SizeObj)->TryGetNumberField(TEXT("width"), W);
			(*SizeObj)->TryGetNumberField(TEXT("height"), H);
			Slot->SetSize(FVector2D(W, H));
		}
		
		SlotType = TEXT("CanvasPanelSlot");
		SlotData = BuildCanvasSlotJson(Slot);
	}
	// VerticalBox
	else if (UVerticalBox* VBox = Cast<UVerticalBox>(Parent))
	{
		UVerticalBoxSlot* Slot = Cast<UVerticalBoxSlot>(VBox->AddChild(NewWidget));
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to VerticalBox"));
			return;
		}
		
		// 设置 Slot 属性
		FString SizeRule;
		if (Payload->TryGetStringField(TEXT("size_rule"), SizeRule))
		{
			if (SizeRule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
			{
				Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
			else
			{
				Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
			}
		}
		
		// 设置对齐
		FString HAlign;
		if (Payload->TryGetStringField(TEXT("h_align"), HAlign))
		{
			if (HAlign.Equals(TEXT("Left"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Left);
			else if (HAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
			else if (HAlign.Equals(TEXT("Right"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Right);
			else
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
		}
		
		FString VAlign;
		if (Payload->TryGetStringField(TEXT("v_align"), VAlign))
		{
			if (VAlign.Equals(TEXT("Top"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Top);
			else if (VAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Center);
			else if (VAlign.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Bottom);
			else
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Fill);
		}
		
		// 设置内边距
		const TSharedPtr<FJsonObject>* PaddingObj;
		if (Payload->TryGetObjectField(TEXT("padding"), PaddingObj))
		{
			double Left = 0, Top = 0, Right = 0, Bottom = 0;
			(*PaddingObj)->TryGetNumberField(TEXT("left"), Left);
			(*PaddingObj)->TryGetNumberField(TEXT("top"), Top);
			(*PaddingObj)->TryGetNumberField(TEXT("right"), Right);
			(*PaddingObj)->TryGetNumberField(TEXT("bottom"), Bottom);
			Slot->SetPadding(FMargin(Left, Top, Right, Bottom));
		}
		
		SlotType = TEXT("VerticalBoxSlot");
		SlotData = BuildVerticalSlotJson(Slot);
	}
	// HorizontalBox
	else if (UHorizontalBox* HBox = Cast<UHorizontalBox>(Parent))
	{
		UHorizontalBoxSlot* Slot = Cast<UHorizontalBoxSlot>(HBox->AddChild(NewWidget));
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to HorizontalBox"));
			return;
		}
		
		FString SizeRule;
		if (Payload->TryGetStringField(TEXT("size_rule"), SizeRule))
		{
			if (SizeRule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
			{
				Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			}
			else
			{
				Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
			}
		}
		
		const TSharedPtr<FJsonObject>* PaddingObj;
		if (Payload->TryGetObjectField(TEXT("padding"), PaddingObj))
		{
			double Left = 0, Top = 0, Right = 0, Bottom = 0;
			(*PaddingObj)->TryGetNumberField(TEXT("left"), Left);
			(*PaddingObj)->TryGetNumberField(TEXT("top"), Top);
			(*PaddingObj)->TryGetNumberField(TEXT("right"), Right);
			(*PaddingObj)->TryGetNumberField(TEXT("bottom"), Bottom);
			Slot->SetPadding(FMargin(Left, Top, Right, Bottom));
		}
		
		SlotType = TEXT("HorizontalBoxSlot");
	}
	// Overlay
	else if (UOverlay* Overlay = Cast<UOverlay>(Parent))
	{
		UOverlaySlot* Slot = Cast<UOverlaySlot>(Overlay->AddChild(NewWidget));
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to Overlay"));
			return;
		}
		
		// 设置对齐
		FString HAlign;
		if (Payload->TryGetStringField(TEXT("h_align"), HAlign))
		{
			if (HAlign.Equals(TEXT("Left"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Left);
			else if (HAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
			else if (HAlign.Equals(TEXT("Right"), ESearchCase::IgnoreCase))
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Right);
			else
				Slot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
		}
		
		FString VAlign;
		if (Payload->TryGetStringField(TEXT("v_align"), VAlign))
		{
			if (VAlign.Equals(TEXT("Top"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Top);
			else if (VAlign.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Center);
			else if (VAlign.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase))
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Bottom);
			else
				Slot->SetVerticalAlignment(EVerticalAlignment::VAlign_Fill);
		}
		
		const TSharedPtr<FJsonObject>* PaddingObj;
		if (Payload->TryGetObjectField(TEXT("padding"), PaddingObj))
		{
			double Left = 0, Top = 0, Right = 0, Bottom = 0;
			(*PaddingObj)->TryGetNumberField(TEXT("left"), Left);
			(*PaddingObj)->TryGetNumberField(TEXT("top"), Top);
			(*PaddingObj)->TryGetNumberField(TEXT("right"), Right);
			(*PaddingObj)->TryGetNumberField(TEXT("bottom"), Bottom);
			Slot->SetPadding(FMargin(Left, Top, Right, Bottom));
		}
		
		SlotType = TEXT("OverlaySlot");
	}
	// Button/Border/SizeBox 等单内容容器 (ContentWidget)
	else if (UContentWidget* ContentWidget = Cast<UContentWidget>(Parent))
	{
		// 如果已经有内容，移除旧内容
		if (ContentWidget->GetChildrenCount() > 0)
		{
			ContentWidget->ClearChildren();
		}
		
		UPanelSlot* Slot = ContentWidget->AddChild(NewWidget);
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to set content widget"));
			return;
		}
		
		SlotType = TEXT("ContentSlot");
	}
	else
	{
		// 尝试作为通用 PanelWidget 添加
		UPanelWidget* Panel = Cast<UPanelWidget>(Parent);
		if (!Panel)
		{
			UAL_CommandUtils::SendError(RequestId, 400, 
				FString::Printf(TEXT("Parent '%s' (%s) is not a container"), *ParentName, *ParentType));
			return;
		}
		
		UPanelSlot* Slot = Panel->AddChild(NewWidget);
		if (!Slot)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to add widget to panel"));
			return;
		}
		
		SlotType = TEXT("PanelSlot");
	}
	
	// 如果是 TextBlock，设置文本
	FString Text;
	if (Payload->TryGetStringField(TEXT("text"), Text))
	{
		UTextBlock* TextBlock = Cast<UTextBlock>(NewWidget);
		if (TextBlock)
		{
			TextBlock->SetText(FText::FromString(Text));
		}
	}
	
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), NewWidget->GetName());
	Result->SetStringField(TEXT("class"), NewWidget->GetClass()->GetName());
	Result->SetStringField(TEXT("parent"), Parent->GetName());
	Result->SetStringField(TEXT("parent_type"), ParentType);
	Result->SetStringField(TEXT("slot_type"), SlotType);
	if (SlotData->Values.Num() > 0)
	{
		Result->SetObjectField(TEXT("slot_data"), SlotData);
	}
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.add_child: type=%s, name=%s, parent=%s (%s)"), 
		*ControlType, *NewWidget->GetName(), *Parent->GetName(), *ParentType);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.add_child is only available in editor mode"));
#endif
}

// ============================================================================
// Phase 4: 预览渲染
// ============================================================================

void FUAL_WidgetCommands::Handle_Preview(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	int32 Width = 1920;
	int32 Height = 1080;
	Payload->TryGetNumberField(TEXT("width"), Width);
	Payload->TryGetNumberField(TEXT("height"), Height);
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	// 编译蓝图确保最新
	FKismetEditorUtilities::CompileBlueprint(WidgetBP);
	
	// 获取生成类
	UClass* WidgetClass = WidgetBP->GeneratedClass;
	if (!WidgetClass || !WidgetClass->IsChildOf(UUserWidget::StaticClass()))
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Widget Blueprint has no valid generated class"));
		return;
	}
	
	// 创建临时 Widget 实例
	UWorld* PreviewWorld = GEditor->GetEditorWorldContext().World();
	if (!PreviewWorld)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No editor world available for preview"));
		return;
	}
	
	UUserWidget* TempWidget = CreateWidget<UUserWidget>(PreviewWorld, WidgetClass);
	if (!TempWidget)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create widget instance for preview"));
		return;
	}
	
	// 强制布局计算
	TempWidget->ForceLayoutPrepass();
	
	// 使用 FWidgetRenderer 渲染到 RenderTarget
	FWidgetRenderer WidgetRenderer(true);
	
	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>();
	RenderTarget->InitAutoFormat(Width, Height);
	RenderTarget->UpdateResourceImmediate();
	
	WidgetRenderer.DrawWidget(RenderTarget, TempWidget->TakeWidget(), FVector2D(Width, Height), 0.0f);
	
	// 保存到文件
	FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots/UAL"));
	IFileManager::Get().MakeDirectory(*OutputDir, true);
	
	FString Filename = FString::Printf(TEXT("widget_preview_%s_%lld.png"), 
		*FPaths::GetBaseFilename(Path), FDateTime::Now().GetTicks());
	FString OutputPath = FPaths::Combine(OutputDir, Filename);
	
	// 读取像素数据
	TArray<FColor> Bitmap;
	FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (RTResource && RTResource->ReadPixels(Bitmap))
	{
		// 保存为 PNG
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		
		if (ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			const TArray64<uint8>& PNGData = ImageWrapper->GetCompressed();
			if (PNGData.Num() > 0)
			{
				FFileHelper::SaveArrayToFile(PNGData, *OutputPath);
			}
		}
	}
	
	// 清理临时实例
	TempWidget->RemoveFromParent();
	TempWidget->ConditionalBeginDestroy();
	
	// 返回结果
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("path"), OutputPath);
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("height"), Height);
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.preview: path=%s, output=%s"), *Path, *OutputPath);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.preview is only available in editor mode"));
#endif
}

// ============================================================================
// Phase 5: 事件绑定
// ============================================================================

void FUAL_WidgetCommands::Handle_MakeVariable(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString WidgetName;
	if (!Payload->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: widget_name"));
		return;
	}
	
	FString VariableName;
	if (!Payload->TryGetStringField(TEXT("variable_name"), VariableName))
	{
		VariableName = WidgetName; // 默认使用控件名
	}
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	UWidget* Widget = FindWidgetByName(WidgetBP->WidgetTree, WidgetName);
	if (!Widget)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
		return;
	}
	
	const FScopedTransaction Transaction(NSLOCTEXT("UAL", "MakeVariable", "Agent Make Variable"));
	WidgetBP->Modify();
	
	// 设置控件为变量
	Widget->bIsVariable = true;
	
	// 重命名控件（变量名）- 使用安全重命名防止重名崩溃
	if (VariableName != WidgetName)
	{
		FName SafeName = GenerateUniqueWidgetName(WidgetBP->WidgetTree, VariableName);
		Widget->Rename(*SafeName.ToString());
	}
	
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("widget_name"), Widget->GetName());
	Result->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.make_variable: widget=%s"), *Widget->GetName());
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.make_variable is only available in editor mode"));
#endif
}

void FUAL_WidgetCommands::Handle_SetProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	FString Path;
	if (!Payload->TryGetStringField(TEXT("path"), Path))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	
	FString WidgetName;
	if (!Payload->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: widget_name"));
		return;
	}
	
	FString PropertyName;
	if (!Payload->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: property_name"));
		return;
	}
	
	FString Error;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(Path, Error);
	if (!WidgetBP)
	{
		UAL_CommandUtils::SendError(RequestId, 404, Error);
		return;
	}
	
	UWidget* Widget = FindWidgetByName(WidgetBP->WidgetTree, WidgetName);
	if (!Widget)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
		return;
	}
	
	const FScopedTransaction Transaction(NSLOCTEXT("UAL", "SetProperty", "Agent Set Property"));
	WidgetBP->Modify();
	
	bool bSuccess = false;
	FString ResultMessage;
	
	// 处理常见属性（类型安全）
	if (PropertyName.Equals(TEXT("Text"), ESearchCase::IgnoreCase))
	{
		// TextBlock.Text
		FString TextValue;
		if (Payload->TryGetStringField(TEXT("value"), TextValue))
		{
			if (UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
			{
				TextBlock->SetText(FText::FromString(TextValue));
				bSuccess = true;
				ResultMessage = FString::Printf(TEXT("Set Text to: %s"), *TextValue);
			}
		}
	}
	else if (PropertyName.Equals(TEXT("Visibility"), ESearchCase::IgnoreCase))
	{
		FString VisValue;
		if (Payload->TryGetStringField(TEXT("value"), VisValue))
		{
			ESlateVisibility NewVis = ESlateVisibility::Visible;
			if (VisValue.Contains(TEXT("Hidden"))) NewVis = ESlateVisibility::Hidden;
			else if (VisValue.Contains(TEXT("Collapsed"))) NewVis = ESlateVisibility::Collapsed;
			else if (VisValue.Contains(TEXT("HitTestInvisible"))) NewVis = ESlateVisibility::HitTestInvisible;
			else if (VisValue.Contains(TEXT("SelfHitTestInvisible"))) NewVis = ESlateVisibility::SelfHitTestInvisible;
			
			Widget->SetVisibility(NewVis);
			bSuccess = true;
			ResultMessage = FString::Printf(TEXT("Set Visibility to: %s"), *VisValue);
		}
	}
	else if (PropertyName.Equals(TEXT("IsEnabled"), ESearchCase::IgnoreCase))
	{
		bool bEnabled = true;
		if (Payload->TryGetBoolField(TEXT("value"), bEnabled))
		{
			Widget->SetIsEnabled(bEnabled);
			bSuccess = true;
			ResultMessage = FString::Printf(TEXT("Set IsEnabled to: %s"), bEnabled ? TEXT("true") : TEXT("false"));
		}
	}
	else if (PropertyName.Equals(TEXT("ToolTipText"), ESearchCase::IgnoreCase))
	{
		FString TooltipValue;
		if (Payload->TryGetStringField(TEXT("value"), TooltipValue))
		{
			Widget->SetToolTipText(FText::FromString(TooltipValue));
			bSuccess = true;
			ResultMessage = FString::Printf(TEXT("Set ToolTipText to: %s"), *TooltipValue);
		}
	}
	else if (PropertyName.Equals(TEXT("Percent"), ESearchCase::IgnoreCase))
	{
		// ProgressBar.Percent
		double PercentValue = 0.0;
		if (Payload->TryGetNumberField(TEXT("value"), PercentValue))
		{
			if (UProgressBar* ProgressBar = Cast<UProgressBar>(Widget))
			{
				ProgressBar->SetPercent(PercentValue);
				bSuccess = true;
				ResultMessage = FString::Printf(TEXT("Set Percent to: %.2f"), PercentValue);
			}
		}
	}
	
	if (!bSuccess)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Failed to set property '%s' on widget '%s'. Property may not be supported or value type is incorrect."), 
			*PropertyName, *WidgetName));
		return;
	}
	
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("widget_name"), WidgetName);
	Result->SetStringField(TEXT("property_name"), PropertyName);
	Result->SetStringField(TEXT("message"), ResultMessage);
	
	UE_LOG(LogUALWidget, Log, TEXT("widget.set_property: widget=%s, property=%s"), *WidgetName, *PropertyName);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("widget.set_property is only available in editor mode"));
#endif
}

// ============================================================================
// 辅助函数
// ============================================================================

UWidgetBlueprint* FUAL_WidgetCommands::LoadWidgetBlueprint(const FString& Path, FString& OutError)
{
#if WITH_EDITOR
	// 尝试多种路径格式
	FString NormalizedPath = Path;
	if (!NormalizedPath.StartsWith(TEXT("/")))
	{
		NormalizedPath = TEXT("/") + NormalizedPath;
	}
	
	// 尝试直接加载
	UObject* LoadedObject = StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *NormalizedPath);
	if (LoadedObject)
	{
		return Cast<UWidgetBlueprint>(LoadedObject);
	}
	
	// 尝试添加 _C 后缀（BlueprintGeneratedClass）
	if (!NormalizedPath.EndsWith(TEXT("_C")))
	{
		FString ClassPath = NormalizedPath + TEXT("_C");
		LoadedObject = StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *ClassPath);
		if (LoadedObject)
		{
			return Cast<UWidgetBlueprint>(LoadedObject);
		}
	}
	
	// 尝试通过 Asset Registry 查找
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	FString PackagePath, AssetName;
	NormalizedPath.Split(TEXT("."), &PackagePath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (AssetName.IsEmpty())
	{
		// 从路径获取资产名
		int32 LastSlash;
		if (NormalizedPath.FindLastChar(TEXT('/'), LastSlash))
		{
			AssetName = NormalizedPath.Mid(LastSlash + 1);
			PackagePath = NormalizedPath.Left(LastSlash);
		}
	}
	
	TArray<FAssetData> AssetDataList;
	FARFilter Filter;
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
#else
	Filter.ClassNames.Add(UWidgetBlueprint::StaticClass()->GetFName());
#endif
	Filter.PackagePaths.Add(FName(*PackagePath));
	AssetRegistry.GetAssets(Filter, AssetDataList);
	
	for (const FAssetData& AssetData : AssetDataList)
	{
		if (AssetData.AssetName.ToString() == AssetName || AssetData.GetFullName().Contains(AssetName))
		{
			UObject* Asset = AssetData.GetAsset();
			if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Asset))
			{
				return WidgetBP;
			}
		}
	}
	
	OutError = FString::Printf(TEXT("Widget Blueprint not found: %s"), *Path);
	return nullptr;
#else
	OutError = TEXT("LoadWidgetBlueprint is only available in editor mode");
	return nullptr;
#endif
}

UWidget* FUAL_WidgetCommands::FindWidgetByName(UWidgetTree* WidgetTree, const FString& WidgetName)
{
	if (!WidgetTree)
	{
		return nullptr;
	}
	
	// 如果名称为空或 "root"，返回根控件
	if (WidgetName.IsEmpty() || WidgetName.Equals(TEXT("root"), ESearchCase::IgnoreCase))
	{
		return WidgetTree->RootWidget;
	}
	
	UWidget* FoundWidget = nullptr;
	WidgetTree->ForEachWidget([&FoundWidget, &WidgetName](UWidget* Widget)
	{
		if (Widget && Widget->GetName().Equals(WidgetName, ESearchCase::IgnoreCase))
		{
			FoundWidget = Widget;
		}
	});
	
	return FoundWidget;
}

UClass* FUAL_WidgetCommands::FindWidgetClass(const FString& ClassName)
{
	// 常见控件类型映射
	static TMap<FString, UClass*> WidgetClassMap;
	if (WidgetClassMap.Num() == 0)
	{
		WidgetClassMap.Add(TEXT("Button"), UButton::StaticClass());
		WidgetClassMap.Add(TEXT("Text"), UTextBlock::StaticClass());
		WidgetClassMap.Add(TEXT("TextBlock"), UTextBlock::StaticClass());
		WidgetClassMap.Add(TEXT("Image"), UImage::StaticClass());
		WidgetClassMap.Add(TEXT("CanvasPanel"), UCanvasPanel::StaticClass());
		WidgetClassMap.Add(TEXT("VerticalBox"), UVerticalBox::StaticClass());
		WidgetClassMap.Add(TEXT("HorizontalBox"), UHorizontalBox::StaticClass());
		WidgetClassMap.Add(TEXT("Overlay"), UOverlay::StaticClass());
		WidgetClassMap.Add(TEXT("Border"), UBorder::StaticClass());
		WidgetClassMap.Add(TEXT("ScrollBox"), UScrollBox::StaticClass());
		WidgetClassMap.Add(TEXT("SizeBox"), USizeBox::StaticClass());
		WidgetClassMap.Add(TEXT("Spacer"), USpacer::StaticClass());
		WidgetClassMap.Add(TEXT("ProgressBar"), UProgressBar::StaticClass());
		WidgetClassMap.Add(TEXT("Slider"), USlider::StaticClass());
		WidgetClassMap.Add(TEXT("CheckBox"), UCheckBox::StaticClass());
		WidgetClassMap.Add(TEXT("ComboBox"), UComboBoxString::StaticClass());
		WidgetClassMap.Add(TEXT("ComboBoxString"), UComboBoxString::StaticClass());
		WidgetClassMap.Add(TEXT("EditableText"), UEditableText::StaticClass());
		WidgetClassMap.Add(TEXT("EditableTextBox"), UEditableTextBox::StaticClass());
		WidgetClassMap.Add(TEXT("SpinBox"), USpinBox::StaticClass());
		WidgetClassMap.Add(TEXT("RichTextBlock"), URichTextBlock::StaticClass());
		WidgetClassMap.Add(TEXT("GridPanel"), UGridPanel::StaticClass());
		WidgetClassMap.Add(TEXT("WrapBox"), UWrapBox::StaticClass());
		WidgetClassMap.Add(TEXT("UniformGridPanel"), UUniformGridPanel::StaticClass());
	}
	
	UClass** FoundClass = WidgetClassMap.Find(ClassName);
	if (FoundClass)
	{
		return *FoundClass;
	}
	
	// 尝试动态查找
	FString FullClassName = FString::Printf(TEXT("U%s"), *ClassName);
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	UClass* DynamicClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::None);
#else
	UClass* DynamicClass = FindObject<UClass>(ANY_PACKAGE, *FullClassName);
#endif
	if (DynamicClass && DynamicClass->IsChildOf(UWidget::StaticClass()))
	{
		return DynamicClass;
	}
	
	return nullptr;
}

TSharedPtr<FJsonObject> FUAL_WidgetCommands::BuildWidgetJson(UWidget* Widget)
{
	if (!Widget)
	{
		return nullptr;
	}
	
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	
	// 基本信息
	Obj->SetStringField(TEXT("name"), Widget->GetName());
	Obj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
	Obj->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
	Obj->SetBoolField(TEXT("is_visible"), Widget->IsVisible());
	
	// Slot 信息
	if (UPanelSlot* Slot = Widget->Slot)
	{
		Obj->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());
		
		// 根据 Slot 类型导出具体属性
		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			Obj->SetObjectField(TEXT("slot_data"), BuildCanvasSlotJson(CanvasSlot));
		}
		else if (UVerticalBoxSlot* VSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			Obj->SetObjectField(TEXT("slot_data"), BuildVerticalSlotJson(VSlot));
		}
		else if (UHorizontalBoxSlot* HSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			Obj->SetObjectField(TEXT("slot_data"), BuildHorizontalSlotJson(HSlot));
		}
		else if (UOverlaySlot* OSlot = Cast<UOverlaySlot>(Slot))
		{
			// Overlay Slot
			TSharedPtr<FJsonObject> SlotData = MakeShared<FJsonObject>();
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
			SlotData->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(OSlot->GetHorizontalAlignment()));
			SlotData->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(OSlot->GetVerticalAlignment()));
#else
			SlotData->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(TEXT("EHorizontalAlignment"), OSlot->HorizontalAlignment));
			SlotData->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(TEXT("EVerticalAlignment"), OSlot->VerticalAlignment));
#endif
			Obj->SetObjectField(TEXT("slot_data"), SlotData);
		}
		else if (UGridSlot* GSlot = Cast<UGridSlot>(Slot))
		{
			// Grid Slot
			TSharedPtr<FJsonObject> SlotData = MakeShared<FJsonObject>();
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
			SlotData->SetNumberField(TEXT("row"), GSlot->GetRow());
			SlotData->SetNumberField(TEXT("column"), GSlot->GetColumn());
			SlotData->SetNumberField(TEXT("row_span"), GSlot->GetRowSpan());
			SlotData->SetNumberField(TEXT("column_span"), GSlot->GetColumnSpan());
#else
			SlotData->SetNumberField(TEXT("row"), GSlot->Row);
			SlotData->SetNumberField(TEXT("column"), GSlot->Column);
			SlotData->SetNumberField(TEXT("row_span"), GSlot->RowSpan);
			SlotData->SetNumberField(TEXT("column_span"), GSlot->ColumnSpan);
#endif
			Obj->SetObjectField(TEXT("slot_data"), SlotData);
		}
	}
	
	// 如果是容器控件，递归处理子控件
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		for (int32 i = 0; i < Panel->GetChildrenCount(); i++)
		{
			UWidget* Child = Panel->GetChildAt(i);
			TSharedPtr<FJsonObject> ChildJson = BuildWidgetJson(Child);
			if (ChildJson.IsValid())
			{
				Children.Add(MakeShared<FJsonValueObject>(ChildJson));
			}
		}
		
		if (Children.Num() > 0)
		{
			Obj->SetArrayField(TEXT("children"), Children);
		}
	}
	
	return Obj;
}

TSharedPtr<FJsonObject> FUAL_WidgetCommands::BuildCanvasSlotJson(UCanvasPanelSlot* Slot)
{
	if (!Slot)
	{
		return nullptr;
	}
	
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	
	// Anchors
	FAnchors Anchors = Slot->GetAnchors();
	TSharedPtr<FJsonObject> AnchorsObj = MakeShared<FJsonObject>();
	AnchorsObj->SetNumberField(TEXT("min_x"), Anchors.Minimum.X);
	AnchorsObj->SetNumberField(TEXT("min_y"), Anchors.Minimum.Y);
	AnchorsObj->SetNumberField(TEXT("max_x"), Anchors.Maximum.X);
	AnchorsObj->SetNumberField(TEXT("max_y"), Anchors.Maximum.Y);
	Obj->SetObjectField(TEXT("anchors"), AnchorsObj);
	
	// Offsets (Position/Size)
	FMargin Offsets = Slot->GetOffsets();
	TSharedPtr<FJsonObject> OffsetsObj = MakeShared<FJsonObject>();
	OffsetsObj->SetNumberField(TEXT("left"), Offsets.Left);
	OffsetsObj->SetNumberField(TEXT("top"), Offsets.Top);
	OffsetsObj->SetNumberField(TEXT("right"), Offsets.Right);
	OffsetsObj->SetNumberField(TEXT("bottom"), Offsets.Bottom);
	Obj->SetObjectField(TEXT("offsets"), OffsetsObj);
	
	// Position
	FVector2D Position = Slot->GetPosition();
	TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
	PosObj->SetNumberField(TEXT("x"), Position.X);
	PosObj->SetNumberField(TEXT("y"), Position.Y);
	Obj->SetObjectField(TEXT("position"), PosObj);
	
	// Size
	FVector2D Size = Slot->GetSize();
	TSharedPtr<FJsonObject> SizeObj = MakeShared<FJsonObject>();
	SizeObj->SetNumberField(TEXT("width"), Size.X);
	SizeObj->SetNumberField(TEXT("height"), Size.Y);
	Obj->SetObjectField(TEXT("size"), SizeObj);
	
	// Alignment
	FVector2D Alignment = Slot->GetAlignment();
	TSharedPtr<FJsonObject> AlignObj = MakeShared<FJsonObject>();
	AlignObj->SetNumberField(TEXT("x"), Alignment.X);
	AlignObj->SetNumberField(TEXT("y"), Alignment.Y);
	Obj->SetObjectField(TEXT("alignment"), AlignObj);
	
	// Auto Size
	Obj->SetBoolField(TEXT("auto_size"), Slot->GetAutoSize());
	
	// Z Order
	Obj->SetNumberField(TEXT("z_order"), Slot->GetZOrder());
	
	return Obj;
}

TSharedPtr<FJsonObject> FUAL_WidgetCommands::BuildVerticalSlotJson(UVerticalBoxSlot* Slot)
{
	if (!Slot)
	{
		return nullptr;
	}
	
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	
	// Padding
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	FMargin Padding = Slot->GetPadding();
#else
	FMargin Padding = Slot->Padding;
#endif
	TSharedPtr<FJsonObject> PaddingObj = MakeShared<FJsonObject>();
	PaddingObj->SetNumberField(TEXT("left"), Padding.Left);
	PaddingObj->SetNumberField(TEXT("top"), Padding.Top);
	PaddingObj->SetNumberField(TEXT("right"), Padding.Right);
	PaddingObj->SetNumberField(TEXT("bottom"), Padding.Bottom);
	Obj->SetObjectField(TEXT("padding"), PaddingObj);
	
	// Size
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	FSlateChildSize Size = Slot->GetSize();
	Obj->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(Slot->GetHorizontalAlignment()));
	Obj->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(Slot->GetVerticalAlignment()));
#else
	FSlateChildSize Size = Slot->Size;
	Obj->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(TEXT("EHorizontalAlignment"), Slot->HorizontalAlignment));
	Obj->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(TEXT("EVerticalAlignment"), Slot->VerticalAlignment));
#endif
	Obj->SetStringField(TEXT("size_rule"), Size.SizeRule == ESlateSizeRule::Automatic ? TEXT("Auto") : TEXT("Fill"));
	Obj->SetNumberField(TEXT("size_value"), Size.Value);
	
	return Obj;
}

TSharedPtr<FJsonObject> FUAL_WidgetCommands::BuildHorizontalSlotJson(UHorizontalBoxSlot* Slot)
{
	if (!Slot)
	{
		return nullptr;
	}
	
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	
	// Padding
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	FMargin Padding = Slot->GetPadding();
#else
	FMargin Padding = Slot->Padding;
#endif
	TSharedPtr<FJsonObject> PaddingObj = MakeShared<FJsonObject>();
	PaddingObj->SetNumberField(TEXT("left"), Padding.Left);
	PaddingObj->SetNumberField(TEXT("top"), Padding.Top);
	PaddingObj->SetNumberField(TEXT("right"), Padding.Right);
	PaddingObj->SetNumberField(TEXT("bottom"), Padding.Bottom);
	Obj->SetObjectField(TEXT("padding"), PaddingObj);
	
	// Size
#if ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 1
	FSlateChildSize Size = Slot->GetSize();
	Obj->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(Slot->GetHorizontalAlignment()));
	Obj->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(Slot->GetVerticalAlignment()));
#else
	FSlateChildSize Size = Slot->Size;
	Obj->SetStringField(TEXT("h_align"), UEnum::GetValueAsString(TEXT("EHorizontalAlignment"), Slot->HorizontalAlignment));
	Obj->SetStringField(TEXT("v_align"), UEnum::GetValueAsString(TEXT("EVerticalAlignment"), Slot->VerticalAlignment));
#endif
	Obj->SetStringField(TEXT("size_rule"), Size.SizeRule == ESlateSizeRule::Automatic ? TEXT("Auto") : TEXT("Fill"));
	Obj->SetNumberField(TEXT("size_value"), Size.Value);
	
	return Obj;
}

FAnchors FUAL_WidgetCommands::ParseAnchors(const FString& AnchorsStr)
{
	// 预设锚点映射
	if (AnchorsStr.Equals(TEXT("TopLeft"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.0f, 0.0f, 0.0f, 0.0f);
	}
	else if (AnchorsStr.Equals(TEXT("TopCenter"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.5f, 0.0f, 0.5f, 0.0f);
	}
	else if (AnchorsStr.Equals(TEXT("TopRight"), ESearchCase::IgnoreCase))
	{
		return FAnchors(1.0f, 0.0f, 1.0f, 0.0f);
	}
	else if (AnchorsStr.Equals(TEXT("CenterLeft"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.0f, 0.5f, 0.0f, 0.5f);
	}
	else if (AnchorsStr.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.5f, 0.5f, 0.5f, 0.5f);
	}
	else if (AnchorsStr.Equals(TEXT("CenterRight"), ESearchCase::IgnoreCase))
	{
		return FAnchors(1.0f, 0.5f, 1.0f, 0.5f);
	}
	else if (AnchorsStr.Equals(TEXT("BottomLeft"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.0f, 1.0f, 0.0f, 1.0f);
	}
	else if (AnchorsStr.Equals(TEXT("BottomCenter"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.5f, 1.0f, 0.5f, 1.0f);
	}
	else if (AnchorsStr.Equals(TEXT("BottomRight"), ESearchCase::IgnoreCase))
	{
		return FAnchors(1.0f, 1.0f, 1.0f, 1.0f);
	}
	else if (AnchorsStr.Equals(TEXT("Stretch"), ESearchCase::IgnoreCase) || 
	         AnchorsStr.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
	{
		return FAnchors(0.0f, 0.0f, 1.0f, 1.0f);
	}
	
	// 默认左上角
	return FAnchors(0.0f, 0.0f, 0.0f, 0.0f);
}

FVector2D FUAL_WidgetCommands::ParseVector2D(const TSharedPtr<FJsonObject>& Obj)
{
	if (!Obj.IsValid())
	{
		return FVector2D::ZeroVector;
	}
	
	double X = 0.0, Y = 0.0;
	
	// 支持 x/y 或 width/height
	if (!Obj->TryGetNumberField(TEXT("x"), X))
	{
		Obj->TryGetNumberField(TEXT("width"), X);
	}
	if (!Obj->TryGetNumberField(TEXT("y"), Y))
	{
		Obj->TryGetNumberField(TEXT("height"), Y);
	}
	
	return FVector2D(X, Y);
}

// ============================================================================
// 辅助函数: 唯一名称生成（防止重名崩溃）
// ============================================================================

FName FUAL_WidgetCommands::GenerateUniqueWidgetName(UWidgetTree* WidgetTree, const FString& DesiredName)
{
	if (!WidgetTree || DesiredName.IsEmpty())
	{
		return NAME_None;
	}
	
	// 先检查期望名称是否可用
	if (!WidgetTree->FindWidget(FName(*DesiredName)))
	{
		return FName(*DesiredName);
	}
	
	// 名称已存在，追加后缀递增
	UE_LOG(LogUALWidget, Warning, TEXT("Widget name '%s' already exists, generating unique name"), *DesiredName);
	
	for (int32 i = 1; i < 1000; i++)
	{
		FString Candidate = FString::Printf(TEXT("%s_%d"), *DesiredName, i);
		if (!WidgetTree->FindWidget(FName(*Candidate)))
		{
			UE_LOG(LogUALWidget, Log, TEXT("Using unique name: %s"), *Candidate);
			return FName(*Candidate);
		}
	}
	
	// 极端情况 fallback：使用 MakeUniqueObjectName
	return MakeUniqueObjectName(WidgetTree, UWidget::StaticClass(), FName(*DesiredName));
}
