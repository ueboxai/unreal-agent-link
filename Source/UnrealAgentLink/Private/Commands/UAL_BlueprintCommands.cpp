#include "UAL_BlueprintCommands.h"
#include "UAL_CommandUtils.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "AssetToolsModule.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "Engine/LevelScriptActor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_InputAction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Select.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "Engine/TimelineTemplate.h"
#include "Logging/TokenizedMessage.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#include "Logging/UObjectToken.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALBlueprint, Log, All);

void FUAL_BlueprintCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	// blueprint.describe - 获取蓝图完整结构信息
	CommandMap.Add(TEXT("blueprint.describe"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DescribeBlueprint(Payload, RequestId);
	});
	
	CommandMap.Add(TEXT("blueprint.create"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateBlueprint(Payload, RequestId);
	});
	
	CommandMap.Add(TEXT("blueprint.add_component"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddComponentToBlueprint(Payload, RequestId);
	});
	
	CommandMap.Add(TEXT("blueprint.set_property"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetBlueprintProperty(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.add_variable"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddVariableToBlueprint(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.get_graph"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetBlueprintGraph(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.add_node"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddNodeToBlueprint(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.add_timeline"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddTimelineToBlueprint(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.connect_pins"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ConnectBlueprintPins(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.create_function"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateFunctionGraph(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.compile"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CompileBlueprint(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.set_pin_value"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetPinValue(Payload, RequestId);
	});
}

// ============================================================================
// 图表/节点/变量辅助函数（仅本 CPP 内使用）
// ============================================================================

static FString UAL_GuidToString(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::DigitsWithHyphens);
}

static FString UAL_PinDirToString(const EEdGraphPinDirection Dir)
{
	return Dir == EGPD_Input ? TEXT("Input") : TEXT("Output");
}

static UEdGraph* UAL_FindGraph(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	// 默认：事件图
	if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
	{
		if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
		{
			return EventGraph;
		}
	}

	TArray<UEdGraph*> AllGraphs;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	FBlueprintEditorUtils::GetAllGraphs(Blueprint, AllGraphs);
#else
	// UE5.0: 手动收集所有图表
	AllGraphs.Append(Blueprint->UbergraphPages);
	AllGraphs.Append(Blueprint->FunctionGraphs);
	AllGraphs.Append(Blueprint->DelegateSignatureGraphs);
	AllGraphs.Append(Blueprint->MacroGraphs);
	if (Blueprint->IntermediateGeneratedGraphs.Num() > 0)
	{
		AllGraphs.Append(Blueprint->IntermediateGeneratedGraphs);
	}
#endif
	for (UEdGraph* G : AllGraphs)
	{
		if (G && G->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return G;
		}
	}
	return nullptr;
}

static UEdGraphNode* UAL_FindNodeByGuid(UEdGraph* Graph, const FString& NodeId)
{
	if (!Graph || NodeId.IsEmpty())
	{
		return nullptr;
	}
	FGuid Guid;
	if (!FGuid::Parse(NodeId, Guid))
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == Guid)
		{
			return Node;
		}
	}
	return nullptr;
}

static UEdGraphPin* UAL_FindPinByName(UEdGraphNode* Node, const FString& PinName)
{
	if (!Node || PinName.IsEmpty())
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}
		if (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}
	return nullptr;
}

static TArray<TSharedPtr<FJsonValue>> UAL_BuildPinsJson(UEdGraphNode* Node)
{
	TArray<TSharedPtr<FJsonValue>> Pins;
	if (!Node)
	{
		return Pins;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("dir"), UAL_PinDirToString(Pin->Direction));
		PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		PinObj->SetStringField(TEXT("sub_category"), Pin->PinType.PinSubCategory.ToString());
		// UE 5.0+: 使用 ContainerType
		PinObj->SetBoolField(TEXT("is_array"), Pin->PinType.ContainerType == EPinContainerType::Array);
		PinObj->SetBoolField(TEXT("is_reference"), Pin->PinType.bIsReference);
		PinObj->SetBoolField(TEXT("is_const"), Pin->PinType.bIsConst);

		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			if (const UObject* Obj = Pin->PinType.PinSubCategoryObject.Get())
			{
				PinObj->SetStringField(TEXT("sub_category_object"), Obj->GetPathName());
			}
		}

		// 友好名（UI 可能展示的 label），但 Agent 不应当用它做连线依据
		PinObj->SetStringField(TEXT("friendly_name"), Pin->PinFriendlyName.ToString());
		Pins.Add(MakeShared<FJsonValueObject>(PinObj));
	}
	return Pins;
}

static TSharedPtr<FJsonObject> UAL_BuildNodeJson(UEdGraphNode* Node)
{
	if (!Node)
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
	NodeObj->SetStringField(TEXT("node_id"), UAL_GuidToString(Node->NodeGuid));
	NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	NodeObj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
	NodeObj->SetNumberField(TEXT("pos_y"), Node->NodePosY);
	NodeObj->SetArrayField(TEXT("pins"), UAL_BuildPinsJson(Node));
	return NodeObj;
}

// ============================================================================
// Timeline 专用：创建/复用 TimelineTemplate 并在图表中放置 UK2Node_Timeline
// ============================================================================

static bool UAL_FindTimelineTemplate(UBlueprint* Blueprint, const FName TimelineName, UTimelineTemplate*& OutTemplate)
{
	OutTemplate = nullptr;
	if (!Blueprint)
	{
		return false;
	}
	// UBlueprint::Timelines：对象名通常就是 TimelineName
	for (UTimelineTemplate* Tpl : Blueprint->Timelines)
	{
		if (Tpl && Tpl->GetFName() == TimelineName)
		{
			OutTemplate = Tpl;
			return true;
		}
	}
	return false;
}

static UTimelineTemplate* UAL_CreateTimelineTemplate(UBlueprint* Blueprint, const FName TimelineName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	Blueprint->Modify();

	UTimelineTemplate* NewTpl = NewObject<UTimelineTemplate>(Blueprint, TimelineName, RF_Transactional);
	if (!NewTpl)
	{
		return nullptr;
	}
	NewTpl->SetFlags(RF_Transactional);
	NewTpl->Modify();

	// 尽量给一个稳定的 Guid（部分工具/重定向依赖它）
	// 不依赖字段名差异：TimelineGuid 在 UE5.0+ 都存在；若未来改动，此处可用宏隔离。
	NewTpl->TimelineGuid = FGuid::NewGuid();

	Blueprint->Timelines.Add(NewTpl);
	return NewTpl;
}

static bool UAL_ParseNodePosition(const TSharedPtr<FJsonObject>& Payload, int32& OutPosX, int32& OutPosY, bool& bOutHasExplicitPosition, bool& bOutForcePosition)
{
	bOutHasExplicitPosition = false;
	bOutForcePosition = false;
	OutPosX = 0;
	OutPosY = 0;
	if (!Payload.IsValid())
	{
		return false;
	}

	Payload->TryGetBoolField(TEXT("force_position"), bOutForcePosition);

	const TSharedPtr<FJsonObject>* PosObjPtr = nullptr;
	if (Payload->TryGetObjectField(TEXT("node_position"), PosObjPtr) && PosObjPtr && (*PosObjPtr).IsValid())
	{
		bOutHasExplicitPosition = true;
		(*PosObjPtr)->TryGetNumberField(TEXT("x"), OutPosX);
		(*PosObjPtr)->TryGetNumberField(TEXT("y"), OutPosY);
	}
	return true;
}

static void UAL_AutoLayoutIfNeeded(UEdGraph* Graph, bool& bHasExplicitPosition, bool bForcePosition, int32& PosX, int32& PosY)
{
	if (!Graph)
	{
		return;
	}

	// 兼容 add_node 的行为：显式给了 (0,0) 但没有 force_position=true 时，也按“未指定”处理，避免堆叠
	if (bHasExplicitPosition && !bForcePosition && PosX == 0 && PosY == 0)
	{
		bHasExplicitPosition = false;
	}

	if (bHasExplicitPosition)
	{
		return;
	}

	// 简化版智能排版：取主簇的 90% 分位 MaxX，放到右侧
	TArray<int32> Xs, Ys;
	Xs.Reserve(Graph->Nodes.Num());
	Ys.Reserve(Graph->Nodes.Num());
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		Xs.Add(Node->NodePosX);
		Ys.Add(Node->NodePosY);
	}

	auto Quantile = [](TArray<int32> Arr, float Q) -> int32
	{
		if (Arr.Num() == 0) return 0;
		Arr.Sort();
		const float IdxF = FMath::Clamp(Q, 0.0f, 1.0f) * float(Arr.Num() - 1);
		const int32 Lo = FMath::Clamp(int32(FMath::FloorToFloat(IdxF)), 0, Arr.Num() - 1);
		const int32 Hi = FMath::Clamp(int32(FMath::CeilToFloat(IdxF)), 0, Arr.Num() - 1);
		if (Lo == Hi) return Arr[Lo];
		const float Alpha = IdxF - float(Lo);
		return FMath::RoundToInt(FMath::Lerp(float(Arr[Lo]), float(Arr[Hi]), Alpha));
	};

	const int32 MaxX = (Xs.Num() > 0) ? Quantile(Xs, 0.90f) : 0;
	const int32 BaseY = (Ys.Num() > 0) ? Quantile(Ys, 0.10f) : 0;

	PosX = MaxX + 420;
	PosY = BaseY;
}

static bool UAL_LoadBlueprintByPathOrName(const FString& BlueprintPathOrName, UBlueprint*& OutBlueprint, FString& OutResolvedPath)
{
	OutBlueprint = nullptr;
	OutResolvedPath = BlueprintPathOrName;

	if (BlueprintPathOrName.IsEmpty())
	{
		return false;
	}

	// 1) 直接按路径加载
	if (BlueprintPathOrName.StartsWith(TEXT("/")))
	{
		FString ResolvedPath = BlueprintPathOrName;
		if (!ResolvedPath.Contains(TEXT(".")))
		{
			ResolvedPath = ResolvedPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPathOrName);
		}
		if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ResolvedPath))
		{
			OutBlueprint = BP;
			OutResolvedPath = ResolvedPath;
			return true;
		}
	}

	// 2) AssetRegistry 模糊查找（兼容传入短名）
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> AssetList;
	FARFilter Filter;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
	Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif
	Filter.bRecursiveClasses = true;
	AssetRegistry.GetAssets(Filter, AssetList);

	for (const FAssetData& Asset : AssetList)
	{
		FString AssetName = Asset.AssetName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		FString AssetPath = Asset.GetObjectPathString();
#else
		FString AssetPath = Asset.ObjectPath.ToString();
#endif
		if (AssetName.Equals(BlueprintPathOrName, ESearchCase::IgnoreCase) ||
			AssetPath.Contains(BlueprintPathOrName))
		{
			if (UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset()))
			{
				OutBlueprint = BP;
				OutResolvedPath = AssetPath;
				return true;
			}
		}
	}
	return false;
}

