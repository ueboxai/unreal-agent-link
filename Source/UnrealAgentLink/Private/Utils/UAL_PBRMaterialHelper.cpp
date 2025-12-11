#include "Utils/UAL_PBRMaterialHelper.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/Material.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"

DEFINE_LOG_CATEGORY_STATIC(LogPBRHelper, Log, All);

// ==============================================================================
// 辅助函数实现
// ==============================================================================

bool FUAL_PBRMaterialHelper::ContainsAnyKeyword(const FString& Text, const TArray<FString>& Keywords)
{
	FString LowerText = Text.ToLower();
	for (const FString& Keyword : Keywords)
	{
		if (LowerText.Contains(Keyword.ToLower()))
		{
			return true;
		}
	}
	return false;
}

FString FUAL_PBRMaterialHelper::RemoveTypeSuffix(const FString& TextureName)
{
	// 待实现
	return TextureName;
}

float FUAL_PBRMaterialHelper::CalculateNameSimilarity(const FString& Name1, const FString& Name2)
{
	// 待实现：使用简单的Levenshtein距离
	return 0.0f;
}

// ==============================================================================
// 核心功能实现（占位符，后续逐步填充）
// ==============================================================================

EUAL_PBRTextureType FUAL_PBRMaterialHelper::ClassifyTexture(const FString& TextureName)
{
	FString LowerName = TextureName.ToLower();
	
	// Albedo/BaseColor/Diffuse 检测
	{
		TArray<FString> AlbedoKeywords = {
			TEXT("albedo"), TEXT("basecolor"), TEXT("base_color"), TEXT("diffuse"),
			TEXT("color"), TEXT("_d."), TEXT("_d_"), TEXT("_a."), TEXT("_a_"),
			TEXT("_bc."), TEXT("_bc_"), TEXT("_diff.")
		};
		if (ContainsAnyKeyword(LowerName, AlbedoKeywords))
		{
			return EUAL_PBRTextureType::Albedo;
		}
	}
	
	// Normal 检测
	{
		TArray<FString> NormalKeywords = {
			TEXT("normal"), TEXT("nrm"), TEXT("nrml"), TEXT("_n."), TEXT("_n_"),
			TEXT("norm"), TEXT("bump")
		};
		if (ContainsAnyKeyword(LowerName, NormalKeywords))
		{
			return EUAL_PBRTextureType::Normal;
		}
	}
	
	// Roughness 检测
	{
		TArray<FString> RoughnessKeywords = {
			TEXT("rough"), TEXT("_r."), TEXT("_r_"), TEXT("rgh")
		};
		if (ContainsAnyKeyword(LowerName, RoughnessKeywords))
		{
			return EUAL_PBRTextureType::Roughness;
		}
	}
	
	// Metallic/Metalness 检测
	{
		TArray<FString> MetallicKeywords = {
			TEXT("metal"), TEXT("_m."), TEXT("_m_"), TEXT("mtl")
		};
		if (ContainsAnyKeyword(LowerName, MetallicKeywords))
		{
			return EUAL_PBRTextureType::Metallic;
		}
	}
	
	// AO (Ambient Occlusion) 检测
	{
		TArray<FString> AOKeywords = {
			TEXT("_ao."), TEXT("_ao_"), TEXT("ambient"), TEXT("occlusion"),
			TEXT("ambientocclusion")
		};
		if (ContainsAnyKeyword(LowerName, AOKeywords))
		{
			return EUAL_PBRTextureType::AO;
		}
	}
	
	// Height/Displacement 检测
	{
		TArray<FString> HeightKeywords = {
			TEXT("height"), TEXT("displace"), TEXT("disp"), TEXT("_h."), TEXT("_h_")
		};
		if (ContainsAnyKeyword(LowerName, HeightKeywords))
		{
			return EUAL_PBRTextureType::Height;
		}
	}
	
	// Emissive 检测
	{
		TArray<FString> EmissiveKeywords = {
			TEXT("emissive"), TEXT("emission"), TEXT("emit"), TEXT("glow")
		};
		if (ContainsAnyKeyword(LowerName, EmissiveKeywords))
		{
			return EUAL_PBRTextureType::Emissive;
		}
	}
	
	// Opacity/Alpha 检测
	{
		TArray<FString> OpacityKeywords = {
			TEXT("opacity"), TEXT("alpha"), TEXT("transparent"), TEXT("trans")
		};
		if (ContainsAnyKeyword(LowerName, OpacityKeywords))
		{
			return EUAL_PBRTextureType::Opacity;
		}
	}
	
	// Specular 检测
	{
		TArray<FString> SpecularKeywords = {
			TEXT("specular"), TEXT("spec"), TEXT("_s."), TEXT("_s_")
		};
		if (ContainsAnyKeyword(LowerName, SpecularKeywords))
		{
			return EUAL_PBRTextureType::Specular;
		}
	}
	
	// Subsurface 检测
	{
		TArray<FString> SubsurfaceKeywords = {
			TEXT("subsurface"), TEXT("sss"), TEXT("scattering")
		};
		if (ContainsAnyKeyword(LowerName, SubsurfaceKeywords))
		{
			return EUAL_PBRTextureType::Subsurface;
		}
	}
	
	return EUAL_PBRTextureType::Unknown;
}

