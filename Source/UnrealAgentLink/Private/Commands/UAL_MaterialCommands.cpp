#include "UAL_MaterialCommands.h"
#include "UAL_CommandUtils.h"
#include "Utils/UAL_PBRMaterialHelper.h"

#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "UObject/SavePackage.h"

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

	UE_LOG(LogUALMaterial, Log, TEXT("Registered 4 material commands"));
}

// ============================================================================
// Handle_CreateMaterial - 从贴图创建 PBR 材质
// ============================================================================
void FUAL_MaterialCommands::Handle_CreateMaterial(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析必填参数: texture_paths
	const TArray<TSharedPtr<FJsonValue>>* TexturePathsJson = nullptr;
	if (!Payload->TryGetArrayField(TEXT("texture_paths"), TexturePathsJson) || !TexturePathsJson || TexturePathsJson->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required field: texture_paths (array of texture asset paths)"));
		return;
	}

	// 2. 加载贴图资产
	TArray<UTexture2D*> Textures;
	TArray<FString> LoadedPaths;
	TArray<FString> FailedPaths;

	for (const TSharedPtr<FJsonValue>& PathValue : *TexturePathsJson)
	{
		FString TexturePath;
		if (!PathValue->TryGetString(TexturePath) || TexturePath.IsEmpty())
		{
			continue;
		}

		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *TexturePath);
		if (Texture)
		{
			Textures.Add(Texture);
			LoadedPaths.Add(TexturePath);
			UE_LOG(LogUALMaterial, Log, TEXT("Loaded texture: %s"), *TexturePath);
		}
		else
		{
			FailedPaths.Add(TexturePath);
			UE_LOG(LogUALMaterial, Warning, TEXT("Failed to load texture: %s"), *TexturePath);
		}
	}

	if (Textures.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("No valid textures could be loaded"));
		return;
	}

	// 3. 解析可选参数
	FString MaterialName;
	if (!Payload->TryGetStringField(TEXT("material_name"), MaterialName) || MaterialName.IsEmpty())
	{
		// 自动从第一个贴图生成名称
		MaterialName = FUAL_PBRMaterialHelper::ExtractBaseName(Textures[0]->GetName());
	}

	FString DestinationPath = TEXT("/Game/Materials");
	Payload->TryGetStringField(TEXT("destination_path"), DestinationPath);

	FString ParentMaterial;
	Payload->TryGetStringField(TEXT("parent_material"), ParentMaterial);

	// 4. 配置选项
	FUAL_PBRMaterialOptions Options;
	Options.bApplyToMesh = false; // 不自动应用到网格体
	Options.bUseStandardNaming = true;
	Options.bAutoConfigureTextures = true;
	if (!ParentMaterial.IsEmpty())
	{
		Options.MasterMaterialPath = ParentMaterial;
	}
	else
	{
		// 使用插件自带的 PBR 母材质
		Options.MasterMaterialPath = TEXT("/UnrealAgentLink/Materials/M_UAMaster");
	}

	// 5. 分组贴图
	TArray<FUAL_TextureGroup> Groups = FUAL_PBRMaterialHelper::GroupTexturesByAsset(Textures);

	if (Groups.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Could not classify any textures. Check texture naming (e.g., T_Wood_Albedo, T_Wood_Normal)"));
		return;
	}

	// 6. 创建材质实例（使用第一个分组）
	const FUAL_TextureGroup& Group = Groups[0];
	UMaterialInstanceConstant* CreatedMaterial = FUAL_PBRMaterialHelper::CreatePBRMaterialInstance(
		MaterialName,
		DestinationPath,
		Group,
		Options);

	if (!CreatedMaterial)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to create material instance"));
		return;
	}

	// 7. 构建响应
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material_name"), CreatedMaterial->GetName());
	Data->SetStringField(TEXT("material_path"), CreatedMaterial->GetPathName());

	// 贴图绑定信息
	TSharedPtr<FJsonObject> Bindings = MakeShared<FJsonObject>();
	for (const auto& Pair : Group.Textures)
	{
		FString SlotName;
		switch (Pair.Key)
		{
		case EUAL_PBRTextureType::Albedo: SlotName = TEXT("BaseColor"); break;
		case EUAL_PBRTextureType::Normal: SlotName = TEXT("Normal"); break;
		case EUAL_PBRTextureType::Roughness: SlotName = TEXT("Roughness"); break;
		case EUAL_PBRTextureType::Metallic: SlotName = TEXT("Metallic"); break;
		case EUAL_PBRTextureType::AO: SlotName = TEXT("AmbientOcclusion"); break;
		case EUAL_PBRTextureType::Emissive: SlotName = TEXT("Emissive"); break;
		case EUAL_PBRTextureType::Height: SlotName = TEXT("Height"); break;
		case EUAL_PBRTextureType::Opacity: SlotName = TEXT("Opacity"); break;
		default: SlotName = TEXT("Unknown"); break;
		}
		if (Pair.Value)
		{
			Bindings->SetStringField(SlotName, Pair.Value->GetPathName());
		}
	}
	Data->SetObjectField(TEXT("texture_bindings"), Bindings);
	Data->SetNumberField(TEXT("texture_count"), Group.Textures.Num());

	UE_LOG(LogUALMaterial, Log, TEXT("Created material: %s with %d textures"), *CreatedMaterial->GetName(), Group.Textures.Num());

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