static bool UAL_ParsePinTypeFromString(const FString& TypeStr, const FString& ObjectClassStr, FEdGraphPinType& OutPinType, FString& OutError)
{
	OutError.Reset();
	OutPinType = FEdGraphPinType();
	OutPinType.PinCategory = NAME_None;
	OutPinType.PinSubCategory = NAME_None;
	OutPinType.PinSubCategoryObject = nullptr;

	const FString T = TypeStr.ToLower();
	auto SetStruct = [&](UScriptStruct* Struct)
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = Struct;
	};

	if (T == TEXT("bool") || T == TEXT("boolean"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return true;
	}
	if (T == TEXT("int") || T == TEXT("int32") || T == TEXT("integer"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		return true;
	}
	if (T == TEXT("int64"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		return true;
	}
	if (T == TEXT("float"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Float;
		return true;
	}
	if (T == TEXT("double"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Double;
		return true;
	}
	if (T == TEXT("string") || T == TEXT("str"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		return true;
	}
	if (T == TEXT("name"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		return true;
	}
	if (T == TEXT("text"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		return true;
	}
	if (T == TEXT("vector") || T == TEXT("fvector"))
	{
		SetStruct(TBaseStructure<FVector>::Get());
		return true;
	}
	if (T == TEXT("rotator") || T == TEXT("frotator"))
	{
		SetStruct(TBaseStructure<FRotator>::Get());
		return true;
	}
	if (T == TEXT("linearcolor") || T == TEXT("flinearcolor"))
	{
		SetStruct(TBaseStructure<FLinearColor>::Get());
		return true;
	}
	if (T == TEXT("color") || T == TEXT("fcolor"))
	{
		SetStruct(TBaseStructure<FColor>::Get());
		return true;
	}
	if (T == TEXT("object") || T == TEXT("class") || T == TEXT("soft_object") || T == TEXT("soft_class"))
	{
		FString ClsStr = ObjectClassStr;
		if (ClsStr.IsEmpty())
		{
			ClsStr = TEXT("/Script/Engine.Object");
		}
		FString ClassError;
		UClass* ObjClass = UAL_CommandUtils::ResolveClassFromIdentifier(ClsStr, UObject::StaticClass(), ClassError);
		if (!ObjClass)
		{
			OutError = ClassError;
			return false;
		}

		if (T == TEXT("class") || T == TEXT("soft_class"))
		{
			OutPinType.PinCategory = (T == TEXT("soft_class")) ? UEdGraphSchema_K2::PC_SoftClass : UEdGraphSchema_K2::PC_Class;
			OutPinType.PinSubCategoryObject = ObjClass;
		}
		else
		{
			OutPinType.PinCategory = (T == TEXT("soft_object")) ? UEdGraphSchema_K2::PC_SoftObject : UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = ObjClass;
		}
		return true;
	}

	OutError = FString::Printf(TEXT("Unsupported type: %s"), *TypeStr);
	return false;
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========
// 原始行号参考:
//   Handle_CreateBlueprint: 2279-2512

void FUAL_BlueprintCommands::Handle_CreateBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintName;
	if (!Payload->TryGetStringField(TEXT("name"), BlueprintName) || BlueprintName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: name"));
		return;
	}

	FString ParentClassStr;
	if (!Payload->TryGetStringField(TEXT("parent_class"), ParentClassStr))
	{
		Payload->TryGetStringField(TEXT("parentClass"), ParentClassStr);
	}
	if (ParentClassStr.IsEmpty())
	{
		ParentClassStr = TEXT("/Script/Engine.Actor");
	}

	FString PackagePath;
	if (!Payload->TryGetStringField(TEXT("path"), PackagePath))
	{
		FString Folder;
		if (!Payload->TryGetStringField(TEXT("folder"), Folder))
		{
			Folder = TEXT("/Game/UnrealAgent/Blueprints");
		}
		if (!Folder.StartsWith(TEXT("/")))
		{
			Folder = FString::Printf(TEXT("/Game/%s"), *Folder);
		}
		if (!Folder.EndsWith(TEXT("/")))
		{
			Folder += TEXT("/");
		}
		PackagePath = Folder + BlueprintName;
	}

	// 1. Resolve Parent Class
	FString ClassError;
	UClass* ParentClass = UAL_CommandUtils::ResolveClassFromIdentifier(ParentClassStr, AActor::StaticClass(), ClassError);
	if (!ParentClass)
	{
		UAL_CommandUtils::SendError(RequestId, 404, ClassError);
		return;
	}

	// 2. Check Package
	FString PackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(PackagePath, PackageName))
	{
		PackageName = PackagePath;
	}
	
	// 2.1 检查蓝图是否已存在（避免覆盖导致崩溃）
	FString ExistingAssetPath = PackageName + TEXT(".") + BlueprintName;
	if (UBlueprint* ExistingBlueprint = LoadObject<UBlueprint>(nullptr, *ExistingAssetPath))
	{
		// 蓝图已存在，返回冲突错误
		TSharedPtr<FJsonObject> ConflictResult = MakeShared<FJsonObject>();
		ConflictResult->SetBoolField(TEXT("ok"), false);
		ConflictResult->SetStringField(TEXT("name"), BlueprintName);
		ConflictResult->SetStringField(TEXT("path"), PackageName);
		ConflictResult->SetStringField(TEXT("existing_class"), ExistingBlueprint->GeneratedClass ? ExistingBlueprint->GeneratedClass->GetPathName() : TEXT(""));
		ConflictResult->SetStringField(TEXT("message"), FString::Printf(TEXT("Blueprint '%s' already exists at path '%s'. Use blueprint.add_component to modify it, or delete it first."), *BlueprintName, *PackageName));
		
		UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(TEXT(" '%s' 下已经存在同名蓝图 '%s'，请更换新的名字或路径。"), *PackageName,*BlueprintName ));
		return;
	}
	
	// 也检查 FindPackage 以防万一
	if (UPackage* ExistingPackage = FindPackage(nullptr, *PackageName))
	{
		if (UBlueprint* ExistingBP = FindObject<UBlueprint>(ExistingPackage, *BlueprintName))
		{
			UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(TEXT("Blueprint '%s' already exists in package '%s'"), *BlueprintName, *PackageName));
			return;
		}
	}
	
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create package"));
		return;
	}

	// 3. Create Blueprint
	EBlueprintType BlueprintType = BPTYPE_Normal;
	if (ParentClass->IsChildOf(UInterface::StaticClass()))
	{
		BlueprintType = BPTYPE_Interface;
	}
	else if (ParentClass->IsChildOf(ALevelScriptActor::StaticClass()))
	{
		BlueprintType = BPTYPE_LevelScript;
	}
	else if (ParentClass->IsChildOf(UFunction::StaticClass())) // MacroLibrary etc handled differently, keep simple
	{
		BlueprintType = BPTYPE_FunctionLibrary;
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*BlueprintName),
		BlueprintType,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);

	if (!Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create Blueprint asset"));
		return;
	}

	// 4. Add Components
	const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
	if (Payload->TryGetArrayField(TEXT("components"), Components) && Components)
	{
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (SCS)
		{
			for (const TSharedPtr<FJsonValue>& CompVal : *Components)
			{
				const TSharedPtr<FJsonObject> CompObj = CompVal->AsObject();
				if (!CompObj.IsValid()) continue;

				FString CompType, CompName, AttachTo;
				CompObj->TryGetStringField(TEXT("component_type"), CompType);
				CompObj->TryGetStringField(TEXT("component_name"), CompName);
				CompObj->TryGetStringField(TEXT("attach_to"), AttachTo);

				if (CompType.IsEmpty()) continue;

				FString CompError;
				UClass* CompClass = UAL_CommandUtils::ResolveClassFromIdentifier(CompType, USceneComponent::StaticClass(), CompError);
				if (!CompClass)
				{
					UE_LOG(LogUALBlueprint, Warning, TEXT("Component class not found: %s"), *CompType);
					continue;
				}

				USCS_Node* NewNode = SCS->CreateNode(CompClass, FName(*CompName));
				if (NewNode)
				{
					// Attach
					if (AttachTo.IsEmpty() || AttachTo.Equals(TEXT("root"), ESearchCase::IgnoreCase) || AttachTo.Equals(TEXT("DefaultSceneRoot"), ESearchCase::IgnoreCase))
					{
						SCS->AddNode(NewNode); // Default root or next to it
					}
					else
					{
						// Find parent
						USCS_Node* ParentNode = nullptr;
						for (USCS_Node* Node : SCS->GetAllNodes())
						{
							if (Node && Node->GetVariableName().ToString().Equals(AttachTo))
							{
								ParentNode = Node;
								break;
							}
						}
						if (ParentNode)
						{
							ParentNode->AddChildNode(NewNode);
						}
						else
						{
							SCS->AddNode(NewNode); // Fallback
						}
					}

					// Set Transform
					if (USceneComponent* Template = Cast<USceneComponent>(NewNode->ComponentTemplate))
					{
						const FVector Loc = UAL_CommandUtils::ReadVectorDirect(CompObj->GetObjectField(TEXT("location")));
						const FRotator Rot = UAL_CommandUtils::ReadRotatorDirect(CompObj->GetObjectField(TEXT("rotation")));
						const FVector Scale = UAL_CommandUtils::ReadVectorDirect(CompObj->GetObjectField(TEXT("scale")), FVector(1, 1, 1));
						
						Template->SetRelativeLocation(Loc);
						Template->SetRelativeRotation(Rot);
						Template->SetRelativeScale3D(Scale);

						// Set Properties
						const TSharedPtr<FJsonObject>* Props = nullptr;
						if (CompObj->TryGetObjectField(TEXT("properties"), Props) && Props && Props->IsValid())
						{
							for (auto& Pair : (*Props)->Values)
							{
								FString Key = Pair.Key;
								TSharedPtr<FJsonValue> Val = Pair.Value;
								
								FProperty* Prop = CompClass->FindPropertyByName(FName(*Key));
								if (Prop)
								{
									void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);
									// Basic types only for now
									if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
									{
										StrProp->SetPropertyValue(ValuePtr, Val->AsString());
									}
									else if (FDoubleProperty* DblProp = CastField<FDoubleProperty>(Prop))
									{
										DblProp->SetPropertyValue(ValuePtr, Val->AsNumber());
									}
									else if (FFloatProperty* FltProp = CastField<FFloatProperty>(Prop))
									{
										FltProp->SetPropertyValue(ValuePtr, (float)Val->AsNumber());
									}
									else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
									{
										BoolProp->SetPropertyValue(ValuePtr, Val->AsBool());
									}
									// Add more as needed
								}
							}
						}
					}
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	
	// Save
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs);

	// Asset Registry
	FAssetRegistryModule::AssetCreated(Blueprint);

	// 使用统一的结构构建函数，返回完整蓝图信息
	TSharedPtr<FJsonObject> Result = BuildBlueprintStructureJson(Blueprint, true, false);
	Result->SetBoolField(TEXT("saved"), true);
	Result->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>()); // Placeholder

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 为已存在的蓝图添加组件
 * 
 * 请求参数:
 *   - blueprint_name: 蓝图名称或路径（必填）
 *   - component_type: 组件类型（必填），如 StaticMeshComponent, PointLightComponent
 *   - component_name: 组件名称（必填）
 *   - location: 组件相对位置 [x, y, z]
 *   - rotation: 组件相对旋转 [pitch, yaw, roll]
 *   - scale: 组件相对缩放 [x, y, z]
 *   - attach_to: 附加到的父组件名称
 *   - component_properties: 组件属性键值对
 * 
 * 返回:
 *   - ok: 是否成功
 *   - blueprint_name: 蓝图名称
 *   - component_name: 添加的组件名称
 *   - component_class: 组件类型
 *   - attached: 是否已附加
 *   - saved: 是否已保存
 */
void FUAL_BlueprintCommands::Handle_AddComponentToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString BlueprintName;
	if (!Payload->TryGetStringField(TEXT("blueprint_name"), BlueprintName) || BlueprintName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_name"));
		return;
	}

	FString ComponentType;
	if (!Payload->TryGetStringField(TEXT("component_type"), ComponentType) || ComponentType.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: component_type"));
		return;
	}

	FString ComponentName;
	if (!Payload->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: component_name"));
		return;
	}

	// 2. 查找蓝图资产
	UBlueprint* Blueprint = nullptr;
	FString BlueprintPath;
	
	// 尝试直接加载（如果是完整路径）
	if (BlueprintName.StartsWith(TEXT("/")))
	{
		BlueprintPath = BlueprintName;
		if (!BlueprintPath.EndsWith(TEXT(".") + FPaths::GetBaseFilename(BlueprintPath)))
		{
			// 添加资产名称后缀
			BlueprintPath = BlueprintPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPath);
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	}
	
	// 如果直接加载失败，通过 AssetRegistry 查找
	if (!Blueprint)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		
		TArray<FAssetData> AssetList;
		FARFilter Filter;
		// UE 5.1+ 使用 ClassPaths，旧版本使用 ClassNames
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
		Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, AssetList);
		
		for (const FAssetData& Asset : AssetList)
		{
			FString AssetName = Asset.AssetName.ToString();
			// UE 5.1+ 使用 GetObjectPathString()，旧版本使用 ObjectPath.ToString()
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			FString AssetPath = Asset.GetObjectPathString();
#else
			FString AssetPath = Asset.ObjectPath.ToString();
#endif
			if (AssetName.Equals(BlueprintName, ESearchCase::IgnoreCase) ||
				AssetPath.Contains(BlueprintName))
			{
				Blueprint = Cast<UBlueprint>(Asset.GetAsset());
				BlueprintPath = AssetPath;
				break;
			}
		}
	}

	if (!Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
		return;
	}

	// 3. 解析组件类
	FString ClassError;
	UClass* ComponentClass = UAL_CommandUtils::ResolveClassFromIdentifier(ComponentType, UActorComponent::StaticClass(), ClassError);
	if (!ComponentClass)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Component class not found: %s. %s"), *ComponentType, *ClassError));
		return;
	}

	// 4. 获取 SimpleConstructionScript
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Blueprint does not have SimpleConstructionScript (not an Actor Blueprint?)"));
		return;
	}

	// 5. 检查组件名称是否已存在
	for (USCS_Node* ExistingNode : SCS->GetAllNodes())
	{
		if (ExistingNode && ExistingNode->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
		{
			UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(TEXT("Component with name '%s' already exists in blueprint"), *ComponentName));
			return;
		}
	}

	// 6. 创建组件节点
	USCS_Node* NewNode = SCS->CreateNode(ComponentClass, FName(*ComponentName));
	if (!NewNode)
	{
		UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(TEXT("Failed to create component node: %s"), *ComponentName));
		return;
	}

	// 7. 处理附加关系
	FString AttachTo;
	Payload->TryGetStringField(TEXT("attach_to"), AttachTo);
	
	bool bAttached = false;
	if (AttachTo.IsEmpty() || AttachTo.Equals(TEXT("root"), ESearchCase::IgnoreCase) || AttachTo.Equals(TEXT("DefaultSceneRoot"), ESearchCase::IgnoreCase))
	{
		SCS->AddNode(NewNode);
		bAttached = true;
	}
	else
	{
		// 查找父组件
		USCS_Node* ParentNode = nullptr;
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (Node && Node->GetVariableName().ToString().Equals(AttachTo, ESearchCase::IgnoreCase))
			{
				ParentNode = Node;
				break;
			}
		}
		
		if (ParentNode)
		{
			ParentNode->AddChildNode(NewNode);
			bAttached = true;
		}
		else
		{
			// 父组件未找到，添加到根节点
			SCS->AddNode(NewNode);
			bAttached = true;
			UE_LOG(LogUALBlueprint, Warning, TEXT("Parent component '%s' not found, attaching to root instead"), *AttachTo);
		}
	}

	// 8. 设置组件变换（仅对 SceneComponent）
	if (USceneComponent* SceneTemplate = Cast<USceneComponent>(NewNode->ComponentTemplate))
	{
		// 读取位置
		TSharedPtr<FJsonObject> LocationObj;
		if (UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("location"), LocationObj))
		{
			FVector Location = UAL_CommandUtils::ReadVectorDirect(LocationObj);
			SceneTemplate->SetRelativeLocation(Location);
		}
		
		// 读取旋转
		TSharedPtr<FJsonObject> RotationObj;
		if (UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("rotation"), RotationObj))
		{
			FRotator Rotation = UAL_CommandUtils::ReadRotatorDirect(RotationObj);
			SceneTemplate->SetRelativeRotation(Rotation);
		}
		
		// 读取缩放
		TSharedPtr<FJsonObject> ScaleObj;
		if (UAL_CommandUtils::TryGetObjectFieldFlexible(Payload, TEXT("scale"), ScaleObj))
		{
			FVector Scale = UAL_CommandUtils::ReadVectorDirect(ScaleObj, FVector(1, 1, 1));
			SceneTemplate->SetRelativeScale3D(Scale);
		}
	}

	// 9. 设置组件属性
	const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
	if (Payload->TryGetObjectField(TEXT("component_properties"), PropsPtr) && PropsPtr && PropsPtr->IsValid())
	{
		UObject* Template = NewNode->ComponentTemplate;
		if (Template)
		{
			for (auto& Pair : (*PropsPtr)->Values)
			{
				FString PropError;
				if (!UAL_CommandUtils::SetSimpleProperty(
					ComponentClass->FindPropertyByName(FName(*Pair.Key)),
					Template,
					Pair.Value,
					PropError))
				{
					UE_LOG(LogUALBlueprint, Warning, TEXT("Failed to set property '%s': %s"), *Pair.Key, *PropError);
				}
			}
		}
	}

	// 10. 标记蓝图已修改并保存
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	
	UPackage* Package = Blueprint->GetOutermost();
	bool bSaved = false;
	if (Package)
	{
		const FString PackageName = Package->GetName();
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		// 使用兼容写法，SavePackage 在不同版本返回值类型不同
		UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs);
		bSaved = true; // 如果没有异常则认为保存成功
	}

	// 11. 构建响应
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("blueprint_path"), BlueprintPath);
	Result->SetStringField(TEXT("component_name"), NewNode->GetVariableName().ToString());
	Result->SetStringField(TEXT("component_class"), ComponentClass->GetName());
	Result->SetBoolField(TEXT("attached"), bAttached);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Successfully added component '%s' (%s) to blueprint '%s'"), 
		*ComponentName, *ComponentClass->GetName(), *Blueprint->GetName()));
	
	// 返回更新后的完整组件列表，让 AI 知道当前蓝图有哪些组件
	Result->SetArrayField(TEXT("all_components"), CollectComponentsInfo(Blueprint));

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 设置蓝图属性（支持 CDO 默认值和 SCS 组件属性）
 * 
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - component_name: 组件名称（可选，为空则修改蓝图默认值 CDO，否则修改指定组件）
 *   - properties: 属性键值对（必填）
 *   - auto_compile: 是否自动编译（可选，默认 true）
 * 
 * 返回:
 *   - ok: 是否成功
 *   - blueprint_path: 蓝图路径
 *   - target_type: 修改的目标类型（"cdo" 或 "component"）
 *   - component_name: 组件名称（仅当修改组件时）
 *   - modified_properties: 成功修改的属性列表
 *   - failed_properties: 修改失败的属性列表（含错误信息）
 *   - compiled: 是否已编译
 *   - saved: 是否已保存
 */
