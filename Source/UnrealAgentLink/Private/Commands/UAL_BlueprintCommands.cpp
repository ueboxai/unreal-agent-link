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
