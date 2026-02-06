#include "UAL_MaterialCommands.h"
#include "UAL_CommandUtils.h"
#include "Utils/UAL_PBRMaterialHelper.h"

#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Factories/MaterialFactoryNew.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionDotProduct.h"
#include "Materials/MaterialExpressionCrossProduct.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "UObject/SavePackage.h"
#include "PropertyEditorModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "AssetToolsModule.h"
#include "ScopedTransaction.h"
#include "MaterialEditorUtilities.h"
#include "MaterialGraph/MaterialGraph.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALMaterial, Log, All);

/**
 * 注册所有材质相关命令
 */
void FUAL_MaterialCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("material.create"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateMaterial(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.apply"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ApplyMaterial(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.describe"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DescribeMaterial(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.set_param"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetMaterialParam(Payload, RequestId);
	});

	// Phase 1: 材质图表编辑命令
	CommandMap.Add(TEXT("material.get_graph"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetMaterialGraph(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.add_node"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddMaterialNode(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.connect_pins"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ConnectMaterialPins(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.compile"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CompileMaterial(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.set_node_value"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetMaterialNodeValue(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.delete_node"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DeleteMaterialNode(Payload, RequestId);
	});

	// Phase 2: 材质管理命令（智能容错）
	CommandMap.Add(TEXT("material.duplicate"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DuplicateMaterial(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.set_property"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetMaterialProperty(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.create_instance"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateMaterialInstance(Payload, RequestId);
	});

	// Phase 3: 材质查询和预览命令
	CommandMap.Add(TEXT("material.list"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ListMaterials(Payload, RequestId);
	});

	CommandMap.Add(TEXT("material.preview"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_PreviewMaterial(Payload, RequestId);
	});

	UE_LOG(LogUALMaterial, Log, TEXT("Registered 15 material commands"));
}

// ============================================================================
// Handle_CreateMaterial - 创建 UMaterial（母材质）
// ============================================================================
void FUAL_MaterialCommands::Handle_CreateMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数: material_name
	FString MaterialName;
	if (!Payload->TryGetStringField(TEXT("material_name"), MaterialName) || MaterialName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_name"));
		return;
	}

	// 2. 解析可选参数
	FString DestinationPath = TEXT("/Game/Materials");
	Payload->TryGetStringField(TEXT("destination_path"), DestinationPath);

	FString BlendModeStr;
	Payload->TryGetStringField(TEXT("blend_mode"), BlendModeStr);

	FString ShadingModelStr;
	Payload->TryGetStringField(TEXT("shading_model"), ShadingModelStr);

	bool bTwoSided = false;
	Payload->TryGetBoolField(TEXT("two_sided"), bTwoSided);

	// 3. 创建材质包
	FString PackagePath = DestinationPath / MaterialName;
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create package for material"));
		return;
	}

	// 4. 创建 UMaterial
	FScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "CreateMaterial", "Create Material"));

	UMaterialFactoryNew* MaterialFactory = NewObject<UMaterialFactoryNew>();
	UMaterial* NewMaterial = Cast<UMaterial>(MaterialFactory->FactoryCreateNew(
		UMaterial::StaticClass(),
		Package,
		FName(*MaterialName),
		RF_Public | RF_Standalone,
		nullptr,
		GWarn
	));

	if (!NewMaterial)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create UMaterial"));
		return;
	}

	// 标记新对象的修改
	NewMaterial->PreEditChange(nullptr);
	NewMaterial->Modify();

	// 5. 设置材质属性
	if (!BlendModeStr.IsEmpty())
	{
		EBlendMode BlendMode;
		if (ParseBlendMode(BlendModeStr, BlendMode))
		{
			NewMaterial->BlendMode = BlendMode;
		}
	}

	if (!ShadingModelStr.IsEmpty())
	{
		EMaterialShadingModel ShadingModel;
		if (ParseShadingModel(ShadingModelStr, ShadingModel))
		{
			NewMaterial->SetShadingModel(ShadingModel);
		}
	}

	NewMaterial->TwoSided = bTwoSided;

	// 6. 标记已修改并保存
	FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
	NewMaterial->PostEditChangeProperty(PropertyChangedEvent);
	
	// 强制刷新材质编辑器图表和逻辑
	if (NewMaterial && NewMaterial->MaterialGraph)
	{
		FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(NewMaterial->MaterialGraph);
	}
	
	NewMaterial->MarkPackageDirty();

	// 通知资产注册表
	FAssetRegistryModule::AssetCreated(NewMaterial);

	// 仅通知属性面板刷新
	if (GEditor)
	{
		FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
		if (PropertyModule)
		{
			PropertyModule->NotifyCustomizationModuleChanged();
		}
	}

	// 7. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_name"), NewMaterial->GetName());
	Data->SetStringField(TEXT("material_path"), NewMaterial->GetPathName());
	Data->SetStringField(TEXT("material_type"), TEXT("UMaterial"));
	Data->SetStringField(TEXT("blend_mode"), StaticEnum<EBlendMode>()->GetNameStringByValue((int64)NewMaterial->BlendMode));
	Data->SetBoolField(TEXT("two_sided"), NewMaterial->TwoSided);

	// 返回可用的材质引脚
	TArray<TSharedPtr<FJsonValue>> AvailablePins;
	AvailablePins.Add(MakeShared<FJsonValueString>(TEXT("BaseColor")));
	AvailablePins.Add(MakeShared<FJsonValueString>(TEXT("Metallic")));
	AvailablePins.Add(MakeShared<FJsonValueString>(TEXT("Roughness")));
	AvailablePins.Add(MakeShared<FJsonValueString>(TEXT("Normal")));
	AvailablePins.Add(MakeShared<FJsonValueString>(TEXT("EmissiveColor")));
	AvailablePins.Add(MakeShared<FJsonValueString>(TEXT("Opacity")));
	AvailablePins.Add(MakeShared<FJsonValueString>(TEXT("OpacityMask")));
	AvailablePins.Add(MakeShared<FJsonValueString>(TEXT("AmbientOcclusion")));
	AvailablePins.Add(MakeShared<FJsonValueString>(TEXT("WorldPositionOffset")));
	AvailablePins.Add(MakeShared<FJsonValueString>(TEXT("Specular")));
	Data->SetArrayField(TEXT("available_pins"), AvailablePins);

	UE_LOG(LogUALMaterial, Log, TEXT("Created UMaterial: %s"), *NewMaterial->GetName());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}


// ============================================================================
// Handle_ApplyMaterial - 将材质应用到 Actor
// ============================================================================
void FUAL_MaterialCommands::Handle_ApplyMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数: material_path
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	// 2. 加载材质
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// 3. 解析 targets 选择器
	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: targets"));
		return;
	}

	// 4. 获取目标世界
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	// 5. 解析 Actor
	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!UAL_CommandUtils::ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError))
	{
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	// 6. 解析可选参数
	int32 SlotIndex = 0;
	Payload->TryGetNumberField(TEXT("slot_index"), SlotIndex);

	// 7. 应用材质到每个 Actor
	int32 AppliedCount = 0;
	TArray<TSharedPtr<FJsonValue>> ActorsJson;

	for (AActor* Actor : TargetSet)
	{
		if (!Actor) continue;

		// 查找 StaticMeshComponent
		UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>();
		if (!SMC)
		{
			UE_LOG(LogUALMaterial, Warning, TEXT("Actor %s has no StaticMeshComponent"), 
				*UAL_CommandUtils::GetActorFriendlyName(Actor));
			continue;
		}

		// 验证 slot 索引
		int32 NumMaterials = SMC->GetNumMaterials();
		if (SlotIndex < 0 || SlotIndex >= NumMaterials)
		{
			UE_LOG(LogUALMaterial, Warning, TEXT("Invalid slot index %d for Actor %s (has %d slots)"),
				SlotIndex, *UAL_CommandUtils::GetActorFriendlyName(Actor), NumMaterials);
			continue;
		}

		// 应用材质
		SMC->SetMaterial(SlotIndex, Material);
#if WITH_EDITOR
		Actor->Modify();
#endif

		AppliedCount++;

		// 构建响应
		TSharedPtr<FJsonObject> ActorInfo = MakeShared<FJsonObject>();
		ActorInfo->SetStringField(TEXT("name"), UAL_CommandUtils::GetActorFriendlyName(Actor));
		ActorInfo->SetStringField(TEXT("path"), Actor->GetPathName());
		ActorInfo->SetNumberField(TEXT("slot_index"), SlotIndex);
		ActorsJson.Add(MakeShared<FJsonValueObject>(ActorInfo));

		UE_LOG(LogUALMaterial, Log, TEXT("Applied material %s to %s at slot %d"),
			*Material->GetName(), *UAL_CommandUtils::GetActorFriendlyName(Actor), SlotIndex);
	}

	// 8. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("applied_count"), AppliedCount);
	Data->SetNumberField(TEXT("target_count"), TargetSet.Num());
	Data->SetStringField(TEXT("material_path"), MaterialPath);
	Data->SetArrayField(TEXT("actors"), ActorsJson);

	UAL_CommandUtils::SendResponse(RequestId, AppliedCount > 0 ? 200 : 404, Data);
}