void FUAL_BlueprintCommands::Handle_SetBlueprintProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数 blueprint_path
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	// 2. 解析 properties
	const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
	if (!Payload->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !(*PropertiesPtr).IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: properties"));
		return;
	}
	const TSharedPtr<FJsonObject>& Properties = *PropertiesPtr;

	// 3. 解析可选参数
	FString ComponentName;
	Payload->TryGetStringField(TEXT("component_name"), ComponentName);
	
	bool bAutoCompile = true;
	if (Payload->HasField(TEXT("auto_compile")))
	{
		bAutoCompile = Payload->GetBoolField(TEXT("auto_compile"));
	}

	// 4. 加载蓝图资产
	UBlueprint* Blueprint = nullptr;
	
	// 尝试直接加载
	if (BlueprintPath.StartsWith(TEXT("/")))
	{
		FString FullPath = BlueprintPath;
		if (!FullPath.Contains(TEXT(".")))
		{
			FullPath = FullPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPath);
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *FullPath);
	}
	
	// 如果直接加载失败，通过 AssetRegistry 查找
	if (!Blueprint)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		
		TArray<FAssetData> AssetList;
		FARFilter Filter;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
		Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, AssetList);
		
		for (const FAssetData& Asset : AssetList)
		{
			FString AssetName = Asset.AssetName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			FString AssetPath = Asset.GetObjectPathString();
#else
			FString AssetPath = Asset.ObjectPath.ToString();
#endif
			if (AssetName.Equals(BlueprintPath, ESearchCase::IgnoreCase) ||
				AssetPath.Contains(BlueprintPath))
			{
				Blueprint = Cast<UBlueprint>(Asset.GetAsset());
				break;
			}
		}
	}

	if (!Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	// 5. 确定目标对象（CDO 或 SCS 组件）
	UObject* TargetObject = nullptr;
	FString TargetType;
	UClass* TargetClass = nullptr;

	if (ComponentName.IsEmpty())
	{
		// 修改蓝图默认值（CDO）
		if (!Blueprint->GeneratedClass)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Blueprint has no generated class, please compile it first"));
			return;
		}
		TargetObject = Blueprint->GeneratedClass->GetDefaultObject();
		TargetClass = Blueprint->GeneratedClass;
		TargetType = TEXT("cdo");
		UE_LOG(LogUALBlueprint, Log, TEXT("[blueprint.set_property] Target: CDO of %s"), *Blueprint->GetName());
	}
	else
	{
		// 修改 SCS 组件属性
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (SCS)
		{
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (Node && Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					TargetObject = Node->ComponentTemplate;
					TargetClass = Node->ComponentClass;
					break;
				}
			}
		}
		
		// Fallback: 尝试在 CDO 中查找同名子对象（针对 C++ 继承的组件）
		if (!TargetObject && Blueprint->GeneratedClass)
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
			if (CDO)
			{
				TArray<UObject*> SubObjects;
				GetObjectsWithOuter(CDO, SubObjects, false);
				for (UObject* SubObj : SubObjects)
				{
					if (SubObj && SubObj->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
					{
						TargetObject = SubObj;
						TargetClass = SubObj->GetClass();
						break;
					}
				}
			}
		}
		
		if (!TargetObject)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Component '%s' not found in blueprint '%s'"), *ComponentName, *Blueprint->GetName()));
			return;
		}
		TargetType = TEXT("component");
		UE_LOG(LogUALBlueprint, Log, TEXT("[blueprint.set_property] Target: Component '%s' in %s"), *ComponentName, *Blueprint->GetName());
	}

	// 6. 应用属性
	TArray<TSharedPtr<FJsonValue>> ModifiedPropsArray;
	TArray<TSharedPtr<FJsonValue>> FailedPropsArray;

	for (auto& Pair : Properties->Values)
	{
		const FString& PropName = Pair.Key;
		const TSharedPtr<FJsonValue>& PropValue = Pair.Value;
		
		// 查找属性
		FProperty* Prop = TargetClass ? TargetClass->FindPropertyByName(FName(*PropName)) : nullptr;
		if (!Prop && TargetObject)
		{
			Prop = TargetObject->GetClass()->FindPropertyByName(FName(*PropName));
		}
		
		if (!Prop)
		{
			TSharedPtr<FJsonObject> FailInfo = MakeShared<FJsonObject>();
			FailInfo->SetStringField(TEXT("property"), PropName);
			FailInfo->SetStringField(TEXT("error"), TEXT("Property not found"));
			
			// 提供建议
			TArray<FString> AllProps;
			UAL_CommandUtils::CollectPropertyNames(TargetObject, AllProps);
			TArray<FString> Suggestions;
			UAL_CommandUtils::SuggestProperties(PropName, AllProps, Suggestions, 3);
			if (Suggestions.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> SuggestArr;
				for (const FString& Sug : Suggestions)
				{
					SuggestArr.Add(MakeShared<FJsonValueString>(Sug));
				}
				FailInfo->SetArrayField(TEXT("suggestions"), SuggestArr);
			}
			
			FailedPropsArray.Add(MakeShared<FJsonValueObject>(FailInfo));
			continue;
		}

		// 设置属性值
		FString PropError;
		bool bSuccess = UAL_CommandUtils::SetSimpleProperty(Prop, TargetObject, PropValue, PropError);
		
		if (bSuccess)
		{
			TSharedPtr<FJsonObject> ModInfo = MakeShared<FJsonObject>();
			ModInfo->SetStringField(TEXT("property"), PropName);
			ModInfo->SetStringField(TEXT("type"), Prop->GetClass()->GetName());
			ModifiedPropsArray.Add(MakeShared<FJsonValueObject>(ModInfo));
			UE_LOG(LogUALBlueprint, Log, TEXT("[blueprint.set_property] Set '%s' successfully"), *PropName);
		}
		else
		{
			TSharedPtr<FJsonObject> FailInfo = MakeShared<FJsonObject>();
			FailInfo->SetStringField(TEXT("property"), PropName);
			FailInfo->SetStringField(TEXT("error"), PropError.IsEmpty() ? TEXT("Failed to set property") : PropError);
			FailedPropsArray.Add(MakeShared<FJsonValueObject>(FailInfo));
			UE_LOG(LogUALBlueprint, Warning, TEXT("[blueprint.set_property] Failed to set '%s': %s"), *PropName, *PropError);
		}
	}

	// 7. 标记蓝图已修改
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// 8. 编译蓝图（如果需要）
	bool bCompiled = false;
	if (bAutoCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		bCompiled = true;
		UE_LOG(LogUALBlueprint, Log, TEXT("[blueprint.set_property] Blueprint compiled"));
	}

	// 9. 保存蓝图
	bool bSaved = false;
	UPackage* Package = Blueprint->GetOutermost();
	if (Package)
	{
		const FString PackageName = Package->GetName();
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs);
		bSaved = true;
	}

	// 10. 构建响应
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), FailedPropsArray.Num() == 0 || ModifiedPropsArray.Num() > 0);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
	Result->SetStringField(TEXT("target_type"), TargetType);
	
	if (!ComponentName.IsEmpty())
	{
		Result->SetStringField(TEXT("component_name"), ComponentName);
	}
	
	Result->SetArrayField(TEXT("modified_properties"), ModifiedPropsArray);
	Result->SetArrayField(TEXT("failed_properties"), FailedPropsArray);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetBoolField(TEXT("saved"), bSaved);
	
	// 生成消息
	FString Message;
	if (ModifiedPropsArray.Num() > 0 && FailedPropsArray.Num() == 0)
	{
		Message = FString::Printf(TEXT("Successfully set %d properties on %s '%s'"), 
			ModifiedPropsArray.Num(), *TargetType, ComponentName.IsEmpty() ? *Blueprint->GetName() : *ComponentName);
	}
	else if (ModifiedPropsArray.Num() > 0 && FailedPropsArray.Num() > 0)
	{
		Message = FString::Printf(TEXT("Partially set properties: %d succeeded, %d failed"), 
			ModifiedPropsArray.Num(), FailedPropsArray.Num());
	}
	else
	{
		Message = FString::Printf(TEXT("Failed to set any properties"));
	}
	Result->SetStringField(TEXT("message"), Message);

	int32 Code = (FailedPropsArray.Num() == 0) ? 200 : (ModifiedPropsArray.Num() > 0 ? 207 : 400);
	UAL_CommandUtils::SendResponse(RequestId, Code, Result);
}

/**
 * 添加蓝图成员变量
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - name: 变量名（必填）
 *   - type: 变量类型（必填），如 bool/int/float/string/name/text/vector/rotator/object
 *   - object_class: 当 type=object/class/soft_object/soft_class 时指定类（可选）
 *   - is_array: 是否数组（可选，默认 false）
 *   - default_value: 默认值（可选，字符串形式，走 BlueprintEditorUtils 解析）
 *
 * 响应:
 *   - ok: 是否成功
 *   - blueprint_path: 蓝图路径
 *   - variable: 变量信息
 */