FString FUAL_PBRMaterialHelper::ExtractBaseName(const FString& TextureName)
{
	FString BaseName = TextureName;
	
	// 移除常见的纹理类型后缀
	TArray<FString> Suffixes = {
		TEXT("_Albedo"), TEXT("_BaseColor"), TEXT("_Diffuse"), TEXT("_Color"),
		TEXT("_Normal"), TEXT("_NRM"), TEXT("_N"),
		TEXT("_Roughness"), TEXT("_Rough"), TEXT("_R"),
		TEXT("_Metallic"), TEXT("_Metal"), TEXT("_M"),
		TEXT("_AO"), TEXT("_Occlusion"),
		TEXT("_Height"), TEXT("_Displacement"), TEXT("_H"),
		TEXT("_Emissive"), TEXT("_Emit"),
		TEXT("_Opacity"), TEXT("_Alpha"),
		TEXT("_Specular"), TEXT("_Spec"), TEXT("_S"),
		TEXT("_D"), TEXT("_A"), TEXT("_BC")
	};
	
	// 不区分大小写的后缀移除
	for (const FString& Suffix : Suffixes)
	{
		if (BaseName.EndsWith(Suffix, ESearchCase::IgnoreCase))
		{
			BaseName = BaseName.Left(BaseName.Len() - Suffix.Len());
			break;
		}
	}
	
	// 移除数字后缀（例如 _01, _02）
	if (BaseName.Len() > 3)
	{
		int32 UnderscorePos = BaseName.Len() - 3;
		if (BaseName[UnderscorePos] == '_' &&
			FChar::IsDigit(BaseName[UnderscorePos + 1]) &&
			FChar::IsDigit(BaseName[UnderscorePos + 2]))
		{
			BaseName = BaseName.Left(UnderscorePos);
		}
	}
	
	return BaseName.TrimStartAndEnd();
}

TArray<FUAL_TextureGroup> FUAL_PBRMaterialHelper::GroupTexturesByAsset(const TArray<UTexture2D*>& Textures)
{
	TMap<FString, FUAL_TextureGroup> GroupMap;
	
	for (UTexture2D* Texture : Textures)
	{
		if (!Texture) continue;
		
		FString TextureName = Texture->GetName();
		EUAL_PBRTextureType Type = ClassifyTexture(TextureName);
		
		// 只处理识别的纹理类型
		if (Type == EUAL_PBRTextureType::Unknown)
		{
			UE_LOG(LogPBRHelper, Warning, TEXT("Unknown texture type: %s"), *TextureName);
			continue;
		}
		
		// 提取基础名称
		FString BaseName = ExtractBaseName(TextureName);
		
		// 如果该基础名称的组不存在，创建新组
		if (!GroupMap.Contains(BaseName))
		{
			FUAL_TextureGroup NewGroup;
			NewGroup.BaseName = BaseName;
			GroupMap.Add(BaseName, NewGroup);
		}
		
		// 添加到对应组
		FUAL_TextureGroup& Group = GroupMap[BaseName];
		
		// 检查是否已有相同类型的纹理
		if (Group.Textures.Contains(Type))
		{
			UE_LOG(LogPBRHelper, Warning, 
				TEXT("Duplicate texture type %d for asset '%s', keeping first one"),
				(int32)Type, *BaseName);
		}
		else
		{
			Group.Textures.Add(Type, Texture);
			UE_LOG(LogPBRHelper, Log, 
				TEXT("Grouped texture: %s -> %s (Type: %d)"),
				*TextureName, *BaseName, (int32)Type);
		}
	}
	
	// 转换Map为Array
	TArray<FUAL_TextureGroup> Result;
	for (auto& Pair : GroupMap)
	{
		Result.Add(Pair.Value);
	}
	
	UE_LOG(LogPBRHelper, Log, TEXT("Grouped %d textures into %d assets"), 
		Textures.Num(), Result.Num());
	
	return Result;
}