// ============================================================================
// Handle_DescribeMaterial - 获取材质详细信息
// ============================================================================
void FUAL_MaterialCommands::Handle_DescribeMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数: path
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	// 2. 加载材质
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// 3. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Material->GetName());
	Data->SetStringField(TEXT("path"), Material->GetPathName());
	Data->SetStringField(TEXT("class"), Material->GetClass()->GetName());

	// 4. 如果是 MaterialInstance，获取父材质
	UMaterialInstance* MatInst = Cast<UMaterialInstance>(Material);
	if (MatInst && MatInst->Parent)
	{
		Data->SetStringField(TEXT("parent_material"), MatInst->Parent->GetPathName());
	}

	// 5. 获取材质参数
	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid> ScalarGuids;
	Material->GetAllScalarParameterInfo(ScalarInfos, ScalarGuids);

	TArray<FMaterialParameterInfo> VectorInfos;
	TArray<FGuid> VectorGuids;
	Material->GetAllVectorParameterInfo(VectorInfos, VectorGuids);

	TArray<FMaterialParameterInfo> TextureInfos;
	TArray<FGuid> TextureGuids;
	Material->GetAllTextureParameterInfo(TextureInfos, TextureGuids);

	// 6. 构建标量参数列表
	TArray<TSharedPtr<FJsonValue>> ScalarParams;
	for (const FMaterialParameterInfo& Info : ScalarInfos)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Info.Name.ToString());
		
		float Value = 0.0f;
		if (Material->GetScalarParameterValue(Info, Value))
		{
			ParamObj->SetNumberField(TEXT("value"), Value);
		}
		ScalarParams.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	Data->SetArrayField(TEXT("scalar_params"), ScalarParams);

	// 7. 构建向量参数列表
	TArray<TSharedPtr<FJsonValue>> VectorParams;
	for (const FMaterialParameterInfo& Info : VectorInfos)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Info.Name.ToString());
		
		FLinearColor Value;
		if (Material->GetVectorParameterValue(Info, Value))
		{
			TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
			ColorObj->SetNumberField(TEXT("r"), Value.R);
			ColorObj->SetNumberField(TEXT("g"), Value.G);
			ColorObj->SetNumberField(TEXT("b"), Value.B);
			ColorObj->SetNumberField(TEXT("a"), Value.A);
			ParamObj->SetObjectField(TEXT("value"), ColorObj);
		}
		VectorParams.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	Data->SetArrayField(TEXT("vector_params"), VectorParams);

	// 8. 构建贴图参数列表
	TArray<TSharedPtr<FJsonValue>> TextureParams;
	for (const FMaterialParameterInfo& Info : TextureInfos)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Info.Name.ToString());
		
		UTexture* Texture = nullptr;
		if (Material->GetTextureParameterValue(Info, Texture) && Texture)
		{
			ParamObj->SetStringField(TEXT("value"), Texture->GetPathName());
		}
		else
		{
			ParamObj->SetStringField(TEXT("value"), TEXT(""));
		}
		TextureParams.Add(MakeShared<FJsonValueObject>(ParamObj));
	}
	Data->SetArrayField(TEXT("texture_params"), TextureParams);

	UE_LOG(LogUALMaterial, Log, TEXT("Described material: %s (Scalars: %d, Vectors: %d, Textures: %d)"),
		*Material->GetName(), ScalarInfos.Num(), VectorInfos.Num(), TextureInfos.Num());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_SetMaterialParam - 设置材质参数
// ============================================================================
void FUAL_MaterialCommands::Handle_SetMaterialParam(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数: path
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	// 2. 加载材质（必须是 MaterialInstanceConstant）
	UMaterialInstanceConstant* MatInst = LoadObject<UMaterialInstanceConstant>(nullptr, *MaterialPath);
	if (!MatInst)
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			FString::Printf(TEXT("Material must be a MaterialInstanceConstant: %s"), *MaterialPath));
		return;
	}

	// 3. 解析 params 对象
	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("params"), ParamsObj) || !ParamsObj || !ParamsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: params"));
		return;
	}

	// 4. 遍历并设置参数
	TArray<FString> UpdatedParams;
	TArray<TSharedPtr<FJsonValue>> Errors;

	for (const auto& Pair : (*ParamsObj)->Values)
	{
		const FString& ParamName = Pair.Key;
		const TSharedPtr<FJsonValue>& ParamValue = Pair.Value;

		if (!ParamValue.IsValid()) continue;

		bool bSuccess = false;

		// 尝试作为标量处理
		double ScalarValue;
		if (ParamValue->TryGetNumber(ScalarValue))
		{
			MatInst->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(*ParamName), (float)ScalarValue);
			UpdatedParams.Add(ParamName);
			bSuccess = true;
			UE_LOG(LogUALMaterial, Log, TEXT("Set scalar param %s = %f"), *ParamName, ScalarValue);
		}

		// 尝试作为向量/颜色处理 (对象格式 { r, g, b, a })
		const TSharedPtr<FJsonObject>* ColorObj = nullptr;
		if (!bSuccess && ParamValue->TryGetObject(ColorObj) && ColorObj && ColorObj->IsValid())
		{
			FLinearColor Color;
			Color.R = (*ColorObj)->HasField(TEXT("r")) ? (*ColorObj)->GetNumberField(TEXT("r")) : 0.0f;
			Color.G = (*ColorObj)->HasField(TEXT("g")) ? (*ColorObj)->GetNumberField(TEXT("g")) : 0.0f;
			Color.B = (*ColorObj)->HasField(TEXT("b")) ? (*ColorObj)->GetNumberField(TEXT("b")) : 0.0f;
			Color.A = (*ColorObj)->HasField(TEXT("a")) ? (*ColorObj)->GetNumberField(TEXT("a")) : 1.0f;

			MatInst->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(*ParamName), Color);
			UpdatedParams.Add(ParamName);
			bSuccess = true;
			UE_LOG(LogUALMaterial, Log, TEXT("Set vector param %s = (%f, %f, %f, %f)"), 
				*ParamName, Color.R, Color.G, Color.B, Color.A);
		}

		// 尝试作为贴图路径处理
		FString TexturePath;
		if (!bSuccess && ParamValue->TryGetString(TexturePath) && !TexturePath.IsEmpty())
		{
			UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
			if (Texture)
			{
				MatInst->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(*ParamName), Texture);
				UpdatedParams.Add(ParamName);
				bSuccess = true;
				UE_LOG(LogUALMaterial, Log, TEXT("Set texture param %s = %s"), *ParamName, *TexturePath);
			}
			else
			{
				TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
				Err->SetStringField(TEXT("param"), ParamName);
				Err->SetStringField(TEXT("error"), FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
				Errors.Add(MakeShared<FJsonValueObject>(Err));
			}
		}

		// 记录未识别的参数
		if (!bSuccess && Errors.Num() == 0)
		{
			TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
			Err->SetStringField(TEXT("param"), ParamName);
			Err->SetStringField(TEXT("error"), TEXT("Unrecognized parameter type"));
			Errors.Add(MakeShared<FJsonValueObject>(Err));
		}
	}

	// 5. 保存更改
	MatInst->PostEditChange();
	MatInst->MarkPackageDirty();

	// 6. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	
	TArray<TSharedPtr<FJsonValue>> UpdatedArray;
	for (const FString& Name : UpdatedParams)
	{
		UpdatedArray.Add(MakeShared<FJsonValueString>(Name));
	}
	Data->SetArrayField(TEXT("updated_params"), UpdatedArray);
	Data->SetArrayField(TEXT("errors"), Errors);
	Data->SetStringField(TEXT("material_path"), MaterialPath);

	UE_LOG(LogUALMaterial, Log, TEXT("Set %d parameters on material %s"), UpdatedParams.Num(), *MatInst->GetName());

	UAL_CommandUtils::SendResponse(RequestId, UpdatedParams.Num() > 0 ? 200 : 400, Data);
}

// ============================================================================
// Handle_GetMaterialGraph - 获取材质图表结构
// ============================================================================
void FUAL_MaterialCommands::Handle_GetMaterialGraph(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数: path
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	// 2. 加载材质（必须是 UMaterial，不能是 Instance）
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		// 尝试作为 MaterialInstance 加载并提示
		UMaterialInstance* MatInst = LoadObject<UMaterialInstance>(nullptr, *MaterialPath);
		if (MatInst)
		{
			UAL_CommandUtils::SendError(RequestId, 400, 
				TEXT("Cannot get graph from MaterialInstance. Use the parent Material path instead."));
			return;
		}
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// 3. 解析可选参数
	bool bIncludeValues = true;
	Payload->TryGetBoolField(TEXT("include_values"), bIncludeValues);

	// 4. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_path"), Material->GetPathName());
	Data->SetStringField(TEXT("material_name"), Material->GetName());

	// 5. 遍历所有材质表达式节点
	TArray<TSharedPtr<FJsonValue>> NodesJson;
	TArray<TSharedPtr<FJsonValue>> ConnectionsJson;
	TMap<UMaterialExpression*, FString> ExpressionToId;

	int32 NodeIndex = 0;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (UMaterialExpression* Expression : Material->GetExpressions())
#else
	for (UMaterialExpression* Expression : Material->Expressions)
#endif
	{
		if (!Expression) continue;

		// 生成节点 ID
		FString NodeId = FString::Printf(TEXT("%s_%d"), *Expression->GetClass()->GetName(), NodeIndex++);
		ExpressionToId.Add(Expression, NodeId);

		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("node_id"), NodeId);
		NodeObj->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
		NodeObj->SetStringField(TEXT("display_name"), Expression->GetName());

		// 位置
		TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
		PosObj->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
		PosObj->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
		NodeObj->SetObjectField(TEXT("position"), PosObj);

		// 描述（如果有）
		if (!Expression->Desc.IsEmpty())
		{
			NodeObj->SetStringField(TEXT("description"), Expression->Desc);
		}

		NodesJson.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	Data->SetArrayField(TEXT("nodes"), NodesJson);
	Data->SetNumberField(TEXT("node_count"), NodesJson.Num());

	// 6. 材质主节点可用引脚
	TArray<TSharedPtr<FJsonValue>> MaterialPins;
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("BaseColor")));
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("Metallic")));
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("Specular")));
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("Roughness")));
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("EmissiveColor")));
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("Opacity")));
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("OpacityMask")));
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("Normal")));
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("WorldPositionOffset")));
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("SubsurfaceColor")));
	MaterialPins.Add(MakeShared<FJsonValueString>(TEXT("AmbientOcclusion")));
	Data->SetArrayField(TEXT("material_pins"), MaterialPins);

	// 7. 连接数量（简化版本，后续可扩展）
	Data->SetNumberField(TEXT("connection_count"), 0);
	Data->SetArrayField(TEXT("connections"), ConnectionsJson);

	UE_LOG(LogUALMaterial, Log, TEXT("Got material graph: %s with %d nodes"), 
		*Material->GetName(), NodesJson.Num());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_AddMaterialNode - 添加材质表达式节点