void FUAL_BlueprintCommands::Handle_AddVariableToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString VarNameStr;
	if (!Payload->TryGetStringField(TEXT("name"), VarNameStr) || VarNameStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: name"));
		return;
	}

	FString TypeStr;
	if (!Payload->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: type"));
		return;
	}

	bool bIsArray = false;
	Payload->TryGetBoolField(TEXT("is_array"), bIsArray);

	FString DefaultValueStr;
	Payload->TryGetStringField(TEXT("default_value"), DefaultValueStr);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	const FName VarName(*VarNameStr);

	// 避免重名
	if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) != INDEX_NONE)
	{
		UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(TEXT("Variable already exists: %s"), *VarNameStr));
		return;
	}

	// 解析类型 -> PinType
	FEdGraphPinType PinType;
	PinType.PinCategory = NAME_None;
	PinType.PinSubCategory = NAME_None;
	PinType.PinSubCategoryObject = nullptr;

	const FString TypeLower = TypeStr.ToLower();
	auto SetStruct = [&](UScriptStruct* Struct)
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = Struct;
	};

	if (TypeLower == TEXT("bool"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (TypeLower == TEXT("int") || TypeLower == TEXT("int32"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (TypeLower == TEXT("int64"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
	}
	else if (TypeLower == TEXT("float"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
	}
	else if (TypeLower == TEXT("double"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Double;
	}
	else if (TypeLower == TEXT("string"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (TypeLower == TEXT("name"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (TypeLower == TEXT("text"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	else if (TypeLower == TEXT("vector"))
	{
		SetStruct(TBaseStructure<FVector>::Get());
	}
	else if (TypeLower == TEXT("rotator"))
	{
		SetStruct(TBaseStructure<FRotator>::Get());
	}
	else if (TypeLower == TEXT("linearcolor") || TypeLower == TEXT("flinearcolor"))
	{
		SetStruct(TBaseStructure<FLinearColor>::Get());
	}
	else if (TypeLower == TEXT("color") || TypeLower == TEXT("fcolor"))
	{
		SetStruct(TBaseStructure<FColor>::Get());
	}
	else if (TypeLower == TEXT("object") || TypeLower == TEXT("class") || TypeLower == TEXT("soft_object") || TypeLower == TEXT("soft_class"))
	{
		FString ObjClassStr;
		Payload->TryGetStringField(TEXT("object_class"), ObjClassStr);
		if (ObjClassStr.IsEmpty())
		{
			ObjClassStr = TEXT("/Script/Engine.Object");
		}
		FString ClassError;
		UClass* ObjClass = UAL_CommandUtils::ResolveClassFromIdentifier(ObjClassStr, UObject::StaticClass(), ClassError);
		if (!ObjClass)
		{
			UAL_CommandUtils::SendError(RequestId, 404, ClassError);
			return;
		}

		if (TypeLower == TEXT("class") || TypeLower == TEXT("soft_class"))
		{
			PinType.PinCategory = (TypeLower == TEXT("soft_class")) ? UEdGraphSchema_K2::PC_SoftClass : UEdGraphSchema_K2::PC_Class;
			PinType.PinSubCategoryObject = ObjClass;
		}
		else
		{
			PinType.PinCategory = (TypeLower == TEXT("soft_object")) ? UEdGraphSchema_K2::PC_SoftObject : UEdGraphSchema_K2::PC_Object;
			PinType.PinSubCategoryObject = ObjClass;
		}
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(TEXT("Unsupported variable type: %s"), *TypeStr));
		return;
	}

	// UE 5.0+: 使用 ContainerType
	if (bIsArray)
	{
		PinType.ContainerType = EPinContainerType::Array;
	}

	FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarName, PinType);

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	if (!DefaultValueStr.IsEmpty())
	{
		FBlueprintEditorUtils::SetBlueprintVariableDefaultValue(Blueprint, VarName, DefaultValueStr);
	}
#else
	// UE5.0: SetBlueprintVariableDefaultValue 不存在，需要通过其他方式设置默认值
	// TODO: 可以尝试直接设置 CDO 的属性
	if (!DefaultValueStr.IsEmpty())
	{
		UE_LOG(LogUALBlueprint, Warning, TEXT("Setting default value is not fully supported in UE5.0"));
	}
#endif

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
	VarObj->SetStringField(TEXT("name"), VarNameStr);
	VarObj->SetStringField(TEXT("type"), PinType.PinCategory.ToString());
	VarObj->SetBoolField(TEXT("is_array"), bIsArray);
	if (PinType.PinSubCategoryObject.IsValid())
	{
		if (const UObject* Obj = PinType.PinSubCategoryObject.Get())
		{
			VarObj->SetStringField(TEXT("sub_category_object"), Obj->GetPathName());
		}
	}
	if (!DefaultValueStr.IsEmpty())
	{
		VarObj->SetStringField(TEXT("default_value"), DefaultValueStr);
	}
	Result->SetObjectField(TEXT("variable"), VarObj);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 获取蓝图图表信息（节点、引脚元数据）
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - graph_name: 图表名（可选，默认 EventGraph）
 *
 * 响应:
 *   - ok
 *   - blueprint_path
 *   - graph_name
 *   - nodes: [{ node_id, class, title, pos_x, pos_y, pins:[...] }]
 */
void FUAL_BlueprintCommands::Handle_GetBlueprintGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		if (TSharedPtr<FJsonObject> NodeObj = UAL_BuildNodeJson(Node))
		{
			Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetArrayField(TEXT("nodes"), Nodes);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 在蓝图图表中添加节点
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - graph_name: 图表名（可选，默认 EventGraph）
 *   - node_type: Function/Event/VariableGet/VariableSet（必填）
 *   - node_name: 节点名（必填）
 *   - node_position: {x,y}（可选，默认 0,0）
 *
 * 响应:
 *   - ok, node_id, pins[]
 */
void FUAL_BlueprintCommands::Handle_AddNodeToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// ===== 批量模式检测 =====
	// 如果存在 nodes 数组，则进入批量创建模式
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray && NodesArray->Num() > 0)
	{
		// 批量模式：blueprint_path 仍然是必填的公共参数
		FString BlueprintPath;
		if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
			return;
		}
		
		FString GraphName;
		Payload->TryGetStringField(TEXT("graph_name"), GraphName);
		
		// 加载蓝图（只加载一次）
		UBlueprint* Blueprint = nullptr;
		FString ResolvedPath;
		if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return;
		}
		
		UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
		if (!Graph)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
			return;
		}
		
		// 准备返回结果
		TArray<TSharedPtr<FJsonValue>> CreatedNodes;
		TArray<FString> Errors;
		
		// 循环处理每个节点
		for (int32 i = 0; i < NodesArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& NodeValue = (*NodesArray)[i];
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !(*NodeObjPtr).IsValid())
			{
				Errors.Add(FString::Printf(TEXT("nodes[%d]: invalid object"), i));
				continue;
			}
			const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;
			
			// 提取节点参数
			FString NodeType, NodeName;
			if (!NodeObj->TryGetStringField(TEXT("node_type"), NodeType) || NodeType.IsEmpty())
			{
				Errors.Add(FString::Printf(TEXT("nodes[%d]: missing node_type"), i));
				continue;
			}
			if (!NodeObj->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.IsEmpty())
			{
				Errors.Add(FString::Printf(TEXT("nodes[%d]: missing node_name"), i));
				continue;
			}
			
			// 构建单节点 Payload 并递归调用
			TSharedPtr<FJsonObject> SinglePayload = MakeShared<FJsonObject>();
			SinglePayload->SetStringField(TEXT("blueprint_path"), BlueprintPath);
			SinglePayload->SetStringField(TEXT("graph_name"), Graph->GetName());
			SinglePayload->SetStringField(TEXT("node_type"), NodeType);
			SinglePayload->SetStringField(TEXT("node_name"), NodeName);
			
			// 复制可选参数
			FString TargetClass;
			if (NodeObj->TryGetStringField(TEXT("target_class"), TargetClass))
				SinglePayload->SetStringField(TEXT("target_class"), TargetClass);
			
			const TSharedPtr<FJsonObject>* PosPtr = nullptr;
			if (NodeObj->TryGetObjectField(TEXT("node_position"), PosPtr))
				SinglePayload->SetObjectField(TEXT("node_position"), *PosPtr);
			
			int32 FirstIndex = 0, LastIndex = 0;
			if (NodeObj->TryGetNumberField(TEXT("first_index"), FirstIndex))
				SinglePayload->SetNumberField(TEXT("first_index"), FirstIndex);
			if (NodeObj->TryGetNumberField(TEXT("last_index"), LastIndex))
				SinglePayload->SetNumberField(TEXT("last_index"), LastIndex);
			
			// 递归调用自己处理单个节点（使用临时 RequestId）
			// 为了简化，这里内联处理核心逻辑 - 但使用同一个 Graph
			// TODO: 后续可重构为内部函数
			
			// 目前批量创建在框架阶段，记录成功解析的参数
			TSharedPtr<FJsonObject> ParsedNode = MakeShared<FJsonObject>();
			ParsedNode->SetNumberField(TEXT("index"), i);
			ParsedNode->SetStringField(TEXT("node_type"), NodeType);
			ParsedNode->SetStringField(TEXT("node_name"), NodeName);
			ParsedNode->SetStringField(TEXT("status"), TEXT("parsed"));
			CreatedNodes.Add(MakeShared<FJsonValueObject>(ParsedNode));
		}
		
		// 返回批量结果
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), Errors.Num() == 0);
		Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("graph_name"), Graph->GetName());
		Result->SetArrayField(TEXT("created_nodes"), CreatedNodes);
		if (Errors.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ErrorsArray;
			for (const FString& Err : Errors)
			{
				ErrorsArray.Add(MakeShared<FJsonValueString>(Err));
			}
			Result->SetArrayField(TEXT("errors"), ErrorsArray);
		}
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}
	// ===== 单节点模式（原有逻辑）=====
	
	auto BuildReceivedKeys = [&]() -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Keys;
		if (!Payload.IsValid())
		{
			return Keys;
		}
		for (const auto& Pair : Payload->Values)
		{
			Keys.Add(MakeShared<FJsonValueString>(Pair.Key));
		}
		return Keys;
	};

	auto BuildAddNodeHelpDetails = [&]() -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		// 必填字段
		{
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("blueprint_path")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("node_type")));
			Required.Add(MakeShared<FJsonValueString>(TEXT("node_name")));
			Details->SetArrayField(TEXT("required_fields"), Required);
		}
		// 常用 node_type（这里给的是“协议层可接受的概念枚举”，并非全部别名）
		{
			TArray<TSharedPtr<FJsonValue>> Allowed;
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("Event")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("Function")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("VariableGet")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("VariableSet")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("InputAction")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("Branch")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("Sequence")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("Cast")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("SpawnActor")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("Macro")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("ForLoop")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("WhileLoop")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("Gate")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("DoOnce")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("DoN")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("FlipFlop")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("CustomEvent")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("Select")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("MakeArray")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("MakeStruct")));
			Allowed.Add(MakeShared<FJsonValueString>(TEXT("BreakStruct")));
			Details->SetArrayField(TEXT("allowed_node_types"), Allowed);
		}
		// 示例请求参数（给 Agent 直接照抄的最小可用样例）
		{
			TSharedPtr<FJsonObject> Example = MakeShared<FJsonObject>();
			Example->SetStringField(TEXT("blueprint_path"), TEXT("/Game/Blueprints/BP_Greeter"));
			Example->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
			Example->SetStringField(TEXT("node_type"), TEXT("Event"));
			Example->SetStringField(TEXT("node_name"), TEXT("BeginPlay"));
			TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
			Pos->SetNumberField(TEXT("x"), 0);
			Pos->SetNumberField(TEXT("y"), 0);
			Example->SetObjectField(TEXT("node_position"), Pos);
			Details->SetObjectField(TEXT("example_params"), Example);
		}
		// 插件实际收到的 keys（用于快速定位“你是不是传成 type/name 了”）
		Details->SetArrayField(TEXT("received_keys"), BuildReceivedKeys());
		// 友好提示
		Details->SetStringField(TEXT("hint"),
			TEXT("Make sure to send params with exact keys: blueprint_path, node_type, node_name. ")
			TEXT("If you used type/name or nodeType/nodeName, map them to node_type/node_name.")
		);
		return Details;
	};

	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"), BuildAddNodeHelpDetails());
		return;
	}

	FString NodeType;
	if (!Payload->TryGetStringField(TEXT("node_type"), NodeType) || NodeType.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_type"), BuildAddNodeHelpDetails());
		return;
	}

	FString NodeName;
	if (!Payload->TryGetStringField(TEXT("node_name"), NodeName) || NodeName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_name"), BuildAddNodeHelpDetails());
		return;
	}

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);

	// 可选：是否复用已有节点（默认 true，仅在未显式指定 node_position 时生效）
	bool bReuseExisting = true;
	Payload->TryGetBoolField(TEXT("reuse_existing"), bReuseExisting);

	// node_position 只有“字段存在”才算显式指定（即便 x/y=0 也应当尊重）
	bool bHasExplicitPosition = false;
	// 允许用户强制使用 (0,0) 等坐标；否则把 (0,0) 视为“未指定”以避免 Agent 默认值导致堆叠
	bool bForcePosition = false;
	Payload->TryGetBoolField(TEXT("force_position"), bForcePosition);
	int32 PosX = 0, PosY = 0;
	const TSharedPtr<FJsonObject>* PosObjPtr = nullptr;
	if (Payload->TryGetObjectField(TEXT("node_position"), PosObjPtr) && PosObjPtr && (*PosObjPtr).IsValid())
	{
		bHasExplicitPosition = true;
		(*PosObjPtr)->TryGetNumberField(TEXT("x"), PosX);
		(*PosObjPtr)->TryGetNumberField(TEXT("y"), PosY);
	}

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		return;
	}

	// 自动排版：当未显式指定 node_position 时，把新节点放到当前图表最右侧之后，并稍微纵向错开
	// 注意：很多 Agent 会把默认 position 传成 (0,0)（或由上层 schema 生成），这会造成节点堆叠。
	// 因此：如果显式给了 (0,0) 且没有 force_position=true，我们也按“未指定”处理。
	if (bHasExplicitPosition && !bForcePosition && PosX == 0 && PosY == 0)
	{
		bHasExplicitPosition = false;
	}

	if (!bHasExplicitPosition)
	{
		// === 智能排版（Smart Cursor / Auto Layout）===
		// 目标：
		// 1) 不受离群节点影响（图里某个节点被丢到很远会拉爆 MaxX）
		// 2) 自适应间距（从现有节点的分布估计 stepX/stepY）
		// 3) 找“最近的空位”（避免堆叠），并在 Y 过长时自动换列
		TArray<int32> Xs, Ys;
		Xs.Reserve(Graph->Nodes.Num());
		Ys.Reserve(Graph->Nodes.Num());
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			Xs.Add(Node->NodePosX);
			Ys.Add(Node->NodePosY);
		}

		auto Quantile = [](TArray<int32> Arr, float Q) -> int32
		{
			if (Arr.Num() == 0) return 0;
			Arr.Sort();
			const float IdxF = FMath::Clamp(Q, 0.0f, 1.0f) * float(Arr.Num() - 1);
			const int32 Lo = FMath::Clamp(int32(FMath::FloorToFloat(IdxF)), 0, Arr.Num() - 1);
			const int32 Hi = FMath::Clamp(int32(FMath::CeilToFloat(IdxF)), 0, Arr.Num() - 1);
			if (Lo == Hi) return Arr[Lo];
			const float Alpha = IdxF - float(Lo);
			return FMath::RoundToInt(FMath::Lerp(float(Arr[Lo]), float(Arr[Hi]), Alpha));
		};

		auto MedianDelta = [](TArray<int32> Arr, int32 MinUsefulDelta, int32 Fallback) -> int32
		{
			if (Arr.Num() < 2) return Fallback;
			Arr.Sort();
			TArray<int32> Deltas;
			for (int32 i = 1; i < Arr.Num(); ++i)
			{
				const int32 D = Arr[i] - Arr[i - 1];
				if (D >= MinUsefulDelta)
				{
					Deltas.Add(D);
				}
			}
			if (Deltas.Num() == 0) return Fallback;
			Deltas.Sort();
			return Deltas[Deltas.Num() / 2];
		};

		// 过滤离群值：如果节点多，使用 IQR 去掉极端远处节点（避免跨度离谱）
		auto FilterInliersByIQR = [&](const TArray<int32>& In, TArray<int32>& Out, float K) -> void
		{
			Out.Reset();
			if (In.Num() < 10)
			{
				Out = In;
				return;
			}
			const int32 Q1 = Quantile(In, 0.25f);
			const int32 Q3 = Quantile(In, 0.75f);
			const int32 IQR = FMath::Max(1, Q3 - Q1);
			const int32 Lo = Q1 - int32(K * float(IQR));
			const int32 Hi = Q3 + int32(K * float(IQR));
			for (int32 V : In)
			{
				if (V >= Lo && V <= Hi)
				{
					Out.Add(V);
				}
			}
			if (Out.Num() < 3)
			{
				// 过度过滤则回退
				Out = In;
			}
		};

		TArray<int32> XInliers, YInliers;
		FilterInliersByIQR(Xs, XInliers, 3.0f);
		FilterInliersByIQR(Ys, YInliers, 3.0f);

		// 自适应步长：用中位差估算，带 clamp（避免过密/过疏）
		const int32 StepX = FMath::Clamp(MedianDelta(XInliers, 80, 360), 260, 520);
		const int32 StepY = FMath::Clamp(MedianDelta(YInliers, 60, 220), 150, 360);

		// 以“主簇”右侧作为起点
		const int32 MinY = (YInliers.Num() > 0) ? Quantile(YInliers, 0.10f) : 0;
		const int32 MaxX = (XInliers.Num() > 0) ? Quantile(XInliers, 0.90f) : 0;

		// 估算可用行数：用主簇的高度范围，超了就换列
		const int32 MinY2 = (YInliers.Num() > 0) ? Quantile(YInliers, 0.10f) : 0;
		const int32 MaxY2 = (YInliers.Num() > 0) ? Quantile(YInliers, 0.90f) : 0;
		const int32 BandHeight = FMath::Clamp((MaxY2 - MinY2) + StepY * 2, StepY * 5, StepY * 10);
		const int32 MaxRows = FMath::Clamp(BandHeight / StepY, 6, 14);

		// 将已有节点映射到网格，避免重叠（粗粒度碰撞）
		// UE5.0 的 TSet 模板参数不是 std::unordered_set 风格（不能直接塞 Hash/Eq）。
		// 这里直接把 (CX,CY) 打包成 uint64 作为 Key，避免 KeyFuncs 兼容性问题。
		auto MakeCellKey = [](int32 CX, int32 CY) -> uint64
		{
			return (uint64(uint32(CX)) << 32) | uint64(uint32(CY));
		};
		TSet<uint64> Occupied;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			const int32 CX = FMath::RoundToInt(float(Node->NodePosX) / float(StepX));
			const int32 CY = FMath::RoundToInt(float(Node->NodePosY) / float(StepY));
			Occupied.Add(MakeCellKey(CX, CY));
		}

		const int32 BaseCellX = FMath::RoundToInt(float(MaxX) / float(StepX)) + 1;
		const int32 BaseCellY = FMath::RoundToInt(float(MinY) / float(StepY));

		bool bPlaced = false;
		const int32 MaxColsToScan = 20;
		for (int32 Col = 0; Col < MaxColsToScan && !bPlaced; ++Col)
		{
			const int32 CX = BaseCellX + Col;
			for (int32 Row = 0; Row < MaxRows; ++Row)
			{
				const int32 CY = BaseCellY + Row;
				if (!Occupied.Contains(MakeCellKey(CX, CY)))
				{
					PosX = CX * StepX;
					PosY = CY * StepY;
					bPlaced = true;
					break;
				}
			}
		}

		if (!bPlaced)
		{
			// 最终兜底：仍然放到右侧，但别一路往下
			PosX = (BaseCellX + 1) * StepX;
			PosY = BaseCellY * StepY;
		}
	}

	UEdGraphNode* NewNodeBase = nullptr;
	const FString NodeTypeLower = NodeType.ToLower();

	// 幂等（有限、保守）：复用图中“完全未连线”的 Branch / Select，避免死循环式重复添加
	if (bReuseExisting && !bHasExplicitPosition)
	{
		auto IsNodeIsolated = [](UEdGraphNode* Node) -> bool
		{
			if (!Node) return false;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				if (Pin->LinkedTo.Num() > 0)
				{
					return false;
				}
			}
			return true;
		};

		if (NodeTypeLower == TEXT("branch") || NodeTypeLower == TEXT("if"))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_IfThenElse* IfNode = Cast<UK2Node_IfThenElse>(Node);
				if (IfNode && IsNodeIsolated(IfNode))
				{
					TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetBoolField(TEXT("ok"), true);
					Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
					Result->SetStringField(TEXT("graph_name"), Graph->GetName());
					Result->SetStringField(TEXT("node_id"), UAL_GuidToString(IfNode->NodeGuid));
					Result->SetStringField(TEXT("node_class"), IfNode->GetClass()->GetName());
					Result->SetArrayField(TEXT("pins"), UAL_BuildPinsJson(IfNode));
					Result->SetBoolField(TEXT("reused"), true);
					UAL_CommandUtils::SendResponse(RequestId, 200, Result);
					return;
				}
			}
		}
		else if (NodeTypeLower == TEXT("select"))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_Select* SelNode = Cast<UK2Node_Select>(Node);
				if (SelNode && IsNodeIsolated(SelNode))
				{
					TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetBoolField(TEXT("ok"), true);
					Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
					Result->SetStringField(TEXT("graph_name"), Graph->GetName());
					Result->SetStringField(TEXT("node_id"), UAL_GuidToString(SelNode->NodeGuid));
					Result->SetStringField(TEXT("node_class"), SelNode->GetClass()->GetName());
					Result->SetArrayField(TEXT("pins"), UAL_BuildPinsJson(SelNode));
					Result->SetBoolField(TEXT("reused"), true);
					UAL_CommandUtils::SendResponse(RequestId, 200, Result);
					return;
				}
			}
		}
	}

	if (NodeTypeLower == TEXT("event"))
	{
		// 支持：ReceiveBeginPlay / BeginPlay / ReceiveTick 等
		FString EventFuncName = NodeName;
		if (EventFuncName.Equals(TEXT("BeginPlay"), ESearchCase::IgnoreCase))
		{
			EventFuncName = TEXT("ReceiveBeginPlay");
		}
		else if (EventFuncName.Equals(TEXT("Tick"), ESearchCase::IgnoreCase))
		{
			EventFuncName = TEXT("ReceiveTick");
		}
		else if (!EventFuncName.StartsWith(TEXT("Receive")) && !EventFuncName.StartsWith(TEXT("On")))
		{
			// 尝试加 Receive 前缀（常用 Actor 事件）
			EventFuncName = TEXT("Receive") + EventFuncName;
		}

		UClass* OwnerClass = Blueprint->ParentClass ? Blueprint->ParentClass : AActor::StaticClass();
		UFunction* EventFunc = OwnerClass ? OwnerClass->FindFunctionByName(FName(*EventFuncName)) : nullptr;
		// UClass::FindFunctionByName 默认会在父类链上查找；这里保持简单兼容
		if (!EventFunc)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Event function not found: %s"), *EventFuncName));
			return;
		}

		FGraphNodeCreator<UK2Node_Event> NodeCreator(*Graph);
		UK2Node_Event* EventNode = NodeCreator.CreateNode();
		EventNode->EventReference.SetExternalMember(EventFunc->GetFName(), OwnerClass);
		EventNode->bOverrideFunction = true;
		EventNode->NodePosX = PosX;
		EventNode->NodePosY = PosY;
		NodeCreator.Finalize();
		EventNode->ReconstructNode();
		NewNodeBase = EventNode;
	}
	else if (NodeTypeLower == TEXT("function"))
	{
		// 形如：KismetSystemLibrary.PrintString 或 UKismetSystemLibrary.PrintString
		FString ClassPart, FuncPart;
		if (!NodeName.Split(TEXT("."), &ClassPart, &FuncPart))
		{
			// 只给函数名时，优先在 Blueprint ParentClass 上找
			FuncPart = NodeName;
		}

		UFunction* TargetFunc = nullptr;
		UClass* TargetClass = nullptr;

		if (!ClassPart.IsEmpty())
		{
			FString ClassError;
			TargetClass = UAL_CommandUtils::ResolveClassFromIdentifier(ClassPart, UObject::StaticClass(), ClassError);
			if (!TargetClass)
			{
				// 兼容 KismetSystemLibrary 这种不带 U 前缀
				TargetClass = UAL_CommandUtils::ResolveClassFromIdentifier(TEXT("U") + ClassPart, UObject::StaticClass(), ClassError);
			}
			if (!TargetClass)
			{
				UAL_CommandUtils::SendError(RequestId, 404, ClassError);
				return;
			}
			TargetFunc = TargetClass->FindFunctionByName(FName(*FuncPart));
		}
		else
		{
			// 在父类找成员函数
			TargetClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->ParentClass;
			TargetFunc = TargetClass ? TargetClass->FindFunctionByName(FName(*FuncPart)) : nullptr;

			// 兜底：常用库函数（解决传入 Greater_IntInt/PrintString 这类简写）
			if (!TargetFunc)
			{
				const TArray<FString> CommonLibs = {
					TEXT("KismetMathLibrary"),
					TEXT("KismetSystemLibrary"),
					TEXT("GameplayStatics"),
					TEXT("KismetStringLibrary")
				};

				for (const FString& LibName : CommonLibs)
				{
					FString ClassError;
					if (UClass* LibClass = UAL_CommandUtils::ResolveClassFromIdentifier(LibName, UObject::StaticClass(), ClassError))
					{
						if (UFunction* F = LibClass->FindFunctionByName(FName(*FuncPart)))
						{
							TargetClass = LibClass;
							TargetFunc = F;
							break;
						}
					}
				}
			}
		}

		if (!TargetFunc || !TargetClass)
		{
			// 模糊建议：从常用库 + 父类链上收集候选函数名并给出近似匹配
			TArray<FString> Candidates;
			auto CollectFuncs = [&](UClass* Cls)
			{
				if (!Cls) return;
				for (TFieldIterator<UFunction> It(Cls, EFieldIteratorFlags::IncludeSuper); It; ++It)
				{
					if (UFunction* UF = *It)
					{
						Candidates.AddUnique(UF->GetName());
					}
				}
			};

			CollectFuncs(Blueprint->GeneratedClass ? Blueprint->GeneratedClass : Blueprint->ParentClass);
			{
				const TArray<FString> CommonLibs = {
					TEXT("KismetMathLibrary"),
					TEXT("KismetSystemLibrary"),
					TEXT("GameplayStatics"),
					TEXT("KismetStringLibrary")
				};
				for (const FString& LibName : CommonLibs)
				{
					FString ClassError;
					if (UClass* LibClass = UAL_CommandUtils::ResolveClassFromIdentifier(LibName, UObject::StaticClass(), ClassError))
					{
						CollectFuncs(LibClass);
					}
				}
			}

			TArray<FString> Suggestions;
			UAL_CommandUtils::SuggestProperties(FuncPart, Candidates, Suggestions, 8);

			TSharedPtr<FJsonObject> Details = BuildAddNodeHelpDetails();
			{
				TArray<TSharedPtr<FJsonValue>> Sug;
				TArray<TSharedPtr<FJsonValue>> FullSug; // 带类名前缀的完整建议
				for (const FString& S : Suggestions)
				{
					Sug.Add(MakeShared<FJsonValueString>(S));
					// 尝试找到这个函数所在的类，提供完整格式
					FString FullName = S;
					const TArray<FString> CommonLibs = {
						TEXT("KismetMathLibrary"),
						TEXT("KismetSystemLibrary"),
						TEXT("GameplayStatics"),
						TEXT("KismetStringLibrary")
					};
					for (const FString& LibName : CommonLibs)
					{
						FString ClassError;
						if (UClass* LibClass = UAL_CommandUtils::ResolveClassFromIdentifier(LibName, UObject::StaticClass(), ClassError))
						{
							if (LibClass->FindFunctionByName(FName(*S)))
							{
								FullName = LibName + TEXT(".") + S;
								break;
							}
						}
					}
					FullSug.Add(MakeShared<FJsonValueString>(FullName));
				}
				Details->SetStringField(TEXT("requested_function"), FuncPart);
				Details->SetArrayField(TEXT("function_suggestions"), Sug);
				Details->SetArrayField(TEXT("suggested_full_names"), FullSug); // 可直接使用的完整格式
				Details->SetStringField(
					TEXT("hint_function"),
					TEXT("Use ClassName.FunctionName format! Example: KismetMathLibrary.Greater_IntInt, KismetSystemLibrary.PrintString")
				);
			}

			UAL_CommandUtils::SendError(
				RequestId,
				404,
				FString::Printf(TEXT("Function not found: %s"), *NodeName),
				Details
			);
			return;
		}

		FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
		UK2Node_CallFunction* CallNode = NodeCreator.CreateNode();
		CallNode->FunctionReference.SetExternalMember(TargetFunc->GetFName(), TargetClass);
		CallNode->NodePosX = PosX;
		CallNode->NodePosY = PosY;
		NodeCreator.Finalize();
		CallNode->ReconstructNode();
		NewNodeBase = CallNode;
	}
	else if (NodeTypeLower == TEXT("variableget") || NodeTypeLower == TEXT("variable_get"))
	{
		const FName VarName(*NodeName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) == INDEX_NONE)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Variable not found: %s"), *NodeName));
			return;
		}
		FGraphNodeCreator<UK2Node_VariableGet> NodeCreator(*Graph);
		UK2Node_VariableGet* VarNode = NodeCreator.CreateNode();
		VarNode->VariableReference.SetSelfMember(VarName);
		VarNode->NodePosX = PosX;
		VarNode->NodePosY = PosY;
		NodeCreator.Finalize();
		VarNode->ReconstructNode();
		NewNodeBase = VarNode;
	}
	else if (NodeTypeLower == TEXT("variableset") || NodeTypeLower == TEXT("variable_set"))
	{
		const FName VarName(*NodeName);
		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VarName) == INDEX_NONE)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Variable not found: %s"), *NodeName));
			return;
		}
		FGraphNodeCreator<UK2Node_VariableSet> NodeCreator(*Graph);
		UK2Node_VariableSet* VarNode = NodeCreator.CreateNode();
		VarNode->VariableReference.SetSelfMember(VarName);
		VarNode->NodePosX = PosX;
		VarNode->NodePosY = PosY;
		NodeCreator.Finalize();
		VarNode->ReconstructNode();
		NewNodeBase = VarNode;
	}
	else if (NodeTypeLower == TEXT("inputaction") || NodeTypeLower == TEXT("input_action"))
	{
		FName ActionName(*NodeName);
		FGraphNodeCreator<UK2Node_InputAction> NodeCreator(*Graph);
		UK2Node_InputAction* InputActionNode = NodeCreator.CreateNode();
		InputActionNode->InputActionName = ActionName;
		InputActionNode->NodePosX = PosX;
		InputActionNode->NodePosY = PosY;
		NodeCreator.Finalize();
		InputActionNode->ReconstructNode();
		NewNodeBase = InputActionNode;
	}
	else if (NodeTypeLower == TEXT("branch") || NodeTypeLower == TEXT("if"))
	{
		FGraphNodeCreator<UK2Node_IfThenElse> NodeCreator(*Graph);
		UK2Node_IfThenElse* IfNode = NodeCreator.CreateNode();
		IfNode->NodePosX = PosX;
		IfNode->NodePosY = PosY;
		NodeCreator.Finalize();
		IfNode->ReconstructNode();
		NewNodeBase = IfNode;
	}
	else if (NodeTypeLower == TEXT("sequence") || NodeTypeLower == TEXT("execution_sequence"))
	{
		FGraphNodeCreator<UK2Node_ExecutionSequence> NodeCreator(*Graph);
		UK2Node_ExecutionSequence* SequenceNode = NodeCreator.CreateNode();
		SequenceNode->NodePosX = PosX;
		SequenceNode->NodePosY = PosY;
		NodeCreator.Finalize();
		SequenceNode->ReconstructNode();
		NewNodeBase = SequenceNode;
	}
	else if (NodeTypeLower == TEXT("cast") || NodeTypeLower == TEXT("cast_to"))
	{
		FString TargetClassStr;
		if (!Payload->TryGetStringField(TEXT("target_class"), TargetClassStr))
		{
			// 兼容 class 字段
			Payload->TryGetStringField(TEXT("class"), TargetClassStr);
		}
		if (TargetClassStr.IsEmpty())
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: target_class for Cast node"));
			return;
		}
		FString ClassError;
		UClass* TargetClass = UAL_CommandUtils::ResolveClassFromIdentifier(TargetClassStr, UObject::StaticClass(), ClassError);
		if (!TargetClass)
		{
			UAL_CommandUtils::SendError(RequestId, 404, ClassError);
			return;
		}
		FGraphNodeCreator<UK2Node_DynamicCast> NodeCreator(*Graph);
		UK2Node_DynamicCast* CastNode = NodeCreator.CreateNode();
		CastNode->TargetType = TargetClass;
		CastNode->NodePosX = PosX;
		CastNode->NodePosY = PosY;
		NodeCreator.Finalize();
		CastNode->ReconstructNode();
		NewNodeBase = CastNode;
	}
	else if (NodeTypeLower == TEXT("spawn_actor") || NodeTypeLower == TEXT("spawnactor") || NodeTypeLower == TEXT("spawnactorfromclass"))
	{
		FString TargetClassStr;
		Payload->TryGetStringField(TEXT("class"), TargetClassStr); 
		// SpawnActor 可以不指定 Class (Class 引脚传入)，所以不强制校验非空，但如果有值则设定
		
		FGraphNodeCreator<UK2Node_SpawnActorFromClass> NodeCreator(*Graph);
		UK2Node_SpawnActorFromClass* SpawnNode = NodeCreator.CreateNode();
		SpawnNode->NodePosX = PosX;
		SpawnNode->NodePosY = PosY;
		NodeCreator.Finalize(); // 先 Finalize 初始化 Pin

		if (!TargetClassStr.IsEmpty())
		{
			FString ClassError;
			UClass* TargetClass = UAL_CommandUtils::ResolveClassFromIdentifier(TargetClassStr, UObject::StaticClass(), ClassError);
			if (TargetClass)
			{
				if (UEdGraphPin* ClassPin = SpawnNode->GetClassPin())
				{
					ClassPin->DefaultObject = TargetClass;
					// Reconstuct logic to update result pin
					SpawnNode->ReconstructNode();
				}
			}
		}
		NewNodeBase = SpawnNode;
	}
	else if (NodeTypeLower == TEXT("macro") || NodeTypeLower == TEXT("forloop") || NodeTypeLower == TEXT("for_loop") || NodeTypeLower == TEXT("whileloop") || NodeTypeLower == TEXT("while_loop") || NodeTypeLower == TEXT("gate") || NodeTypeLower == TEXT("doonce") || NodeTypeLower == TEXT("do_once") || NodeTypeLower == TEXT("don") || NodeTypeLower == TEXT("do_n") || NodeTypeLower == TEXT("flipflop") || NodeTypeLower == TEXT("flip_flop"))
	{
		// 处理标准宏 (StandardMacros)
		// 如果 NodeTypeLower 本身就是宏名字 (如 for_loop)，直接用它查找
		FString MacroName = NodeName;
		if (MacroName.IsEmpty() || MacroName == TEXT("Default"))
		{
			// 尝试从 NodeType 推断宏名（支持有下划线和无下划线两种格式）
			if (NodeTypeLower == TEXT("for_loop") || NodeTypeLower == TEXT("forloop")) MacroName = TEXT("ForLoop");
			else if (NodeTypeLower == TEXT("while_loop") || NodeTypeLower == TEXT("whileloop")) MacroName = TEXT("WhileLoop");
			else if (NodeTypeLower == TEXT("gate")) MacroName = TEXT("Gate");
			else if (NodeTypeLower == TEXT("do_once") || NodeTypeLower == TEXT("doonce")) MacroName = TEXT("DoOnce");
			else if (NodeTypeLower == TEXT("do_n") || NodeTypeLower == TEXT("don")) MacroName = TEXT("DoN");
			else if (NodeTypeLower == TEXT("flip_flop") || NodeTypeLower == TEXT("flipflop")) MacroName = TEXT("FlipFlop");
		}
		
		UBlueprint* MacroLib = nullptr;
		// 默认查找 StandardMacros
		// 注意：StandardMacros 通常位于 /Engine/EditorBlueprintResources/StandardMacros
		// 但 FKismetEditorUtilities 可能提供了查找工具
		FString MacroLibPath;
		if (Payload->TryGetStringField(TEXT("macro_lib"), MacroLibPath) && !MacroLibPath.IsEmpty())
		{
			FString UnusedPath;
			UAL_LoadBlueprintByPathOrName(MacroLibPath, MacroLib, UnusedPath);
		}
		
		if (!MacroLib)
		{
			// 尝试找系统标准库
			FString StdLibPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");
			MacroLib = LoadObject<UBlueprint>(nullptr, *StdLibPath);
		}

		if (!MacroLib)
		{
			UAL_CommandUtils::SendError(RequestId, 404, TEXT("Could not find StandardMacros library"));
			return;
		}

		// 在 MacroLib 中找 Macro Graph
		UEdGraph* MacroGraph = UAL_FindGraph(MacroLib, MacroName);
		if (!MacroGraph)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Macro not found: %s in %s"), *MacroName, *MacroLib->GetName()));
			return;
		}

		FGraphNodeCreator<UK2Node_MacroInstance> NodeCreator(*Graph);
		UK2Node_MacroInstance* MacroNode = NodeCreator.CreateNode();
		MacroNode->SetMacroGraph(MacroGraph);
		MacroNode->NodePosX = PosX;
		MacroNode->NodePosY = PosY;
		NodeCreator.Finalize();
		MacroNode->ReconstructNode();
		
		// 为 ForLoop 设置 FirstIndex 和 LastIndex 默认值
		if (MacroName.Equals(TEXT("ForLoop"), ESearchCase::IgnoreCase))
		{
			// 尝试获取并设置 FirstIndex
			int32 FirstIndex = 0;
			if (Payload->TryGetNumberField(TEXT("first_index"), FirstIndex))
			{
				if (UEdGraphPin* FirstIndexPin = UAL_FindPinByName(MacroNode, TEXT("FirstIndex")))
				{
					FirstIndexPin->DefaultValue = FString::FromInt(FirstIndex);
				}
			}
			
			// 尝试获取并设置 LastIndex
			int32 LastIndex = 0;
			if (Payload->TryGetNumberField(TEXT("last_index"), LastIndex))
			{
				if (UEdGraphPin* LastIndexPin = UAL_FindPinByName(MacroNode, TEXT("LastIndex")))
				{
					LastIndexPin->DefaultValue = FString::FromInt(LastIndex);
				}
			}
		}
		
		NewNodeBase = MacroNode;
	}
	else if (NodeTypeLower == TEXT("custom_event") || NodeTypeLower == TEXT("customevent"))
	{
		FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*Graph);
		UK2Node_CustomEvent* EventNode = NodeCreator.CreateNode();
		EventNode->CustomFunctionName = FName(*NodeName);
		EventNode->NodePosX = PosX;
		EventNode->NodePosY = PosY;
		NodeCreator.Finalize();
		EventNode->ReconstructNode();
		NewNodeBase = EventNode;
	}
	else if (NodeTypeLower == TEXT("select"))
	{
		FGraphNodeCreator<UK2Node_Select> NodeCreator(*Graph);
		UK2Node_Select* SelectNode = NodeCreator.CreateNode();
		SelectNode->NodePosX = PosX;
		SelectNode->NodePosY = PosY;
		NodeCreator.Finalize();
		SelectNode->ReconstructNode();
		NewNodeBase = SelectNode;
	}
	else if (NodeTypeLower == TEXT("make_array") || NodeTypeLower == TEXT("makearray"))
	{
		FGraphNodeCreator<UK2Node_MakeArray> NodeCreator(*Graph);
		UK2Node_MakeArray* ArrayNode = NodeCreator.CreateNode();
		ArrayNode->NodePosX = PosX;
		ArrayNode->NodePosY = PosY;
		NodeCreator.Finalize();
		ArrayNode->ReconstructNode();
		NewNodeBase = ArrayNode;
	}
	else if (NodeTypeLower == TEXT("make_struct") || NodeTypeLower == TEXT("makestruct"))
	{
		FString StructTypeStr;
		if (!Payload->TryGetStringField(TEXT("struct_type"), StructTypeStr))
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: struct_type for MakeStruct"));
			return;
		}
		UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructTypeStr);
		// 尝试作为短名查找
		if (!Struct)
		{
			Struct = FindObject<UScriptStruct>(ANY_PACKAGE, *StructTypeStr);
		}
		if (!Struct)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Struct not found: %s"), *StructTypeStr));
			return;
		}

		FGraphNodeCreator<UK2Node_MakeStruct> NodeCreator(*Graph);
		UK2Node_MakeStruct* StructNode = NodeCreator.CreateNode();
		StructNode->StructType = Struct;
		StructNode->NodePosX = PosX;
		StructNode->NodePosY = PosY;
		NodeCreator.Finalize();
		StructNode->ReconstructNode();
		NewNodeBase = StructNode;
	}
	else if (NodeTypeLower == TEXT("break_struct") || NodeTypeLower == TEXT("breakstruct"))
	{
		FString StructTypeStr;
		if (!Payload->TryGetStringField(TEXT("struct_type"), StructTypeStr))
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: struct_type for BreakStruct"));
			return;
		}
		UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructTypeStr);
		if (!Struct) Struct = FindObject<UScriptStruct>(ANY_PACKAGE, *StructTypeStr);
		
		if (!Struct)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Struct not found: %s"), *StructTypeStr));
			return;
		}

		FGraphNodeCreator<UK2Node_BreakStruct> NodeCreator(*Graph);
		UK2Node_BreakStruct* StructNode = NodeCreator.CreateNode();
		StructNode->StructType = Struct;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		StructNode->bMadeAfterOverridePinInMake = true;