UMaterialInstanceConstant* FUAL_PBRMaterialHelper::CreatePBRMaterialInstance(
	const FString& MaterialName,
	const FString& DestinationPath,
	const FUAL_TextureGroup& TextureGroup,
	const FUAL_PBRMaterialOptions& Options)
{
	if (!TextureGroup.IsValid())
	{
		UE_LOG(LogPBRHelper, Warning, TEXT("Invalid texture group for material creation"));
		return nullptr;
	}
	
	// 1. 加载Master Material
	FString MasterMatPath = Options.MasterMaterialPath.IsEmpty() ? 
		TEXT("/Engine/EngineMaterials/DefaultMaterial") : Options.MasterMaterialPath;
	
	UMaterial* MasterMaterial = LoadObject<UMaterial>(nullptr, *MasterMatPath);
	if (!MasterMaterial)
	{
		UE_LOG(LogPBRHelper, Error, TEXT("Failed to load Master Material: %s"), *MasterMatPath);
		return nullptr;
	}
	
	// 2. 确定材质实例名称
	FString FinalMatName = Options.bUseStandardNaming ? 
		StandardizeAssetName(MaterialName, TEXT("MaterialInstance")) : MaterialName;
	
	// 3. 创建Package
	FString PackagePath = DestinationPath / FinalMatName;
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		UE_LOG(LogPBRHelper, Error, TEXT("Failed to create package: %s"), *PackagePath);
		return nullptr;
	}
	
	// 4. 创建Material Instance Constant
	UMaterialInstanceConstant* MatInst = NewObject<UMaterialInstanceConstant>(
		Package,
		FName(*FinalMatName),
		RF_Public | RF_Standalone | RF_Transactional);
	
	if (!MatInst)
	{
		UE_LOG(LogPBRHelper, Error, TEXT("Failed to create Material Instance"));
		return nullptr;
	}
	
	// 5. 设置Parent Material
	MatInst->SetParentEditorOnly(MasterMaterial);
	
	// 6. 配置纹理并设置参数
	if (Options.bAutoConfigureTextures)
	{
		for (const auto& Pair : TextureGroup.Textures)
		{
			ConfigureTextureSettings(Pair.Value, Pair.Key);
		}
	}
	
	// 7. 设置纹理参数到材质实例
	// Albedo/BaseColor
	if (TextureGroup.Textures.Contains(EUAL_PBRTextureType::Albedo))
	{
		MatInst->SetTextureParameterValueEditorOnly(
			FMaterialParameterInfo("BaseColor"), 
			TextureGroup.Textures[EUAL_PBRTextureType::Albedo]);
		UE_LOG(LogPBRHelper, Log, TEXT("Set Albedo texture"));
	}
	
	// Normal
	if (TextureGroup.Textures.Contains(EUAL_PBRTextureType::Normal))
	{
		MatInst->SetTextureParameterValueEditorOnly(
			FMaterialParameterInfo("Normal"), 
			TextureGroup.Textures[EUAL_PBRTextureType::Normal]);
		UE_LOG(LogPBRHelper, Log, TEXT("Set Normal texture"));
	}
	
	// Roughness
	if (TextureGroup.Textures.Contains(EUAL_PBRTextureType::Roughness))
	{
		MatInst->SetTextureParameterValueEditorOnly(
			FMaterialParameterInfo("Roughness"), 
			TextureGroup.Textures[EUAL_PBRTextureType::Roughness]);
		UE_LOG(LogPBRHelper, Log, TEXT("Set Roughness texture"));
	}
	
	// Metallic
	if (TextureGroup.Textures.Contains(EUAL_PBRTextureType::Metallic))
	{
		MatInst->SetTextureParameterValueEditorOnly(
			FMaterialParameterInfo("Metallic"), 
			TextureGroup.Textures[EUAL_PBRTextureType::Metallic]);
		UE_LOG(LogPBRHelper, Log, TEXT("Set Metallic texture"));
	}
	
	// AO
	if (TextureGroup.Textures.Contains(EUAL_PBRTextureType::AO))
	{
		MatInst->SetTextureParameterValueEditorOnly(
			FMaterialParameterInfo("AmbientOcclusion"), 
			TextureGroup.Textures[EUAL_PBRTextureType::AO]);
		UE_LOG(LogPBRHelper, Log, TEXT("Set AO texture"));
	}
	
	// Emissive
	if (TextureGroup.Textures.Contains(EUAL_PBRTextureType::Emissive))
	{
		MatInst->SetTextureParameterValueEditorOnly(
			FMaterialParameterInfo("EmissiveColor"), 
			TextureGroup.Textures[EUAL_PBRTextureType::Emissive]);
		UE_LOG(LogPBRHelper, Log, TEXT("Set Emissive texture"));
	}
	
	// 8. 标记为已修改并保存
	Package->MarkPackageDirty();
	MatInst->PostEditChange();
	
	// 9. 注册到AssetRegistry
	FAssetRegistryModule::AssetCreated(MatInst);
	
	UE_LOG(LogPBRHelper, Log, TEXT("Created PBR Material Instance: %s with %d textures"),
		*FinalMatName, TextureGroup.Textures.Num());
	
	return MatInst;
}