// ============================================================================
void FUAL_MaterialCommands::Handle_AddMaterialNode(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	FString NodeType;
	if (!Payload->TryGetStringField(TEXT("node_type"), NodeType) || NodeType.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_type"));
		return;
	}

	// 2. 加载材质
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// 3. 解析可选参数
	FString NodeName;
	Payload->TryGetStringField(TEXT("node_name"), NodeName);

	int32 PosX = 0, PosY = 0;
	const TSharedPtr<FJsonObject>* PositionObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("position"), PositionObj) && PositionObj)
	{
		(*PositionObj)->TryGetNumberField(TEXT("x"), PosX);
		(*PositionObj)->TryGetNumberField(TEXT("y"), PosY);
	}

	FString TexturePath;
	Payload->TryGetStringField(TEXT("texture_path"), TexturePath);

	// 4. 根据 NodeType 创建表达式
	UMaterialExpression* NewExpression = nullptr;
	UClass* ExpressionClass = nullptr;

	// 映射 node_type 到 UMaterialExpression 类
	static TMap<FString, UClass*> NodeTypeMap;
	if (NodeTypeMap.Num() == 0)
	{
		NodeTypeMap.Add(TEXT("Constant"), UMaterialExpressionConstant::StaticClass());
		NodeTypeMap.Add(TEXT("Constant3Vector"), UMaterialExpressionConstant3Vector::StaticClass());
		NodeTypeMap.Add(TEXT("Constant4Vector"), UMaterialExpressionConstant4Vector::StaticClass());
		NodeTypeMap.Add(TEXT("ScalarParameter"), UMaterialExpressionScalarParameter::StaticClass());
		NodeTypeMap.Add(TEXT("VectorParameter"), UMaterialExpressionVectorParameter::StaticClass());
		NodeTypeMap.Add(TEXT("TextureSample"), UMaterialExpressionTextureSample::StaticClass());
		NodeTypeMap.Add(TEXT("TextureSampleParameter2D"), UMaterialExpressionTextureSampleParameter2D::StaticClass());
		NodeTypeMap.Add(TEXT("TextureCoordinate"), UMaterialExpressionTextureCoordinate::StaticClass());
		NodeTypeMap.Add(TEXT("Add"), UMaterialExpressionAdd::StaticClass());
		NodeTypeMap.Add(TEXT("Subtract"), UMaterialExpressionSubtract::StaticClass());
		NodeTypeMap.Add(TEXT("Multiply"), UMaterialExpressionMultiply::StaticClass());
		NodeTypeMap.Add(TEXT("Divide"), UMaterialExpressionDivide::StaticClass());
		NodeTypeMap.Add(TEXT("Lerp"), UMaterialExpressionLinearInterpolate::StaticClass());
		NodeTypeMap.Add(TEXT("Clamp"), UMaterialExpressionClamp::StaticClass());
		NodeTypeMap.Add(TEXT("Power"), UMaterialExpressionPower::StaticClass());
		NodeTypeMap.Add(TEXT("OneMinus"), UMaterialExpressionOneMinus::StaticClass());
		NodeTypeMap.Add(TEXT("Saturate"), UMaterialExpressionSaturate::StaticClass());
		NodeTypeMap.Add(TEXT("Fresnel"), UMaterialExpressionFresnel::StaticClass());
		NodeTypeMap.Add(TEXT("Time"), UMaterialExpressionTime::StaticClass());
		NodeTypeMap.Add(TEXT("Panner"), UMaterialExpressionPanner::StaticClass());
		NodeTypeMap.Add(TEXT("ComponentMask"), UMaterialExpressionComponentMask::StaticClass());
		NodeTypeMap.Add(TEXT("AppendVector"), UMaterialExpressionAppendVector::StaticClass());
		NodeTypeMap.Add(TEXT("Normalize"), UMaterialExpressionNormalize::StaticClass());
		NodeTypeMap.Add(TEXT("DotProduct"), UMaterialExpressionDotProduct::StaticClass());
		NodeTypeMap.Add(TEXT("CrossProduct"), UMaterialExpressionCrossProduct::StaticClass());
	}

	UClass** FoundClass = NodeTypeMap.Find(NodeType);
	if (FoundClass)
	{
		ExpressionClass = *FoundClass;
	}
	else
	{
		// 返回可用的节点类型列表
		TArray<TSharedPtr<FJsonValue>> Suggestions;
		for (const auto& Pair : NodeTypeMap)
		{
			Suggestions.Add(MakeShared<FJsonValueString>(Pair.Key));
		}
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown node_type: %s"), *NodeType));
		ErrorData->SetArrayField(TEXT("suggestions"), Suggestions);
		UAL_CommandUtils::SendResponse(RequestId, 400, ErrorData);
		return;
	}

	// 5. 创建表达式并添加到材质
	FScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "AddNode", "Add Material Node"));

	Material->PreEditChange(nullptr);
	Material->Modify();

	NewExpression = NewObject<UMaterialExpression>(Material, ExpressionClass);
	if (!NewExpression)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create material expression"));
		return;
	}

	// 设置位置
	NewExpression->MaterialExpressionEditorX = PosX;
	NewExpression->MaterialExpressionEditorY = PosY;

	// 添加到材质
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	Material->GetExpressionCollection().AddExpression(NewExpression);
#else
	Material->Expressions.Add(NewExpression);
#endif
	NewExpression->UpdateParameterGuid(true, true);

	// 6. 设置特定节点属性
	if (UMaterialExpressionParameter* ParamExpr = Cast<UMaterialExpressionParameter>(NewExpression))
	{
		if (!NodeName.IsEmpty())
		{
			ParamExpr->ParameterName = FName(*NodeName);
		}
	}

	// 记录贴图是否成功设置（用于返回给调用方）
	bool bTextureApplied = false;

	if (UMaterialExpressionTextureSample* TexSample = Cast<UMaterialExpressionTextureSample>(NewExpression))
	{
		if (!TexturePath.IsEmpty())
		{
			UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
			if (Texture)
			{
				TexSample->Texture = Texture;
				bTextureApplied = true;
				UE_LOG(LogUALMaterial, Log, TEXT("Set texture for TextureSample: %s"), *TexturePath);
			}
			else
			{
				UE_LOG(LogUALMaterial, Warning, TEXT("Failed to load texture: %s"), *TexturePath);
			}
		}
	}

	// 设置初始值
	const TSharedPtr<FJsonObject>* InitialValueObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("initial_value"), InitialValueObj))
	{
		ApplyInitialValueToNode(NewExpression, *InitialValueObj);
	}

	// 标记材质已修改并刷新编辑器
	FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
	Material->PostEditChangeProperty(PropertyChangedEvent);
	
	// 强制刷新材质编辑器图表和逻辑
	if (Material && Material->MaterialGraph)
	{
		FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(Material->MaterialGraph);
	}
	
	Material->MarkPackageDirty();

	// 通知编辑器
	if (GEditor)
	{
		// 刷新属性面板
		FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
		if (PropertyModule)
		{
			PropertyModule->NotifyCustomizationModuleChanged();
		}
	}

	// 8. 生成节点 ID
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	FString NodeId = FString::Printf(TEXT("%s_%d"), *NewExpression->GetClass()->GetName(), Material->GetExpressions().Num() - 1);
#else
	FString NodeId = FString::Printf(TEXT("%s_%d"), *NewExpression->GetClass()->GetName(), Material->Expressions.Num() - 1);
#endif

	// 9. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetStringField(TEXT("class"), NewExpression->GetClass()->GetName());
	Data->SetStringField(TEXT("display_name"), NewExpression->GetName());

	TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
	PosObj->SetNumberField(TEXT("x"), PosX);
	PosObj->SetNumberField(TEXT("y"), PosY);
	Data->SetObjectField(TEXT("position"), PosObj);

	// 简化的 pins 信息
	TArray<TSharedPtr<FJsonValue>> PinsJson;
	TSharedPtr<FJsonObject> OutputPin = MakeShared<FJsonObject>();
	OutputPin->SetStringField(TEXT("name"), TEXT("Default"));
	OutputPin->SetStringField(TEXT("type"), TEXT("Output"));
	PinsJson.Add(MakeShared<FJsonValueObject>(OutputPin));
	Data->SetArrayField(TEXT("pins"), PinsJson);

	// 贴图设置状态
	if (!TexturePath.IsEmpty())
	{
		Data->SetStringField(TEXT("texture_path"), TexturePath);
		Data->SetBoolField(TEXT("texture_applied"), bTextureApplied);
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Added node %s to material %s"), 
		*NodeId, *Material->GetName());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_ConnectMaterialPins - 连接材质节点引脚