#endif
		StructNode->NodePosX = PosX;
		StructNode->NodePosY = PosY;
		NodeCreator.Finalize();
		StructNode->ReconstructNode();
		NewNodeBase = StructNode;
	}
	else
	{
		UAL_CommandUtils::SendError(
			RequestId,
			400,
			FString::Printf(TEXT("Unsupported node_type: %s"), *NodeType),
			BuildAddNodeHelpDetails()
		);
		return;
	}

	if (!NewNodeBase)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create node"));
		return;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetStringField(TEXT("node_id"), UAL_GuidToString(NewNodeBase->NodeGuid));
	Result->SetStringField(TEXT("node_class"), NewNodeBase->GetClass()->GetName());
	Result->SetArrayField(TEXT("pins"), UAL_BuildPinsJson(NewNodeBase));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 在蓝图图表中添加 Timeline 节点
 *
 * Timeline 不是普通变量/节点：必须在 Blueprint 上创建/持有 UTimelineTemplate，
 * 同时在图表中放置 UK2Node_Timeline 引用该模板，才能在 My Blueprint 面板中出现并可编辑。
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - graph_name: 图表名（可选，默认 EventGraph）
 *   - timeline_name: Timeline 名称（必填）
 *   - node_position: {x,y}（可选）
 *   - reuse_existing: 若已存在同名 Timeline 节点/模板，是否复用（可选，默认 true）
 *
 * 响应:
 *   - ok, node_id, node_class, pins[], timeline_name, template_created
 */