void FUAL_PBRMaterialHelper::ApplyMaterialToMesh(
	UStaticMesh* StaticMesh,
	UMaterialInterface* Material,
	int32 MaterialIndex)
{
	if (!StaticMesh || !Material)
	{
		UE_LOG(LogPBRHelper, Warning, TEXT("Invalid mesh or material for ApplyMaterialToMesh"));
		return;
	}
	
	// 确保材质槽索引有效
	if (MaterialIndex < 0 || MaterialIndex >= StaticMesh->GetStaticMaterials().Num())
	{
		UE_LOG(LogPBRHelper, Warning, 
			TEXT("Invalid material index %d for mesh %s (has %d slots)"),
			MaterialIndex, *StaticMesh->GetName(), StaticMesh->GetStaticMaterials().Num());
		return;
	}
	
	// 应用材质
	StaticMesh->SetMaterial(MaterialIndex, Material);
	StaticMesh->PostEditChange();
	
	UE_LOG(LogPBRHelper, Log, 
		TEXT("Applied material '%s' to mesh '%s' at slot %d"),
		*Material->GetName(), *StaticMesh->GetName(), MaterialIndex);
}

FString FUAL_PBRMaterialHelper::StandardizeAssetName(const FString& BaseName, const FString& AssetType)
{
	FString Prefix;
	
	// 根据UE命名约定添加前缀
	if (AssetType == TEXT("Texture"))
	{
		Prefix = TEXT("T_");
	}
	else if (AssetType == TEXT("Material"))
	{
		Prefix = TEXT("M_");
	}
	else if (AssetType == TEXT("MaterialInstance"))
	{
		Prefix = TEXT("MI_");
	}
	else if (AssetType == TEXT("StaticMesh"))
	{
		Prefix = TEXT("SM_");
	}
	else if (AssetType == TEXT("SkeletalMesh"))
	{
		Prefix = TEXT("SK_");
	}
	
	// 如果已有前缀，不重复添加
	if (BaseName.StartsWith(Prefix))
	{
		return BaseName;
	}
	
	return Prefix + BaseName;
}

void FUAL_PBRMaterialHelper::ConfigureTextureSettings(UTexture2D* Texture, EUAL_PBRTextureType Type)
{
	if (!Texture) return;
	
	bool bModified = false;
	
	switch (Type)
	{
		case EUAL_PBRTextureType::Albedo:
		case EUAL_PBRTextureType::Emissive:
			// Albedo和Emissive使用sRGB
			if (!Texture->SRGB)
			{
				Texture->SRGB = true;
				bModified = true;
			}
			Texture->CompressionSettings = TC_Default;
			break;
			
		case EUAL_PBRTextureType::Normal:
			// 法线贴图特殊设置
			Texture->SRGB = false;
			Texture->CompressionSettings = TC_Normalmap;
			bModified = true;
			UE_LOG(LogPBRHelper, Log, TEXT("Configured Normal map: %s"), *Texture->GetName());
			break;
			
		case EUAL_PBRTextureType::Roughness:
		case EUAL_PBRTextureType::Metallic:
		case EUAL_PBRTextureType::AO:
		case EUAL_PBRTextureType::Height:
		case EUAL_PBRTextureType::Opacity:
		case EUAL_PBRTextureType::Specular:
			// 数据贴图不使用sRGB
			if (Texture->SRGB)
			{
				Texture->SRGB = false;
				bModified = true;
			}
			Texture->CompressionSettings = TC_Default;
			break;
			
		default:
			break;
	}
	
	if (bModified)
	{
		Texture->UpdateResource();
		Texture->MarkPackageDirty();
	}
}