// ============================================================================
void FUAL_MaterialCommands::Handle_ConnectMaterialPins(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	FString SourceNode, SourcePin, TargetNode, TargetPin;
	if (!Payload->TryGetStringField(TEXT("source_node"), SourceNode) ||
		!Payload->TryGetStringField(TEXT("source_pin"), SourcePin) ||
		!Payload->TryGetStringField(TEXT("target_node"), TargetNode) ||
		!Payload->TryGetStringField(TEXT("target_pin"), TargetPin))
	{
		UAL_CommandUtils::SendError(RequestId, 400, 
			TEXT("Missing required fields: source_node, source_pin, target_node, target_pin"));
		return;
	}

	// 2. 加载材质
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// 3. 查找源节点（支持两种 ID 格式：计算的索引 ID 和 UE 对象实际名称）
	UMaterialExpression* SourceExpression = nullptr;
	int32 NodeIndex = 0;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (UMaterialExpression* Expression : Material->GetExpressions())
#else
	for (UMaterialExpression* Expression : Material->Expressions)
#endif
	{
		if (!Expression) continue;
		
		// 方式1: 匹配计算生成的 ID (ClassName_Index)
		FString NodeId = FString::Printf(TEXT("%s_%d"), *Expression->GetClass()->GetName(), NodeIndex++);
		
		// 方式2: 匹配 UE 对象的实际名称 (display_name)
		FString DisplayName = Expression->GetName();
		
		if (NodeId == SourceNode || DisplayName == SourceNode)
		{
			SourceExpression = Expression;
			break;
		}
	}

	if (!SourceExpression)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Source node not found: %s"), *SourceNode));
		return;
	}

	// 4. 连接到材质主节点

	// 4. 连接到材质主节点
	if (TargetNode == TEXT("Material"))
	{
		// 使用 FScopedTransaction 管理事务
		FScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "ConnectPins", "Connect Material Pins"));
		
		Material->PreEditChange(nullptr);
		Material->Modify();

		// 映射 target_pin 到材质属性
		bool bConnected = false;

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		// UE 5.1+ 使用 GetEditorOnlyData()
		if (TargetPin == TEXT("BaseColor"))
		{
			Material->GetEditorOnlyData()->BaseColor.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("Metallic"))
		{
			Material->GetEditorOnlyData()->Metallic.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("Roughness"))
		{
			Material->GetEditorOnlyData()->Roughness.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("Normal"))
		{
			Material->GetEditorOnlyData()->Normal.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("EmissiveColor"))
		{
			Material->GetEditorOnlyData()->EmissiveColor.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("Opacity"))
		{
			Material->GetEditorOnlyData()->Opacity.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("OpacityMask"))
		{
			Material->GetEditorOnlyData()->OpacityMask.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("AmbientOcclusion"))
		{
			Material->GetEditorOnlyData()->AmbientOcclusion.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("WorldPositionOffset"))
		{
			Material->GetEditorOnlyData()->WorldPositionOffset.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("Specular"))
		{
			Material->GetEditorOnlyData()->Specular.Connect(0, SourceExpression);
			bConnected = true;
		}
#else
		// UE 5.0 直接访问材质属性
		if (TargetPin == TEXT("BaseColor"))
		{
			Material->BaseColor.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("Metallic"))
		{
			Material->Metallic.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("Roughness"))
		{
			Material->Roughness.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("Normal"))
		{
			Material->Normal.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("EmissiveColor"))
		{
			Material->EmissiveColor.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("Opacity"))
		{
			Material->Opacity.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("OpacityMask"))
		{
			Material->OpacityMask.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("AmbientOcclusion"))
		{
			Material->AmbientOcclusion.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("WorldPositionOffset"))
		{
			Material->WorldPositionOffset.Connect(0, SourceExpression);
			bConnected = true;
		}
		else if (TargetPin == TEXT("Specular"))
		{
			Material->Specular.Connect(0, SourceExpression);
			bConnected = true;
		}
#endif

		if (!bConnected)
		{
			Transaction.Cancel();
			UAL_CommandUtils::SendError(RequestId, 400, 
				FString::Printf(TEXT("Unknown material pin: %s"), *TargetPin));
			return;
		}

		// 标记材质已修改并刷新编辑器
		FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
		Material->PostEditChangeProperty(PropertyChangedEvent);
		
		// 强制刷新材质编辑器图表和逻辑
		if (Material && Material->MaterialGraph)
		{
			FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(Material->MaterialGraph);
		}
		
		Material->MarkPackageDirty();

		// 通知编辑器
		if (GEditor)
		{
			// 刷新属性面板
			FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
			if (PropertyModule)
			{
				PropertyModule->NotifyCustomizationModuleChanged();
			}
		}

		// 构建响应
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
		ConnObj->SetStringField(TEXT("from"), FString::Printf(TEXT("%s.%s"), *SourceNode, *SourcePin));
		ConnObj->SetStringField(TEXT("to"), FString::Printf(TEXT("Material.%s"), *TargetPin));
		Data->SetObjectField(TEXT("connection"), ConnObj);

		UE_LOG(LogUALMaterial, Log, TEXT("Connected %s.%s -> Material.%s in %s"), 
			*SourceNode, *SourcePin, *TargetPin, *Material->GetName());

		UAL_CommandUtils::SendResponse(RequestId, 200, Data);
	}
	else
	{
		// TODO: 支持节点之间的连接
		UAL_CommandUtils::SendError(RequestId, 501, 
			TEXT("Node-to-node connections not yet implemented. Use target_node='Material' for now."));
	}
}