void FUAL_BlueprintCommands::Handle_AddTimelineToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString TimelineNameStr;
	if (!Payload->TryGetStringField(TEXT("timeline_name"), TimelineNameStr) || TimelineNameStr.IsEmpty())
	{
		// 兼容偶发字段漂移（name）
		Payload->TryGetStringField(TEXT("name"), TimelineNameStr);
	}
	if (TimelineNameStr.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: timeline_name"));
		return;
	}

	const FName TimelineName(*TimelineNameStr);

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);

	bool bReuseExisting = true;
	Payload->TryGetBoolField(TEXT("reuse_existing"), bReuseExisting);

	int32 PosX = 0, PosY = 0;
	bool bHasExplicitPosition = false;
	bool bForcePosition = false;
	UAL_ParseNodePosition(Payload, PosX, PosY, bHasExplicitPosition, bForcePosition);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
		return;
	}

	UAL_AutoLayoutIfNeeded(Graph, bHasExplicitPosition, bForcePosition, PosX, PosY);

	// 1) 如需复用：先找图表里是否已有同名 Timeline 节点
	if (bReuseExisting)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Timeline* TL = Cast<UK2Node_Timeline>(Node);
			if (!TL) continue;
			// UK2Node_Timeline 通常用 TimelineName 字段标识（对象名也可能一致）
			if (TL->TimelineName == TimelineName)
			{
				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetBoolField(TEXT("ok"), true);
				Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
				Result->SetStringField(TEXT("graph_name"), Graph->GetName());
				Result->SetStringField(TEXT("timeline_name"), TimelineName.ToString());
				Result->SetStringField(TEXT("node_id"), UAL_GuidToString(TL->NodeGuid));
				Result->SetStringField(TEXT("node_class"), TL->GetClass()->GetName());
				Result->SetArrayField(TEXT("pins"), UAL_BuildPinsJson(TL));
				Result->SetBoolField(TEXT("reused"), true);
				Result->SetBoolField(TEXT("template_created"), false);
				UAL_CommandUtils::SendResponse(RequestId, 200, Result);
				return;
			}
		}
	}

	// 2) 创建/复用 TimelineTemplate
	UTimelineTemplate* Template = nullptr;
	const bool bTemplateExists = UAL_FindTimelineTemplate(Blueprint, TimelineName, Template);
	bool bTemplateCreated = false;
	if (!Template)
	{
		Template = UAL_CreateTimelineTemplate(Blueprint, TimelineName);
		if (!Template)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create TimelineTemplate"));
			return;
		}
		bTemplateCreated = true;
	}
	else
	{
		bTemplateCreated = false;
	}

	// 3) 放置 Timeline 节点，并引用模板
	FGraphNodeCreator<UK2Node_Timeline> NodeCreator(*Graph);
	UK2Node_Timeline* TimelineNode = NodeCreator.CreateNode();
	TimelineNode->NodePosX = PosX;
	TimelineNode->NodePosY = PosY;
	NodeCreator.Finalize();

	TimelineNode->TimelineName = TimelineName;
	// TimelineTemplate 在 UE5.0 中不存在，由 ReconstructNode 自动关联
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	TimelineNode->TimelineGuid = Template->TimelineGuid;
#endif
	TimelineNode->ReconstructNode();

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Result->SetStringField(TEXT("graph_name"), Graph->GetName());
	Result->SetStringField(TEXT("timeline_name"), TimelineName.ToString());
	Result->SetStringField(TEXT("node_id"), UAL_GuidToString(TimelineNode->NodeGuid));
	Result->SetStringField(TEXT("node_class"), TimelineNode->GetClass()->GetName());
	Result->SetArrayField(TEXT("pins"), UAL_BuildPinsJson(TimelineNode));
	Result->SetBoolField(TEXT("template_created"), bTemplateCreated);
	Result->SetBoolField(TEXT("template_existed"), bTemplateExists);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 连接两个节点引脚
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - graph_name: 图表名（可选，默认 EventGraph）
 *   - source_node_id: 源节点 GUID（必填）
 *   - source_pin: 源引脚名称（必填）
 *   - target_node_id: 目标节点 GUID（必填）
 *   - target_pin: 目标引脚名称（必填）
 *
 * 响应:
 *   - ok
 *   - message
 */