int32 FUAL_PBRMaterialHelper::BatchProcessPBRAssets(
	const TArray<UTexture2D*>& ImportedTextures,
	const TArray<UStaticMesh*>& ImportedMeshes,
	const FString& DestinationPath,
	const FUAL_PBRMaterialOptions& Options,
	TArray<UMaterialInstanceConstant*>& OutCreatedMaterials)
{
	UE_LOG(LogPBRHelper, Log, TEXT("Starting batch PBR processing: %d textures, %d meshes"),
		ImportedTextures.Num(), ImportedMeshes.Num());
	
	OutCreatedMaterials.Empty();
	
	// 如果没有纹理，无法创建PBR材质
	if (ImportedTextures.Num() == 0)
	{
		UE_LOG(LogPBRHelper, Warning, TEXT("No textures to process"));
		return 0;
	}
	
	// 1. 将纹理按资产分组
	TArray<FUAL_TextureGroup> TextureGroups = GroupTexturesByAsset(ImportedTextures);
	
	if (TextureGroups.Num() == 0)
	{
		UE_LOG(LogPBRHelper, Warning, TEXT("No valid texture groups found"));
		return 0;
	}
	
	int32 SuccessCount = 0;
	
	// 2. 为每个纹理组创建PBR材质
	for (const FUAL_TextureGroup& Group : TextureGroups)
	{
		// 材质名称基于纹理组的基础名称
		FString MaterialName = Group.BaseName + TEXT("_Mat");
		
		UMaterialInstanceConstant* Material = CreatePBRMaterialInstance(
			MaterialName,
			DestinationPath,
			Group,
			Options);
		
		if (Material)
		{
			OutCreatedMaterials.Add(Material);
			SuccessCount++;
			
			UE_LOG(LogPBRHelper, Log, TEXT("Created material for group: %s"), *Group.BaseName);
			
			// 3. 如果启用自动应用，尝试将材质应用到网格体
			if (Options.bApplyToMesh && ImportedMeshes.Num() > 0)
			{
				// 尝试找到名称匹配的网格体
				UStaticMesh* MatchedMesh = nullptr;
				
				for (UStaticMesh* Mesh : ImportedMeshes)
				{
					if (Mesh && Mesh->GetName().Contains(Group.BaseName))
					{
						MatchedMesh = Mesh;
						break;
					}
				}
				
				// 如果没有找到名称匹配的，并且只有一个网格体和一个材质组，自动匹配
				if (!MatchedMesh && ImportedMeshes.Num() == 1 && TextureGroups.Num() == 1)
				{
					MatchedMesh = ImportedMeshes[0];
					UE_LOG(LogPBRHelper, Log, TEXT("Auto-matching single mesh to single material group"));
				}
				
				// 应用材质
				if (MatchedMesh)
				{
					ApplyMaterialToMesh(MatchedMesh, Material, 0);
					UE_LOG(LogPBRHelper, Log, TEXT("Applied material to mesh: %s"),  *MatchedMesh->GetName());
				}
				else
				{
					UE_LOG(LogPBRHelper, Warning, 
						TEXT("No matching mesh found for material group: %s"), *Group.BaseName);
				}
			}
		}
		else
		{
			UE_LOG(LogPBRHelper, Error, TEXT("Failed to create material for group: %s"), 
				*Group.BaseName);
		}
	}
	
	UE_LOG(LogPBRHelper, Log, 
		TEXT("Batch processing complete: Created %d/%d PBR materials"),
		SuccessCount, TextureGroups.Num());
	
	return SuccessCount;
}