// ============================================================================
// Handle_CompileMaterial - 编译材质
// ============================================================================
void FUAL_MaterialCommands::Handle_CompileMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	// 2. 加载材质
	UMaterialInterface* MaterialInterface = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!MaterialInterface)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// 3. 触发编译
	UMaterial* Material = Cast<UMaterial>(MaterialInterface);
	if (Material)
	{
		// 强制重新编译
		Material->ForceRecompileForRendering();
	}

	// Post edit change 会触发编译
	MaterialInterface->PostEditChange();

	// 4. 获取编译错误和警告
	TArray<TSharedPtr<FJsonValue>> ErrorsJson;
	TArray<TSharedPtr<FJsonValue>> WarningsJson;
	bool bHasErrors = false;

	if (Material)
	{
		// 获取材质的编译输出日志
		TArray<FString> CompileErrors;

		// 检查材质的表达式是否有问题
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		for (UMaterialExpression* Expression : Material->GetExpressions())
#else
		for (UMaterialExpression* Expression : Material->Expressions)
#endif
		{
			if (!Expression) continue;

			// 检查 TextureSample 节点是否缺少贴图
			if (UMaterialExpressionTextureSample* TexSample = Cast<UMaterialExpressionTextureSample>(Expression))
			{
				if (TexSample->Texture == nullptr)
				{
					FString ErrorMsg = FString::Printf(TEXT("TextureSample node '%s' is missing input texture"), 
						*Expression->GetName());
					CompileErrors.Add(ErrorMsg);
					bHasErrors = true;
					UE_LOG(LogUALMaterial, Warning, TEXT("%s"), *ErrorMsg);
				}
			}
		}

		// 将错误添加到 JSON
		for (const FString& Error : CompileErrors)
		{
			ErrorsJson.Add(MakeShared<FJsonValueString>(Error));
		}
	}

	// 5. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("compiled"), true);
	Data->SetBoolField(TEXT("has_errors"), bHasErrors);
	Data->SetStringField(TEXT("material_path"), MaterialInterface->GetPathName());
	Data->SetStringField(TEXT("material_name"), MaterialInterface->GetName());
	Data->SetArrayField(TEXT("errors"), ErrorsJson);
	Data->SetArrayField(TEXT("warnings"), WarningsJson);

	if (bHasErrors)
	{
		UE_LOG(LogUALMaterial, Warning, TEXT("Material compiled with %d errors: %s"), 
			ErrorsJson.Num(), *MaterialInterface->GetName());
	}
	else
	{
		UE_LOG(LogUALMaterial, Log, TEXT("Compiled material successfully: %s"), *MaterialInterface->GetName());
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_SetMaterialNodeValue - 设置材质节点值
// ============================================================================
void FUAL_MaterialCommands::Handle_SetMaterialNodeValue(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	FString NodeId;
	if (!Payload->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_id"));
		return;
	}

	// 2. 加载材质
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// 3. 查找目标节点（支持两种 ID 格式：计算的索引 ID 和 UE 对象实际名称）
	UMaterialExpression* TargetExpression = nullptr;
	int32 NodeIndex = 0;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (UMaterialExpression* Expression : Material->GetExpressions())
#else
	for (UMaterialExpression* Expression : Material->Expressions)
#endif
	{
		if (!Expression) continue;
		
		// 方式1: 匹配计算生成的 ID (ClassName_Index)
		FString CurrentNodeId = FString::Printf(TEXT("%s_%d"), *Expression->GetClass()->GetName(), NodeIndex++);
		
		// 方式2: 匹配 UE 对象的实际名称 (display_name)
		FString DisplayName = Expression->GetName();
		
		if (CurrentNodeId == NodeId || DisplayName == NodeId)
		{
			TargetExpression = Expression;
			break;
		}
	}

	if (!TargetExpression)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Node not found: %s"), *NodeId));
		return;
	}

	// 4. 设置节点值（根据节点类型）
	FString PropertyName;
	Payload->TryGetStringField(TEXT("property_name"), PropertyName);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetStringField(TEXT("property_name"), PropertyName.IsEmpty() ? TEXT("Value") : PropertyName);

	// 使用 FScopedTransaction 管理事务
	FScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "SetNodeValue", "Set Material Node Value"));
	
	// 在修改值之前调用 PreEdit 和 Modify
	Material->PreEditChange(nullptr);
	Material->Modify();
	TargetExpression->Modify();

	// 尝试设置标量值
	double ScalarValue;
	bool bModified = false;
	if (Payload->TryGetNumberField(TEXT("value"), ScalarValue))
	{
		if (UMaterialExpressionConstant* ConstExpr = Cast<UMaterialExpressionConstant>(TargetExpression))
		{
			Data->SetNumberField(TEXT("old_value"), ConstExpr->R);
			ConstExpr->R = (float)ScalarValue;
			Data->SetNumberField(TEXT("new_value"), ScalarValue);
			bModified = true;
		}
		else if (UMaterialExpressionScalarParameter* ScalarParam = Cast<UMaterialExpressionScalarParameter>(TargetExpression))
		{
			Data->SetNumberField(TEXT("old_value"), ScalarParam->DefaultValue);
			ScalarParam->DefaultValue = (float)ScalarValue;
			Data->SetNumberField(TEXT("new_value"), ScalarValue);
			bModified = true;
		}
	}

	// 尝试设置贴图路径 (TextureSample 节点)
	FString TexturePath;
	if (!bModified && Payload->TryGetStringField(TEXT("value"), TexturePath) && !TexturePath.IsEmpty())
	{
		if (UMaterialExpressionTextureSample* TextureSampleExpr = Cast<UMaterialExpressionTextureSample>(TargetExpression))
		{
			// 记录旧值
			FString OldTexturePath = TextureSampleExpr->Texture ? TextureSampleExpr->Texture->GetPathName() : TEXT("");
			Data->SetStringField(TEXT("old_value"), OldTexturePath);

			// 加载新贴图
			UTexture* NewTexture = LoadObject<UTexture>(nullptr, *TexturePath);
			if (NewTexture)
			{
				TextureSampleExpr->Texture = NewTexture;
				Data->SetStringField(TEXT("new_value"), TexturePath);
				bModified = true;
				UE_LOG(LogUALMaterial, Log, TEXT("Set texture for node %s: %s -> %s"), 
					*NodeId, *OldTexturePath, *TexturePath);
			}
			else
			{
				UE_LOG(LogUALMaterial, Warning, TEXT("Failed to load texture: %s"), *TexturePath);
				Data->SetStringField(TEXT("error"), FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
			}
		}
		else if (UMaterialExpressionTextureSampleParameter2D* TextureParamExpr = Cast<UMaterialExpressionTextureSampleParameter2D>(TargetExpression))
		{
			// 记录旧值
			FString OldTexturePath = TextureParamExpr->Texture ? TextureParamExpr->Texture->GetPathName() : TEXT("");
			Data->SetStringField(TEXT("old_value"), OldTexturePath);

			// 加载新贴图
			UTexture* NewTexture = LoadObject<UTexture>(nullptr, *TexturePath);
			if (NewTexture)
			{
				TextureParamExpr->Texture = NewTexture;
				Data->SetStringField(TEXT("new_value"), TexturePath);
				bModified = true;
				UE_LOG(LogUALMaterial, Log, TEXT("Set texture parameter for node %s: %s -> %s"), 
					*NodeId, *OldTexturePath, *TexturePath);
			}
			else
			{
				UE_LOG(LogUALMaterial, Warning, TEXT("Failed to load texture: %s"), *TexturePath);
				Data->SetStringField(TEXT("error"), FString::Printf(TEXT("Texture not found: %s"), *TexturePath));
			}
		}
	}

	if (bModified)
	{
		// 广播变更并刷新编辑器
		FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
		
		// 必须刷新具体的节点 (这会让节点在图表中刷新显示)
		TargetExpression->PostEditChangeProperty(PropertyChangedEvent);
		
		// 最后再通知材质 (触发整体编译)
		Material->PostEditChangeProperty(PropertyChangedEvent);
		
		// 强制刷新材质编辑器图表和逻辑
		if (Material && Material->MaterialGraph)
		{
			FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(Material->MaterialGraph);
		}
		
		Material->MarkPackageDirty();

		if (GEditor)
		{
			FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
			if (PropertyModule)
			{
				PropertyModule->NotifyCustomizationModuleChanged();
			}
		}
	}
	else 
	{
		// 取消事务
		Transaction.Cancel();
	}

	UE_LOG(LogUALMaterial, Log, TEXT("Set value for node %s in material %s"), 
		*NodeId, *Material->GetName());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_DeleteMaterialNode - 删除材质节点
// ============================================================================
void FUAL_MaterialCommands::Handle_DeleteMaterialNode(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: material_path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);

	FString NodeId;
	if (!Payload->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: node_id"));
		return;
	}

	// 2. 加载材质
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		return;
	}

	// 3. 查找目标节点（支持两种 ID 格式：计算的索引 ID 和 UE 对象实际名称）
	UMaterialExpression* TargetExpression = nullptr;
	int32 NodeIndex = 0;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	for (UMaterialExpression* Expression : Material->GetExpressions())
#else
	for (UMaterialExpression* Expression : Material->Expressions)
#endif
	{
		if (!Expression) continue;
		
		// 方式1: 匹配计算生成的 ID (ClassName_Index)
		FString CurrentNodeId = FString::Printf(TEXT("%s_%d"), *Expression->GetClass()->GetName(), NodeIndex++);
		
		// 方式2: 匹配 UE 对象的实际名称 (display_name)
		FString DisplayName = Expression->GetName();
		
		if (CurrentNodeId == NodeId || DisplayName == NodeId)
		{
			TargetExpression = Expression;
			break;
		}
	}

	if (!TargetExpression)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Node not found: %s"), *NodeId));
		return;
	}

	// 4. 从材质中移除节点
	FScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "DeleteNode", "Delete Material Node"));
	
	Material->PreEditChange(nullptr);
	Material->Modify();
	
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	Material->GetExpressionCollection().RemoveExpression(TargetExpression);
#else
	Material->Expressions.Remove(TargetExpression);
#endif

	// 5. 标记材质已修改并刷新编辑器
	FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
	Material->PostEditChangeProperty(PropertyChangedEvent);
	
	// 强制刷新材质编辑器图表和逻辑
	if (Material && Material->MaterialGraph)
	{
		FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(Material->MaterialGraph);
	}
	
	Material->MarkPackageDirty();

	// 通知编辑器
	if (GEditor)
	{
		// 刷新属性面板
		FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
		if (PropertyModule)
		{
			PropertyModule->NotifyCustomizationModuleChanged();
		}
	}

	// 6. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NodeId);
	Data->SetNumberField(TEXT("disconnected_count"), 0); // 简化版本

	UE_LOG(LogUALMaterial, Log, TEXT("Deleted node %s from material %s"), 
		*NodeId, *Material->GetName());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// 智能容错辅助函数
// ============================================================================

FString FUAL_MaterialCommands::NormalizePath(const FString& InputPath, const FString& DefaultPrefix)
{
	FString Result = InputPath.TrimStartAndEnd();
	
	// 移除可能的文件扩展名
	if (Result.EndsWith(TEXT(".uasset")))
	{
		Result = Result.LeftChop(7);
	}
	
	// 替换反斜杠为正斜杠
	Result.ReplaceInline(TEXT("\\"), TEXT("/"));
	
	// 移除多余的斜杠
	while (Result.Contains(TEXT("//")))
	{
		Result.ReplaceInline(TEXT("//"), TEXT("/"));
	}
	
	// 如果不以 /Game/ 开头，尝试智能补全
	if (!Result.StartsWith(TEXT("/Game/")))
	{
		// 如果以 / 开头但不是 /Game/，可能是其他路径
		if (!Result.StartsWith(TEXT("/")))
		{
			// 完全没有斜杠前缀，补全为默认路径
			Result = DefaultPrefix / Result;
		}
	}
	
	return Result;
}

TArray<FString> FUAL_MaterialCommands::FindSimilarAssets(const FString& PartialPath, const FString& AssetClass)
{
	TArray<FString> Results;
	
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 提取搜索名称
	FString SearchName = FPaths::GetBaseFilename(PartialPath);
	
	// 搜索资产
	TArray<FAssetData> AssetDataList;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	AssetRegistry.GetAssetsByClass(FTopLevelAssetPath(*AssetClass), AssetDataList);
#else
	AssetRegistry.GetAssetsByClass(FName(*AssetClass), AssetDataList);
#endif
	
	for (const FAssetData& AssetData : AssetDataList)
	{
		FString AssetName = AssetData.AssetName.ToString();
		// 简单的模糊匹配：名称包含搜索词
		if (AssetName.Contains(SearchName, ESearchCase::IgnoreCase))
		{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			Results.Add(AssetData.GetSoftObjectPath().ToString());
#else
			Results.Add(AssetData.ObjectPath.ToString());
#endif
			if (Results.Num() >= 5) break; // 最多返回5个
		}
	}
	
	return Results;
}