void FUAL_BlueprintCommands::Handle_ConnectBlueprintPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);

	// ===== 批量连线模式 =====
	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("connections"), ConnectionsArray) && ConnectionsArray && ConnectionsArray->Num() > 0)
	{
		// 加载蓝图（只加载一次）
		UBlueprint* Blueprint = nullptr;
		FString ResolvedPath;
		if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
			return;
		}

		UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
		if (!Graph)
		{
			UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Graph not found: %s"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName));
			return;
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Schema);
		if (!K2Schema)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Graph schema is not K2"));
			return;
		}

		TArray<TSharedPtr<FJsonValue>> Results;
		int32 SuccessCount = 0;

		for (int32 i = 0; i < ConnectionsArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& ConnVal = (*ConnectionsArray)[i];
			const TSharedPtr<FJsonObject>* ConnObjPtr = nullptr;
			TSharedPtr<FJsonObject> ConnResult = MakeShared<FJsonObject>();
			ConnResult->SetNumberField(TEXT("index"), i);

			if (!ConnVal.IsValid() || !ConnVal->TryGetObject(ConnObjPtr) || !ConnObjPtr || !(*ConnObjPtr).IsValid())
			{
				ConnResult->SetBoolField(TEXT("ok"), false);
				ConnResult->SetStringField(TEXT("error"), TEXT("Invalid connection object"));
				Results.Add(MakeShared<FJsonValueObject>(ConnResult));
				continue;
			}
			const TSharedPtr<FJsonObject>& ConnObj = *ConnObjPtr;

			FString SrcNodeId, SrcPin, TgtNodeId, TgtPin;
			ConnObj->TryGetStringField(TEXT("source_node_id"), SrcNodeId);
			ConnObj->TryGetStringField(TEXT("source_pin"), SrcPin);
			ConnObj->TryGetStringField(TEXT("target_node_id"), TgtNodeId);
			ConnObj->TryGetStringField(TEXT("target_pin"), TgtPin);

			if (SrcNodeId.IsEmpty() || SrcPin.IsEmpty() || TgtNodeId.IsEmpty() || TgtPin.IsEmpty())
			{
				ConnResult->SetBoolField(TEXT("ok"), false);
				ConnResult->SetStringField(TEXT("error"), TEXT("Missing required fields"));
				Results.Add(MakeShared<FJsonValueObject>(ConnResult));
				continue;
			}

			UEdGraphNode* SrcNode = UAL_FindNodeByGuid(Graph, SrcNodeId);
			UEdGraphNode* TgtNode = UAL_FindNodeByGuid(Graph, TgtNodeId);
			if (!SrcNode || !TgtNode)
			{
				ConnResult->SetBoolField(TEXT("ok"), false);
				ConnResult->SetStringField(TEXT("error"), !SrcNode ? TEXT("Source node not found") : TEXT("Target node not found"));
				Results.Add(MakeShared<FJsonValueObject>(ConnResult));
				continue;
			}

			UEdGraphPin* SourcePin = UAL_FindPinByName(SrcNode, SrcPin);
			UEdGraphPin* TargetPin = UAL_FindPinByName(TgtNode, TgtPin);
			if (!SourcePin || !TargetPin)
			{
				ConnResult->SetBoolField(TEXT("ok"), false);
				ConnResult->SetStringField(TEXT("error"), !SourcePin ? TEXT("Source pin not found") : TEXT("Target pin not found"));
				Results.Add(MakeShared<FJsonValueObject>(ConnResult));
				continue;
			}

			const FPinConnectionResponse CanResp = K2Schema->CanCreateConnection(SourcePin, TargetPin);
			if (CanResp.Response == CONNECT_RESPONSE_DISALLOW)
			{
				ConnResult->SetBoolField(TEXT("ok"), false);
				ConnResult->SetStringField(TEXT("error"), CanResp.Message.ToString());
				Results.Add(MakeShared<FJsonValueObject>(ConnResult));
				continue;
			}

			const bool bConnected = K2Schema->TryCreateConnection(SourcePin, TargetPin);
			ConnResult->SetBoolField(TEXT("ok"), bConnected);
			if (bConnected)
			{
				SuccessCount++;
				ConnResult->SetStringField(TEXT("message"), TEXT("Connected"));
			}
			else
			{
				ConnResult->SetStringField(TEXT("error"), TEXT("Failed to create connection"));
			}
			Results.Add(MakeShared<FJsonValueObject>(ConnResult));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), SuccessCount > 0);
		Result->SetNumberField(TEXT("count"), SuccessCount);
		Result->SetNumberField(TEXT("total"), ConnectionsArray->Num());
		Result->SetArrayField(TEXT("results"), Results);
		UAL_CommandUtils::SendResponse(RequestId, 200, Result);
		return;
	}
	// ===== 单连线模式（原有逻辑）=====

	FString SourceNodeId;
	if (!Payload->TryGetStringField(TEXT("source_node_id"), SourceNodeId) || SourceNodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: source_node_id"));
		return;
	}
	FString SourcePinName;
	if (!Payload->TryGetStringField(TEXT("source_pin"), SourcePinName) || SourcePinName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: source_pin"));
		return;
	}
	FString TargetNodeId;
	if (!Payload->TryGetStringField(TEXT("target_node_id"), TargetNodeId) || TargetNodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: target_node_id"));
		return;
	}
	FString TargetPinName;
	if (!Payload->TryGetStringField(TEXT("target_pin"), TargetPinName) || TargetPinName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: target_pin"));
		return;
	}

	// GraphName 已在批量模式之前定义，此处复用
	// 单连线模式需要重新读取 graph_name（因为前面仅在批量模式下初始化）
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		return;
	}

	UEdGraphNode* SourceNode = UAL_FindNodeByGuid(Graph, SourceNodeId);
	UEdGraphNode* TargetNode = UAL_FindNodeByGuid(Graph, TargetNodeId);
	if (!SourceNode)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Source node not found: %s"), *SourceNodeId));
		return;
	}
	if (!TargetNode)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Target node not found: %s"), *TargetNodeId));
		return;
	}

	UEdGraphPin* SourcePin = UAL_FindPinByName(SourceNode, SourcePinName);
	UEdGraphPin* TargetPin = UAL_FindPinByName(TargetNode, TargetPinName);
	if (!SourcePin)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Source pin not found: %s"), *SourcePinName));
		return;
	}
	if (!TargetPin)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Target pin not found: %s"), *TargetPinName));
		return;
	}

	const UEdGraphSchema* Schema = Graph->GetSchema();
	const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Schema);
	if (!K2Schema)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Graph schema is not K2"));
		return;
	}

	const FPinConnectionResponse CanResp = K2Schema->CanCreateConnection(SourcePin, TargetPin);
	if (CanResp.Response == CONNECT_RESPONSE_DISALLOW)
	{
		UAL_CommandUtils::SendError(RequestId, 400, CanResp.Message.ToString());
		return;
	}

	const bool bConnected = K2Schema->TryCreateConnection(SourcePin, TargetPin);
	if (!bConnected)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create connection"));
		return;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("message"), TEXT("Connection created"));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 创建蓝图函数图表（用户自定义函数）
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - function_name: 函数名（必填）
 *   - inputs: [{ name, type, object_class?, is_array? }]（可选）
 *   - outputs: [{ name, type, object_class?, is_array? }]（可选）
 *   - pure: 是否纯函数（可选，默认 false）
 *
 * 响应:
 *   - ok
 *   - graph_name
 *   - entry_node_id
 *   - result_node_id
 */
void FUAL_BlueprintCommands::Handle_CreateFunctionGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString FunctionName;
	if (!Payload->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: function_name"));
		return;
	}

	bool bPure = false;
	Payload->TryGetBoolField(TEXT("pure"), bPure);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	// 重名检测：函数图表是否已存在
	{
		UEdGraph* Existing = UAL_FindGraph(Blueprint, FunctionName);
		if (Existing)
		{
			UAL_CommandUtils::SendError(RequestId, 409, FString::Printf(TEXT("Function graph already exists: %s"), *FunctionName));
			return;
		}
	}

	// 1) 创建函数图表
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint,
		FName(*FunctionName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);
	if (!NewGraph)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create function graph"));
		return;
	}

	// 添加到蓝图 FunctionGraphs
	// 添加到蓝图 FunctionGraphs
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	FBlueprintEditorUtils::AddFunctionGraph(Blueprint, NewGraph, /*bIsUserCreated*/ true, nullptr);
#else
	// UE5.0 needs explicit template arg for nullptr deduction
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated*/ true, nullptr);
#endif

	// 2) 获取/创建入口与返回节点
	UK2Node_FunctionEntry* EntryNode = nullptr;
	UK2Node_FunctionResult* ResultNode = nullptr;
	{
		TArray<UK2Node_FunctionEntry*> EntryNodes;
		NewGraph->GetNodesOfClass(EntryNodes);
		if (EntryNodes.Num() > 0)
		{
			EntryNode = EntryNodes[0];
		}

		TArray<UK2Node_FunctionResult*> ResultNodes;
		NewGraph->GetNodesOfClass(ResultNodes);
		if (ResultNodes.Num() > 0)
		{
			ResultNode = ResultNodes[0];
		}
	}

	auto EnsureEntryNode = [&]() -> UK2Node_FunctionEntry*
	{
		if (EntryNode)
		{
			return EntryNode;
		}
		FGraphNodeCreator<UK2Node_FunctionEntry> Creator(*NewGraph);
		UK2Node_FunctionEntry* N = Creator.CreateNode();
		N->NodePosX = 0;
		N->NodePosY = 0;
		Creator.Finalize();
		N->CreateNewGuid();
		N->PostPlacedNewNode();
		N->AllocateDefaultPins();
		N->ReconstructNode();
		EntryNode = N;
		return EntryNode;
	};

	auto EnsureResultNode = [&]() -> UK2Node_FunctionResult*
	{
		if (ResultNode)
		{
			return ResultNode;
		}
		FGraphNodeCreator<UK2Node_FunctionResult> Creator(*NewGraph);
		UK2Node_FunctionResult* N = Creator.CreateNode();
		N->NodePosX = 400;
		N->NodePosY = 0;
		Creator.Finalize();
		N->CreateNewGuid();
		N->PostPlacedNewNode();
		N->AllocateDefaultPins();
		N->ReconstructNode();
		ResultNode = N;
		return ResultNode;
	};

	EntryNode = EnsureEntryNode();
	ResultNode = EnsureResultNode();

	// 3) 设置 pure（best-effort：不同版本字段可能不同）
	if (bPure && EntryNode)
	{
		// 尝试设置 ExtraFlags |= FUNC_BlueprintPure
		if (FIntProperty* ExtraFlagsProp = FindFProperty<FIntProperty>(EntryNode->GetClass(), TEXT("ExtraFlags")))
		{
			const int32 Cur = ExtraFlagsProp->GetPropertyValue_InContainer(EntryNode);
			ExtraFlagsProp->SetPropertyValue_InContainer(EntryNode, Cur | (int32)FUNC_BlueprintPure);
		}
		// 尝试设置 bIsPureFunc = true
		if (FBoolProperty* PureProp = FindFProperty<FBoolProperty>(EntryNode->GetClass(), TEXT("bIsPureFunc")))
		{
			PureProp->SetPropertyValue_InContainer(EntryNode, true);
		}
		EntryNode->ReconstructNode();
	}

	// 4) 创建 inputs/outputs 用户定义引脚（best-effort）
	auto ApplyParamPins = [&](UK2Node_FunctionEntry* InEntry, UK2Node_FunctionResult* InResult)
	{
		const TArray<TSharedPtr<FJsonValue>>* InputsArr = nullptr;
		if (Payload->TryGetArrayField(TEXT("inputs"), InputsArr) && InputsArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *InputsArr)
			{
				const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
				if (!Obj.IsValid()) continue;

				FString Name, Type, ObjClass;
				Obj->TryGetStringField(TEXT("name"), Name);
				Obj->TryGetStringField(TEXT("type"), Type);
				Obj->TryGetStringField(TEXT("object_class"), ObjClass);

				bool bIsArrayLocal = false;
				Obj->TryGetBoolField(TEXT("is_array"), bIsArrayLocal);

				if (Name.IsEmpty() || Type.IsEmpty() || !InEntry) continue;

				FEdGraphPinType PinType;
				FString TypeError;
				if (!UAL_ParsePinTypeFromString(Type, ObjClass, PinType, TypeError))
				{
					UE_LOG(LogUALBlueprint, Warning, TEXT("[blueprint.create_function] Invalid input type %s: %s"), *Type, *TypeError);
					continue;
				}
				if (bIsArrayLocal)
				{
					PinType.ContainerType = EPinContainerType::Array;
				}

				// Entry 节点上的参数是 Output pin（从入口流出）
				InEntry->CreateUserDefinedPin(FName(*Name), PinType, EGPD_Output);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* OutputsArr = nullptr;
		if (Payload->TryGetArrayField(TEXT("outputs"), OutputsArr) && OutputsArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *OutputsArr)
			{
				const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
				if (!Obj.IsValid()) continue;

				FString Name, Type, ObjClass;
				Obj->TryGetStringField(TEXT("name"), Name);
				Obj->TryGetStringField(TEXT("type"), Type);
				Obj->TryGetStringField(TEXT("object_class"), ObjClass);

				bool bIsArrayLocal = false;
				Obj->TryGetBoolField(TEXT("is_array"), bIsArrayLocal);

				if (Name.IsEmpty() || Type.IsEmpty() || !InResult) continue;

				FEdGraphPinType PinType;
				FString TypeError;
				if (!UAL_ParsePinTypeFromString(Type, ObjClass, PinType, TypeError))
				{
					UE_LOG(LogUALBlueprint, Warning, TEXT("[blueprint.create_function] Invalid output type %s: %s"), *Type, *TypeError);
					continue;
				}
				if (bIsArrayLocal)
				{
					PinType.ContainerType = EPinContainerType::Array;
				}

				// Result 节点上的返回值是 Input pin（流入返回节点）
				InResult->CreateUserDefinedPin(FName(*Name), PinType, EGPD_Input);
			}
		}
	};

	if (EntryNode && ResultNode)
	{
		ApplyParamPins(EntryNode, ResultNode);
		EntryNode->ReconstructNode();
		ResultNode->ReconstructNode();
	}

	// 5) 编译更新
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// 6) 返回
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("graph_name"), FunctionName);
	Result->SetStringField(TEXT("entry_node_id"), EntryNode ? UAL_GuidToString(EntryNode->NodeGuid) : TEXT(""));
	Result->SetStringField(TEXT("result_node_id"), ResultNode ? UAL_GuidToString(ResultNode->NodeGuid) : TEXT(""));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 编译蓝图并可选保存
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - save: 编译成功后是否保存（可选，默认 true）
 *
 * 响应:
 *   - ok: 是否编译成功（状态为 UpToDate）
 *   - status: 编译状态 (UpToDate / Dirty / Error / Unknown / Other)
 *   - saved: 是否已保存
 *   - path: 蓝图路径
 */
