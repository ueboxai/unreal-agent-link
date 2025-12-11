#include "UAL_BlueprintCommands.h"
#include "UAL_CommandUtils.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetToolsModule.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "Engine/LevelScriptActor.h"
#include "AssetRegistry/AssetRegistryModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALBlueprint, Log, All);

void FUAL_BlueprintCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
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

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), BlueprintName);
	Result->SetStringField(TEXT("path"), PackageName);
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
	Result->SetBoolField(TEXT("saved"), true);

	TArray<TSharedPtr<FJsonValue>> CompResults;
	if (Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			NodeObj->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown"));
			NodeObj->SetStringField(TEXT("attach_to"), Node->ParentComponentOrVariableName.ToString());
			CompResults.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
	}
	Result->SetArrayField(TEXT("components"), CompResults);
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