bool FUAL_MaterialCommands::ParseBlendMode(const FString& Value, EBlendMode& OutMode)
{
	FString LowerValue = Value.ToLower().TrimStartAndEnd();
	
	// 支持多种格式: 枚举名、中文、数字
	if (LowerValue == TEXT("opaque") || LowerValue == TEXT("0") || LowerValue == TEXT("不透明"))
	{
		OutMode = BLEND_Opaque;
		return true;
	}
	if (LowerValue == TEXT("masked") || LowerValue == TEXT("1") || LowerValue == TEXT("遮罩"))
	{
		OutMode = BLEND_Masked;
		return true;
	}
	if (LowerValue == TEXT("translucent") || LowerValue == TEXT("2") || LowerValue == TEXT("半透明"))
	{
		OutMode = BLEND_Translucent;
		return true;
	}
	if (LowerValue == TEXT("additive") || LowerValue == TEXT("3") || LowerValue == TEXT("叠加"))
	{
		OutMode = BLEND_Additive;
		return true;
	}
	if (LowerValue == TEXT("modulate") || LowerValue == TEXT("4") || LowerValue == TEXT("调制"))
	{
		OutMode = BLEND_Modulate;
		return true;
	}
	
	return false;
}

bool FUAL_MaterialCommands::ParseShadingModel(const FString& Value, EMaterialShadingModel& OutModel)
{
	FString LowerValue = Value.ToLower().TrimStartAndEnd();
	
	if (LowerValue == TEXT("defaultlit") || LowerValue == TEXT("default") || LowerValue == TEXT("默认") || LowerValue == TEXT("默认光照"))
	{
		OutModel = MSM_DefaultLit;
		return true;
	}
	if (LowerValue == TEXT("unlit") || LowerValue == TEXT("无光照") || LowerValue == TEXT("自发光"))
	{
		OutModel = MSM_Unlit;
		return true;
	}
	if (LowerValue == TEXT("subsurface") || LowerValue == TEXT("次表面"))
	{
		OutModel = MSM_Subsurface;
		return true;
	}
	if (LowerValue == TEXT("clearcoat") || LowerValue == TEXT("清漆"))
	{
		OutModel = MSM_ClearCoat;
		return true;
	}
	if (LowerValue == TEXT("twosidedfoliage") || LowerValue == TEXT("双面植物"))
	{
		OutModel = MSM_TwoSidedFoliage;
		return true;
	}
	
	return false;
}

TArray<FString> FUAL_MaterialCommands::GetValidBlendModes()
{
	return { TEXT("Opaque"), TEXT("Masked"), TEXT("Translucent"), TEXT("Additive"), TEXT("Modulate") };
}

TArray<FString> FUAL_MaterialCommands::GetValidShadingModels()
{
	return { TEXT("DefaultLit"), TEXT("Unlit"), TEXT("Subsurface"), TEXT("ClearCoat"), TEXT("TwoSidedFoliage") };
}

void FUAL_MaterialCommands::ApplyInitialValueToNode(UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& InitialValueObj)
{
	if (!Expression || !InitialValueObj.IsValid()) return;

	// 处理常量值
	if (UMaterialExpressionConstant* ConstExpr = Cast<UMaterialExpressionConstant>(Expression))
	{
		double Val;
		if (InitialValueObj->TryGetNumberField(TEXT("value"), Val))
		{
			ConstExpr->R = (float)Val;
		}
	}
	// 处理标量参数
	else if (UMaterialExpressionScalarParameter* ScalarParam = Cast<UMaterialExpressionScalarParameter>(Expression))
	{
		double Val;
		if (InitialValueObj->TryGetNumberField(TEXT("value"), Val))
		{
			ScalarParam->DefaultValue = (float)Val;
		}
	}
	// 处理 3 维向量
	else if (UMaterialExpressionConstant3Vector* Vec3Expr = Cast<UMaterialExpressionConstant3Vector>(Expression))
	{
		const TSharedPtr<FJsonObject>* ColorObj = nullptr;
		if (InitialValueObj->TryGetObjectField(TEXT("value"), ColorObj))
		{
			double R = 0, G = 0, B = 0;
			(*ColorObj)->TryGetNumberField(TEXT("r"), R);
			(*ColorObj)->TryGetNumberField(TEXT("g"), G);
			(*ColorObj)->TryGetNumberField(TEXT("b"), B);
			Vec3Expr->Constant.R = (float)R;
			Vec3Expr->Constant.G = (float)G;
			Vec3Expr->Constant.B = (float)B;
			Vec3Expr->Constant.A = 1.0f;
		}
	}
	// 处理向量参数
	else if (UMaterialExpressionVectorParameter* VecParam = Cast<UMaterialExpressionVectorParameter>(Expression))
	{
		const TSharedPtr<FJsonObject>* ColorObj = nullptr;
		if (InitialValueObj->TryGetObjectField(TEXT("value"), ColorObj))
		{
			double R = 0, G = 0, B = 0, A = 1;
			(*ColorObj)->TryGetNumberField(TEXT("r"), R);
			(*ColorObj)->TryGetNumberField(TEXT("g"), G);
			(*ColorObj)->TryGetNumberField(TEXT("b"), B);
			(*ColorObj)->TryGetNumberField(TEXT("a"), A);
			VecParam->DefaultValue.R = (float)R;
			VecParam->DefaultValue.G = (float)G;
			VecParam->DefaultValue.B = (float)B;
			VecParam->DefaultValue.A = (float)A;
		}
	}
}

// ============================================================================
// Handle_DuplicateMaterial - 复制材质（智能容错）
// ============================================================================
void FUAL_MaterialCommands::Handle_DuplicateMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析源路径（智能补全）
	FString SourcePathRaw;
	if (!Payload->TryGetStringField(TEXT("source_path"), SourcePathRaw) || SourcePathRaw.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: source_path"));
		return;
	}
	
	FString SourcePath = NormalizePath(SourcePathRaw);
	
	// 2. 尝试加载源材质
	UMaterialInterface* SourceMaterial = LoadObject<UMaterialInterface>(nullptr, *SourcePath);
	if (!SourceMaterial)
	{
		// 查找相似资产提供建议
		TArray<FString> SimilarAssets = FindSimilarAssets(SourcePathRaw, TEXT("MaterialInterface"));
		
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetBoolField(TEXT("success"), false);
		ErrorData->SetStringField(TEXT("error"), FString::Printf(TEXT("Material not found: %s"), *SourcePath));
		
		TArray<TSharedPtr<FJsonValue>> SuggestionArray;
		SuggestionArray.Add(MakeShared<FJsonValueString>(TEXT("检查路径是否正确，应以 /Game/ 开头")));
		SuggestionArray.Add(MakeShared<FJsonValueString>(TEXT("使用 material.describe 工具确认材质存在")));
		ErrorData->SetArrayField(TEXT("suggestions"), SuggestionArray);
		
		if (SimilarAssets.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> SimilarArray;
			for (const FString& Asset : SimilarAssets)
			{
				SimilarArray.Add(MakeShared<FJsonValueString>(Asset));
			}
			ErrorData->SetArrayField(TEXT("similar_assets"), SimilarArray);
		}
		
		UAL_CommandUtils::SendResponse(RequestId, 404, ErrorData);
		return;
	}
	
	// 3. 解析可选参数
	FString NewName;
	if (!Payload->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		NewName = SourceMaterial->GetName() + TEXT("_Copy");
	}
	
	FString DestPath;
	if (!Payload->TryGetStringField(TEXT("destination_path"), DestPath) || DestPath.IsEmpty())
	{
		DestPath = FPaths::GetPath(SourcePath);
	}
	else
	{
		DestPath = NormalizePath(DestPath);
	}
	
	// 4. 构建新资产路径并检查冲突
	FString NewAssetPath = DestPath / NewName;
	int32 CopyIndex = 1;
	while (LoadObject<UObject>(nullptr, *NewAssetPath))
	{
		NewAssetPath = DestPath / FString::Printf(TEXT("%s_%d"), *NewName, CopyIndex++);
		if (CopyIndex > 100)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("Too many name conflicts"));
			return;
		}
	}
	
	// 5. 执行复制 (使用 AssetTools 以更好支持编辑器集成)
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	
	FScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "DuplicateMaterial", "Duplicate Material"));

	UObject* DuplicatedAsset = AssetToolsModule.Get().DuplicateAsset(
		FPaths::GetBaseFilename(NewAssetPath), 
		FPaths::GetPath(NewAssetPath), 
		SourceMaterial);

	if (!DuplicatedAsset)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to duplicate material"));
		return;
	}

	// 标记已创建
	FAssetRegistryModule::AssetCreated(DuplicatedAsset);
	
	// 6. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source_path"), SourcePath);
	Data->SetStringField(TEXT("new_path"), NewAssetPath);
	Data->SetStringField(TEXT("new_name"), FPaths::GetBaseFilename(NewAssetPath));
	
	UE_LOG(LogUALMaterial, Log, TEXT("Duplicated material %s to %s"), *SourcePath, *NewAssetPath);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_SetMaterialProperty - 设置材质属性（智能容错）