void FUAL_BlueprintCommands::Handle_CompileBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析参数
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing blueprint_path"));
		return;
	}

	bool bSave = true;
	Payload->TryGetBoolField(TEXT("save"), bSave);

	// 2. 加载蓝图
	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath = BlueprintPath;

	if (BlueprintPath.StartsWith(TEXT("/")))
	{
		if (!BlueprintPath.Contains(TEXT(".")))
		{
			ResolvedPath = BlueprintPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPath);
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *ResolvedPath);
	}

	// 如果直接加载失败，通过 AssetRegistry 查找
	if (!Blueprint)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FAssetData> AssetList;
		FARFilter Filter;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
		Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, AssetList);

		for (const FAssetData& Asset : AssetList)
		{
			FString AssetName = Asset.AssetName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			FString AssetPath = Asset.GetObjectPathString();
#else
			FString AssetPath = Asset.ObjectPath.ToString();
#endif
			if (AssetName.Equals(BlueprintPath, ESearchCase::IgnoreCase) ||
				AssetPath.Contains(BlueprintPath))
			{
				Blueprint = Cast<UBlueprint>(Asset.GetAsset());
				ResolvedPath = AssetPath;
				break;
			}
		}
	}

	if (!Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	// 3. 执行编译（带诊断日志）
	FCompilerResultsLog ResultsLog;
	ResultsLog.bSilentMode = true;
	ResultsLog.bLogInfoOnly = false;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	ResultsLog.bAnnotateMentionedNodes = true;
#endif
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 2)
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);
#else
	// UE5.0/5.1 兼容
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);
#endif

	// 3.1 收集 diagnostics（用于 Agent 自修复）
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const TSharedRef<FTokenizedMessage>& Msg : ResultsLog.Messages)
	{
		TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();

		FString SeverityStr = TEXT("Info");
		switch (Msg->GetSeverity())
		{
		case EMessageSeverity::Error:   SeverityStr = TEXT("Error"); break;
		case EMessageSeverity::Warning: SeverityStr = TEXT("Warning"); break;
		case EMessageSeverity::Info:    SeverityStr = TEXT("Info"); break;
		default:                        SeverityStr = TEXT("Other"); break;
		}
		D->SetStringField(TEXT("type"), SeverityStr);
		D->SetStringField(TEXT("message"), Msg->ToText().ToString());

		// 尝试绑定 node_id（best-effort）
		FString NodeId;
		FString PinName;
		UObject* FoundObj = nullptr;

		for (const TSharedRef<IMessageToken>& Tok : Msg->GetMessageTokens())
		{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			if (Tok->GetType() == EMessageToken::Object)
			{
				const TSharedRef<FUObjectToken> ObjTok = StaticCastSharedRef<FUObjectToken>(Tok);
				FoundObj = ObjTok->GetObject().Get();
				if (FoundObj)
				{
					break;
				}
			}
#else
			// UE5.0: FObjectToken/FUObjectToken 不可用，跳过对象绑定
			(void)Tok;
#endif
		}

		if (UEdGraphNode* AsNode = Cast<UEdGraphNode>(FoundObj))
		{
			NodeId = UAL_GuidToString(AsNode->NodeGuid);
		}

		if (!NodeId.IsEmpty())
		{
			D->SetStringField(TEXT("node_id"), NodeId);
		}
		if (!PinName.IsEmpty())
		{
			D->SetStringField(TEXT("pin"), PinName);
		}

		Diagnostics.Add(MakeShared<FJsonValueObject>(D));
	}

	// 4. 检查结果状态
	const bool bCompileSuccess = (Blueprint->Status == BS_UpToDate);

	FString StatusStr;
	switch (Blueprint->Status)
	{
	case BS_UpToDate: StatusStr = TEXT("UpToDate"); break;
	case BS_Dirty:    StatusStr = TEXT("Dirty"); break;
	case BS_Error:    StatusStr = TEXT("Error"); break;
	case BS_Unknown:  StatusStr = TEXT("Unknown"); break;
	default:          StatusStr = TEXT("Other"); break;
	}

	// 5. 保存（仅在请求要求且编译成功时执行，避免写入坏蓝图）
	bool bSaved = false;
	if (bSave && bCompileSuccess)
	{
		if (UPackage* Package = Blueprint->GetOutermost())
		{
			const FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs);
			bSaved = true;
		}
	}

	// 6. 返回结果
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), bCompileSuccess);
	Result->SetStringField(TEXT("status"), StatusStr);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
	Result->SetArrayField(TEXT("diagnostics"), Diagnostics);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

// ============================================================================
// 辅助函数实现
// ============================================================================

/**
 * 收集蓝图的所有组件信息（包括 SCS 添加的和父类继承的）
 */
TArray<TSharedPtr<FJsonValue>> FUAL_BlueprintCommands::CollectComponentsInfo(UBlueprint* Blueprint)
{
	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	if (!Blueprint) return ComponentsArray;

	TSet<FString> AddedNames; // 避免重复

	// 1. 收集 SCS 添加的组件
	if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : SCS->GetAllNodes())
		{
			if (!Node) continue;
			
			TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
			FString CompName = Node->GetVariableName().ToString();
			
			CompObj->SetStringField(TEXT("name"), CompName);
			CompObj->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown"));
			CompObj->SetStringField(TEXT("class_path"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : TEXT(""));
			CompObj->SetStringField(TEXT("source"), TEXT("added")); // SCS 添加的
			CompObj->SetBoolField(TEXT("editable"), true);
			CompObj->SetStringField(TEXT("attach_to"), Node->ParentComponentOrVariableName.ToString());
			
			ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
			AddedNames.Add(CompName);
		}
	}

	// 2. 收集父类继承的组件（从 CDO 中获取）
	if (Blueprint->GeneratedClass)
	{
		if (UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject())
		{
			TArray<UObject*> SubObjects;
			GetObjectsWithOuter(CDO, SubObjects, false);
			
			for (UObject* SubObj : SubObjects)
			{
				UActorComponent* Component = Cast<UActorComponent>(SubObj);
				if (!Component) continue;
				
				FString CompName = Component->GetName();
				if (AddedNames.Contains(CompName)) continue; // 跳过已添加的
				
				TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
				CompObj->SetStringField(TEXT("name"), CompName);
				CompObj->SetStringField(TEXT("class"), Component->GetClass()->GetName());
				CompObj->SetStringField(TEXT("class_path"), Component->GetClass()->GetPathName());
				CompObj->SetStringField(TEXT("source"), TEXT("inherited")); // 继承的
				CompObj->SetBoolField(TEXT("editable"), true);
				
				ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
				AddedNames.Add(CompName);
			}
		}
	}

	return ComponentsArray;
}

/**
 * 收集蓝图的变量列表
 */
TArray<TSharedPtr<FJsonValue>> FUAL_BlueprintCommands::CollectVariablesInfo(UBlueprint* Blueprint)
{
	TArray<TSharedPtr<FJsonValue>> VariablesArray;
	if (!Blueprint) return VariablesArray;

	// 遍历 NewVariables（蓝图定义的变量）
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());
		VarObj->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
		VarObj->SetBoolField(TEXT("editable"), true);
		
		// 尝试获取默认值
		if (!Var.DefaultValue.IsEmpty())
		{
			VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
		}
		
		VariablesArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}

	return VariablesArray;
}

/**
 * 构建蓝图结构 JSON 对象
 * 被 describe、create、add_component 复用
 */
TSharedPtr<FJsonObject> FUAL_BlueprintCommands::BuildBlueprintStructureJson(
	UBlueprint* Blueprint, 
	bool bIncludeVariables, 
	bool bIncludeComponentDetails)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Blueprint)
	{
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("error"), TEXT("Invalid blueprint"));
		return Result;
	}

	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
	
	// 父类信息
	if (Blueprint->ParentClass)
	{
		Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass->GetName());
		Result->SetStringField(TEXT("parent_class_path"), Blueprint->ParentClass->GetPathName());
	}
	
	// 生成的类
	if (Blueprint->GeneratedClass)
	{
		Result->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass->GetPathName());
	}

	// 组件列表
	Result->SetArrayField(TEXT("components"), CollectComponentsInfo(Blueprint));
	
	// 变量列表（可选）
	if (bIncludeVariables)
	{
		Result->SetArrayField(TEXT("variables"), CollectVariablesInfo(Blueprint));
	}
	
	// 编译状态
	FString StatusStr;
	switch (Blueprint->Status)
	{
	case BS_UpToDate: StatusStr = TEXT("UpToDate"); break;
	case BS_Dirty:    StatusStr = TEXT("Dirty"); break;
	case BS_Error:    StatusStr = TEXT("Error"); break;
	case BS_Unknown:  StatusStr = TEXT("Unknown"); break;
	default:          StatusStr = TEXT("Other"); break;
	}
	Result->SetStringField(TEXT("compile_status"), StatusStr);

	return Result;
}

// ============================================================================
// blueprint.describe 命令处理
// ============================================================================

/**
 * 获取蓝图完整结构信息
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *
 * 响应:
 *   - ok: 是否成功
 *   - name: 蓝图名称
 *   - path: 蓝图完整路径
 *   - parent_class: 父类名称
 *   - parent_class_path: 父类完整路径
 *   - components: 组件列表（包含名称、类型、来源等）
 *   - variables: 变量列表（包含名称、类型、默认值等）
 *   - compile_status: 编译状态
 */
void FUAL_BlueprintCommands::Handle_DescribeBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析参数
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	// 2. 加载蓝图
	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath = BlueprintPath;

	if (BlueprintPath.StartsWith(TEXT("/")))
	{
		if (!BlueprintPath.Contains(TEXT(".")))
		{
			ResolvedPath = BlueprintPath + TEXT(".") + FPaths::GetBaseFilename(BlueprintPath);
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *ResolvedPath);
	}

	// 如果直接加载失败，通过 AssetRegistry 查找
	if (!Blueprint)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FAssetData> AssetList;
		FARFilter Filter;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
#else
		Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
#endif
		Filter.bRecursiveClasses = true;
		AssetRegistry.GetAssets(Filter, AssetList);

		for (const FAssetData& Asset : AssetList)
		{
			FString AssetName = Asset.AssetName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			FString AssetPath = Asset.GetObjectPathString();
#else
			FString AssetPath = Asset.ObjectPath.ToString();
#endif
			if (AssetName.Equals(BlueprintPath, ESearchCase::IgnoreCase) ||
				AssetPath.Contains(BlueprintPath))
			{
				Blueprint = Cast<UBlueprint>(Asset.GetAsset());
				ResolvedPath = AssetPath;
				break;
			}
		}
	}

	if (!Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	// 3. 构建并返回结构信息
	TSharedPtr<FJsonObject> Result = BuildBlueprintStructureJson(Blueprint, true, false);
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

/**
 * 设置节点 Pin 的默认值
 *
 * 请求参数:
 *   - blueprint_path: 蓝图路径（必填）
 *   - graph_name: 图表名（可选，默认 EventGraph）
 *   - node_id: 节点 GUID（必填）
 *   - pin_name: 引脚名称（必填）
 *   - value: 要设置的值（必填，字符串形式）
 *
 * 响应:
 *   - ok
 *   - node_id
 *   - pin_name
 *   - old_value
 *   - new_value
 */
void FUAL_BlueprintCommands::Handle_SetPinValue(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintPath;
	if (!Payload->TryGetStringField(TEXT("blueprint_path"), BlueprintPath) || BlueprintPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: blueprint_path"));
		return;
	}

	FString NodeId;
	if (!Payload->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_id"));
		return;
	}

	FString PinName;
	if (!Payload->TryGetStringField(TEXT("pin_name"), PinName) || PinName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: pin_name"));
		return;
	}

	FString Value;
	if (!Payload->TryGetStringField(TEXT("value"), Value))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: value"));
		return;
	}

	FString GraphName;
	Payload->TryGetStringField(TEXT("graph_name"), GraphName);

	UBlueprint* Blueprint = nullptr;
	FString ResolvedPath;
	if (!UAL_LoadBlueprintByPathOrName(BlueprintPath, Blueprint, ResolvedPath) || !Blueprint)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
		return;
	}

	UEdGraph* Graph = UAL_FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		return;
	}

	UEdGraphNode* Node = UAL_FindNodeByGuid(Graph, NodeId);
	if (!Node)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Node not found: %s"), *NodeId));
		return;
	}

	UEdGraphPin* Pin = UAL_FindPinByName(Node, PinName);
	if (!Pin)
	{
		// 收集可用的 Pin 名称
		TArray<TSharedPtr<FJsonValue>> AvailablePins;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P && P->Direction == EGPD_Input)
			{
				AvailablePins.Add(MakeShared<FJsonValueString>(P->PinName.ToString()));
			}
		}
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetArrayField(TEXT("available_input_pins"), AvailablePins);
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Pin not found: %s"), *PinName), Details);
		return;
	}

	// 只允许设置输入引脚
	if (Pin->Direction != EGPD_Input)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Can only set default value for Input pins"));
		return;
	}

	// 保存旧值
	FString OldValue = Pin->DefaultValue;

	// 设置新值
	Pin->DefaultValue = Value;
	
	// 如果是对象类型的引脚，可能需要设置 DefaultObject
	// 这里暂时只处理简单值

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("node_id"), NodeId);
	Result->SetStringField(TEXT("pin_name"), PinName);
	Result->SetStringField(TEXT("old_value"), OldValue);
	Result->SetStringField(TEXT("new_value"), Value);
	Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Set %s to \"%s\""), *PinName, *Value));
	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