// ============================================================================
void FUAL_MaterialCommands::Handle_SetMaterialProperty(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析路径
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);
	
	// 2. 加载材质（必须是 UMaterial，不是 Instance）
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *MaterialPath);
	if (!Material)
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Material not found or is MaterialInstance: %s"), *MaterialPath));
		return;
	}
	
	// 3. 解析属性对象
	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("properties"), PropertiesObj) || !PropertiesObj)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: properties"));
		return;
	}
	
	// 4. 处理各属性
	TArray<FString> UpdatedProperties;
	TArray<TSharedPtr<FJsonValue>> FailedProperties;

	// 使用 FScopedTransaction 自动管理事务生命周期
	FScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "SetMaterialProperty", "Set Material Properties"));
	
	// 在修改任何属性前调用 PreEditChange 和 Modify
	Material->PreEditChange(nullptr);
	Material->Modify();
	
	// blend_mode
	FString BlendModeStr;
	if ((*PropertiesObj)->TryGetStringField(TEXT("blend_mode"), BlendModeStr))
	{
		EBlendMode NewMode;
		if (ParseBlendMode(BlendModeStr, NewMode))
		{
			Material->BlendMode = NewMode;
			UpdatedProperties.Add(TEXT("blend_mode"));
		}
		else
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("name"), TEXT("blend_mode"));
			FailObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Invalid value: %s"), *BlendModeStr));
			TArray<TSharedPtr<FJsonValue>> ValidValues;
			for (const FString& V : GetValidBlendModes())
			{
				ValidValues.Add(MakeShared<FJsonValueString>(V));
			}
			FailObj->SetArrayField(TEXT("valid_values"), ValidValues);
			FailedProperties.Add(MakeShared<FJsonValueObject>(FailObj));
		}
	}
	
	// shading_model
	FString ShadingModelStr;
	if ((*PropertiesObj)->TryGetStringField(TEXT("shading_model"), ShadingModelStr))
	{
		EMaterialShadingModel NewModel;
		if (ParseShadingModel(ShadingModelStr, NewModel))
		{
			Material->SetShadingModel(NewModel);
			UpdatedProperties.Add(TEXT("shading_model"));
		}
		else
		{
			TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
			FailObj->SetStringField(TEXT("name"), TEXT("shading_model"));
			FailObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Invalid value: %s"), *ShadingModelStr));
			TArray<TSharedPtr<FJsonValue>> ValidValues;
			for (const FString& V : GetValidShadingModels())
			{
				ValidValues.Add(MakeShared<FJsonValueString>(V));
			}
			FailObj->SetArrayField(TEXT("valid_values"), ValidValues);
			FailedProperties.Add(MakeShared<FJsonValueObject>(FailObj));
		}
	}
	
	// two_sided
	bool bTwoSided;
	if ((*PropertiesObj)->TryGetBoolField(TEXT("two_sided"), bTwoSided))
	{
		Material->TwoSided = bTwoSided;
		UpdatedProperties.Add(TEXT("two_sided"));
	}
	
	// 5. 标记材质已修改并刷新编辑器
	if (UpdatedProperties.Num() > 0)
	{
		// 广播属性变更 (触发重编译和 UI 更新)
		FPropertyChangedEvent PropertyChangedEvent(nullptr, EPropertyChangeType::ValueSet);
		Material->PostEditChangeProperty(PropertyChangedEvent);
		
		// 强制刷新材质编辑器图表和逻辑
		if (Material && Material->MaterialGraph)
		{
			FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(Material->MaterialGraph);
		}
		
		Material->MarkPackageDirty();
		
		// 仅通知属性面板刷新
		if (GEditor)
		{
			FPropertyEditorModule* PropertyModule = FModuleManager::GetModulePtr<FPropertyEditorModule>("PropertyEditor");
			if (PropertyModule)
			{
				PropertyModule->NotifyCustomizationModuleChanged();
			}
		}
	}
	else 
	{
		// 如果没有修改，取消事务
		Transaction.Cancel();
	}
	
	// 6. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_path"), MaterialPath);
	
	TArray<TSharedPtr<FJsonValue>> UpdatedArray;
	for (const FString& Prop : UpdatedProperties)
	{
		UpdatedArray.Add(MakeShared<FJsonValueString>(Prop));
	}
	Data->SetArrayField(TEXT("updated_properties"), UpdatedArray);
	Data->SetArrayField(TEXT("failed_properties"), FailedProperties);
	
	// 当前状态
	TSharedPtr<FJsonObject> CurrentState = MakeShared<FJsonObject>();
	CurrentState->SetStringField(TEXT("blend_mode"), StaticEnum<EBlendMode>()->GetNameStringByValue((int64)Material->BlendMode));
	CurrentState->SetBoolField(TEXT("two_sided"), Material->TwoSided);
	Data->SetObjectField(TEXT("current_state"), CurrentState);
	
	UE_LOG(LogUALMaterial, Log, TEXT("Set %d properties on material %s"), UpdatedProperties.Num(), *Material->GetName());
	
	UAL_CommandUtils::SendResponse(RequestId, UpdatedProperties.Num() > 0 ? 200 : 400, Data);
}

// ============================================================================
// Handle_CreateMaterialInstance - 创建材质实例（智能容错）
// ============================================================================
void FUAL_MaterialCommands::Handle_CreateMaterialInstance(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析父材质路径
	FString ParentPathRaw;
	if (!Payload->TryGetStringField(TEXT("parent_path"), ParentPathRaw) || ParentPathRaw.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: parent_path"));
		return;
	}
	FString ParentPath = NormalizePath(ParentPathRaw);
	
	// 2. 加载父材质
	UMaterialInterface* ParentMaterial = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
	if (!ParentMaterial)
	{
		TArray<FString> SimilarAssets = FindSimilarAssets(ParentPathRaw, TEXT("MaterialInterface"));
		
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetBoolField(TEXT("success"), false);
		ErrorData->SetStringField(TEXT("error"), FString::Printf(TEXT("Parent material not found: %s"), *ParentPath));
		
		if (SimilarAssets.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> SimilarArray;
			for (const FString& Asset : SimilarAssets)
			{
				SimilarArray.Add(MakeShared<FJsonValueString>(Asset));
			}
			ErrorData->SetArrayField(TEXT("similar_materials"), SimilarArray);
		}
		
		TArray<TSharedPtr<FJsonValue>> Suggestions;
		Suggestions.Add(MakeShared<FJsonValueString>(TEXT("确保父材质路径正确")));
		Suggestions.Add(MakeShared<FJsonValueString>(TEXT("使用 material.describe 检查材质是否存在")));
		ErrorData->SetArrayField(TEXT("suggestions"), Suggestions);
		
		UAL_CommandUtils::SendResponse(RequestId, 404, ErrorData);
		return;
	}
	
	// 3. 解析可选参数
	FString InstanceName;
	if (!Payload->TryGetStringField(TEXT("instance_name"), InstanceName) || InstanceName.IsEmpty())
	{
		InstanceName = TEXT("MI_") + ParentMaterial->GetName();
	}
	
	FString DestPath;
	if (!Payload->TryGetStringField(TEXT("destination_path"), DestPath) || DestPath.IsEmpty())
	{
		DestPath = FPaths::GetPath(ParentPath);
	}
	else
	{
		DestPath = NormalizePath(DestPath);
	}
	
	// 4. 创建材质实例
	FString PackageName = DestPath / InstanceName;
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create package"));
		return;
	}
	
	// 4. 创建材质实例 (使用 Factory 和事务)
	FScopedTransaction Transaction(NSLOCTEXT("UALMaterial", "CreateMaterialInstance", "Create Material Instance"));

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = ParentMaterial;

	UMaterialInstanceConstant* NewInstance = Cast<UMaterialInstanceConstant>(
		Factory->FactoryCreateNew(
			UMaterialInstanceConstant::StaticClass(),
			Package,
			*InstanceName,
			RF_Public | RF_Standalone,
			nullptr,
			GWarn
		));

	if (!NewInstance)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create material instance"));
		return;
	}
	
	// 5. 标记包已修改
	NewInstance->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(NewInstance);

	
	// 6. 收集可用参数信息
	TArray<TSharedPtr<FJsonValue>> ScalarParams;
	TArray<TSharedPtr<FJsonValue>> VectorParams;
	TArray<TSharedPtr<FJsonValue>> TextureParams;
	
	TArray<FMaterialParameterInfo> ParamInfos;
	TArray<FGuid> ParamIds;
	
	ParentMaterial->GetAllScalarParameterInfo(ParamInfos, ParamIds);
	for (const FMaterialParameterInfo& Info : ParamInfos)
	{
		ScalarParams.Add(MakeShared<FJsonValueString>(Info.Name.ToString()));
	}
	
	ParamInfos.Empty();
	ParamIds.Empty();
	ParentMaterial->GetAllVectorParameterInfo(ParamInfos, ParamIds);
	for (const FMaterialParameterInfo& Info : ParamInfos)
	{
		VectorParams.Add(MakeShared<FJsonValueString>(Info.Name.ToString()));
	}
	
	ParamInfos.Empty();
	ParamIds.Empty();
	ParentMaterial->GetAllTextureParameterInfo(ParamInfos, ParamIds);
	for (const FMaterialParameterInfo& Info : ParamInfos)
	{
		TextureParams.Add(MakeShared<FJsonValueString>(Info.Name.ToString()));
	}
	
	// 7. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("instance_path"), PackageName);
	Data->SetStringField(TEXT("instance_name"), InstanceName);
	Data->SetStringField(TEXT("parent_path"), ParentPath);
	
	TSharedPtr<FJsonObject> AvailableParams = MakeShared<FJsonObject>();
	AvailableParams->SetArrayField(TEXT("scalar_params"), ScalarParams);
	AvailableParams->SetArrayField(TEXT("vector_params"), VectorParams);
	AvailableParams->SetArrayField(TEXT("texture_params"), TextureParams);
	Data->SetObjectField(TEXT("available_params"), AvailableParams);
	
	UE_LOG(LogUALMaterial, Log, TEXT("Created material instance %s from %s"), *InstanceName, *ParentMaterial->GetName());
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_ListMaterials - 列出材质资产
// ============================================================================
void FUAL_MaterialCommands::Handle_ListMaterials(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析参数
	FString SearchPath;
	if (!Payload->TryGetStringField(TEXT("search_path"), SearchPath) || SearchPath.IsEmpty())
	{
		SearchPath = TEXT("/Game");
	}
	
	FString NameFilter;
	Payload->TryGetStringField(TEXT("name_filter"), NameFilter);
	
	FString MaterialType = TEXT("all");
	Payload->TryGetStringField(TEXT("material_type"), MaterialType);
	
	int32 MaxResults = 50;
	Payload->TryGetNumberField(TEXT("max_results"), MaxResults);
	if (MaxResults <= 0) MaxResults = 50;
	if (MaxResults > 200) MaxResults = 200;
	
	// 2. 获取资产注册表
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 3. 搜索材质
	TArray<TSharedPtr<FJsonValue>> MaterialsJson;
	int32 TotalCount = 0;
	
	// 搜索 Material
	if (MaterialType == TEXT("all") || MaterialType == TEXT("material"))
	{
		TArray<FAssetData> MaterialAssets;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetRegistry.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Engine.Material")), MaterialAssets);
#else
		AssetRegistry.GetAssetsByClass(FName(TEXT("Material")), MaterialAssets);
#endif
		
		for (const FAssetData& AssetData : MaterialAssets)
		{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			FString AssetPath = AssetData.GetSoftObjectPath().ToString();
#else
			FString AssetPath = AssetData.ObjectPath.ToString();
#endif
			if (!AssetPath.StartsWith(SearchPath)) continue;
			
			FString AssetName = AssetData.AssetName.ToString();
			if (!NameFilter.IsEmpty())
			{
				// 简单通配符匹配
				FString FilterPattern = NameFilter.Replace(TEXT("*"), TEXT(""));
				if (!AssetName.Contains(FilterPattern, ESearchCase::IgnoreCase)) continue;
			}
			
			if (MaterialsJson.Num() >= MaxResults) break;
			
			TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
			MatObj->SetStringField(TEXT("path"), AssetPath);
			MatObj->SetStringField(TEXT("name"), AssetName);
			MatObj->SetStringField(TEXT("type"), TEXT("Material"));
			MaterialsJson.Add(MakeShared<FJsonValueObject>(MatObj));
			TotalCount++;
		}
	}
	
	// 搜索 MaterialInstance
	if (MaterialType == TEXT("all") || MaterialType == TEXT("instance"))
	{
		TArray<FAssetData> InstanceAssets;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetRegistry.GetAssetsByClass(FTopLevelAssetPath(TEXT("/Script/Engine.MaterialInstanceConstant")), InstanceAssets);
#else
		AssetRegistry.GetAssetsByClass(FName(TEXT("MaterialInstanceConstant")), InstanceAssets);
#endif
		
		for (const FAssetData& AssetData : InstanceAssets)
		{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			FString AssetPath = AssetData.GetSoftObjectPath().ToString();
#else
			FString AssetPath = AssetData.ObjectPath.ToString();
#endif
			if (!AssetPath.StartsWith(SearchPath)) continue;
			
			FString AssetName = AssetData.AssetName.ToString();
			if (!NameFilter.IsEmpty())
			{
				FString FilterPattern = NameFilter.Replace(TEXT("*"), TEXT(""));
				if (!AssetName.Contains(FilterPattern, ESearchCase::IgnoreCase)) continue;
			}
			
			if (MaterialsJson.Num() >= MaxResults) break;
			
			TSharedPtr<FJsonObject> MatObj = MakeShared<FJsonObject>();
			MatObj->SetStringField(TEXT("path"), AssetPath);
			MatObj->SetStringField(TEXT("name"), AssetName);
			MatObj->SetStringField(TEXT("type"), TEXT("MaterialInstance"));
			MaterialsJson.Add(MakeShared<FJsonValueObject>(MatObj));
			TotalCount++;
		}
	}
	
	// 4. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("materials"), MaterialsJson);
	Data->SetNumberField(TEXT("total_count"), TotalCount);
	Data->SetStringField(TEXT("search_path"), SearchPath);
	
	UE_LOG(LogUALMaterial, Log, TEXT("Listed %d materials in %s"), TotalCount, *SearchPath);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// Handle_PreviewMaterial - 预览材质信息
// ============================================================================
void FUAL_MaterialCommands::Handle_PreviewMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析路径
	FString MaterialPath;
	if (!Payload->TryGetStringField(TEXT("path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: path"));
		return;
	}
	MaterialPath = NormalizePath(MaterialPath);
	
	// 2. 加载材质
	UMaterialInterface* MaterialInterface = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!MaterialInterface)
	{
		TArray<FString> SimilarAssets = FindSimilarAssets(MaterialPath, TEXT("MaterialInterface"));
		
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetBoolField(TEXT("success"), false);
		ErrorData->SetStringField(TEXT("error"), FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
		
		if (SimilarAssets.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> SimilarArray;
			for (const FString& Asset : SimilarAssets)
			{
				SimilarArray.Add(MakeShared<FJsonValueString>(Asset));
			}
			ErrorData->SetArrayField(TEXT("similar_materials"), SimilarArray);
		}
		
		UAL_CommandUtils::SendResponse(RequestId, 404, ErrorData);
		return;
	}
	
	// 3. 解析选项
	bool bIncludeGraphSummary = true;
	Payload->TryGetBoolField(TEXT("include_graph_summary"), bIncludeGraphSummary);
	
	// 4. 构建材质信息
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_path"), MaterialPath);
	Data->SetStringField(TEXT("material_name"), MaterialInterface->GetName());
	
	// 判断类型
	UMaterial* Material = Cast<UMaterial>(MaterialInterface);
	UMaterialInstanceConstant* MaterialInstance = Cast<UMaterialInstanceConstant>(MaterialInterface);
	
	if (Material)
	{
		Data->SetStringField(TEXT("material_type"), TEXT("Material"));
		Data->SetStringField(TEXT("blend_mode"), StaticEnum<EBlendMode>()->GetNameStringByValue((int64)Material->BlendMode));
		Data->SetBoolField(TEXT("two_sided"), Material->TwoSided);
		
		if (bIncludeGraphSummary)
		{
			// 统计节点数量
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			int32 NodeCount = Material->GetExpressions().Num();
#else
			int32 NodeCount = Material->Expressions.Num();
#endif
			Data->SetNumberField(TEXT("node_count"), NodeCount);
			
			// 统计贴图数量
			int32 TextureCount = 0;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			for (UMaterialExpression* Expr : Material->GetExpressions())
#else
			for (UMaterialExpression* Expr : Material->Expressions)
#endif
			{
				if (Cast<UMaterialExpressionTextureSample>(Expr) || 
				    Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
				{
					TextureCount++;
				}
			}
			Data->SetNumberField(TEXT("texture_count"), TextureCount);
		}
	}
	else if (MaterialInstance)
	{
		Data->SetStringField(TEXT("material_type"), TEXT("MaterialInstance"));
		
		if (MaterialInstance->Parent)
		{
			Data->SetStringField(TEXT("parent_material"), MaterialInstance->Parent->GetPathName());
		}
		
		// 获取父材质的 BlendMode
		UMaterial* BaseMaterial = MaterialInstance->GetMaterial();
		if (BaseMaterial)
		{
			Data->SetStringField(TEXT("blend_mode"), StaticEnum<EBlendMode>()->GetNameStringByValue((int64)BaseMaterial->BlendMode));
			Data->SetBoolField(TEXT("two_sided"), BaseMaterial->TwoSided);
		}
	}
	
	// 统计参数数量
	TSharedPtr<FJsonObject> ParamCount = MakeShared<FJsonObject>();
	
	TArray<FMaterialParameterInfo> ParamInfos;
	TArray<FGuid> ParamIds;
	
	MaterialInterface->GetAllScalarParameterInfo(ParamInfos, ParamIds);
	ParamCount->SetNumberField(TEXT("scalar"), ParamInfos.Num());
	
	ParamInfos.Empty();
	ParamIds.Empty();
	MaterialInterface->GetAllVectorParameterInfo(ParamInfos, ParamIds);
	ParamCount->SetNumberField(TEXT("vector"), ParamInfos.Num());
	
	ParamInfos.Empty();
	ParamIds.Empty();
	MaterialInterface->GetAllTextureParameterInfo(ParamInfos, ParamIds);
	ParamCount->SetNumberField(TEXT("texture"), ParamInfos.Num());
	
	Data->SetObjectField(TEXT("parameter_count"), ParamCount);
	
	UE_LOG(LogUALMaterial, Log, TEXT("Previewed material: %s"), *MaterialInterface->GetName());
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}
