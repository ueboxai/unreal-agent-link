/**
 * 【进阶参考】PBR材质自动生成示例代码
 * 
 * 这是一个完整的示例，展示如何在导入后自动创建PBR材质实例
 * 此代码为**概念验证(PoC)**，需要根据项目实际情况调整
 * 
 * 作者: Antigravity AI Assistant
 * 日期: 2025-12-11
 */

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/Material.h"
#include "Engine/Texture2D.h"

/**
 * PBR纹理分类器
 * 根据命名规则自动识别纹理类型
 */
class FUAL_PBRTextureClassifier
{
public:
	/**
	 * 纹理类型枚举
	 */
	enum class ETextureType
	{
		Albedo,      // 基础颜色/漫反射
		Normal,      // 法线贴图
		Roughness,   // 粗糙度
		Metallic,    // 金属度
		AO,          // 环境光遮蔽
		Height,      // 高度图/置换
		Emissive,    // 自发光
		Opacity,     // 透明度
		Unknown      // 未知类型
	};

	/**
	 * 根据纹理名称识别类型
	 * 
	 * @param TextureName 纹理资产名称
	 * @return 识别的纹理类型
	 */
	static ETextureType ClassifyTexture(const FString& TextureName)
	{
		FString LowerName = TextureName.ToLower();

		// Albedo/Diffuse/BaseColor
		if (LowerName.Contains(TEXT("albedo")) ||
		    LowerName.Contains(TEXT("diffuse")) ||
		    LowerName.Contains(TEXT("basecolor")) ||
		    LowerName.Contains(TEXT("base_color")) ||
		    LowerName.Contains(TEXT("_d")) ||
		    LowerName.EndsWith(TEXT("_a")))
		{
			return ETextureType::Albedo;
		}

		// Normal
		if (LowerName.Contains(TEXT("normal")) ||
		    LowerName.Contains(TEXT("nrm")) ||
		    LowerName.Contains(TEXT("_n")) ||
		    LowerName.EndsWith(TEXT("_normal")))
		{
			return ETextureType::Normal;
		}

		// Roughness
		if (LowerName.Contains(TEXT("rough")) ||
		    LowerName.Contains(TEXT("_r")) ||
		    LowerName.EndsWith(TEXT("_roughness")))
		{
			return ETextureType::Roughness;
		}

		// Metallic
		if (LowerName.Contains(TEXT("metal")) ||
		    LowerName.Contains(TEXT("_m")) ||
		    LowerName.EndsWith(TEXT("_metallic")))
		{
			return ETextureType::Metallic;
		}

		// AO (Ambient Occlusion)
		if (LowerName.Contains(TEXT("_ao")) ||
		    LowerName.Contains(TEXT("ambient")) ||
		    LowerName.Contains(TEXT("occlusion")))
		{
			return ETextureType::AO;
		}

		// Height/Displacement
		if (LowerName.Contains(TEXT("height")) ||
		    LowerName.Contains(TEXT("displace")) ||
		    LowerName.Contains(TEXT("disp")) ||
		    LowerName.Contains(TEXT("_h")))
		{
			return ETextureType::Height;
		}

		// Emissive
		if (LowerName.Contains(TEXT("emissive")) ||
		    LowerName.Contains(TEXT("emission")) ||
		    LowerName.Contains(TEXT("glow")))
		{
			return ETextureType::Emissive;
		}

		// Opacity
		if (LowerName.Contains(TEXT("opacity")) ||
		    LowerName.Contains(TEXT("alpha")) ||
		    LowerName.Contains(TEXT("transparent")))
		{
			return ETextureType::Opacity;
		}

		return ETextureType::Unknown;
	}

	/**
	 * 批量分类导入的纹理
	 * 
	 * @param Textures 要分类的纹理数组
	 * @return 分类后的纹理映射表
	 */
	static TMap<ETextureType, UTexture2D*> ClassifyTextures(const TArray<UTexture2D*>& Textures)
	{
		TMap<ETextureType, UTexture2D*> ClassifiedTextures;

		for (UTexture2D* Texture : Textures)
		{
			if (Texture)
			{
				ETextureType Type = ClassifyTexture(Texture->GetName());
				if (Type != ETextureType::Unknown)
				{
					ClassifiedTextures.Add(Type, Texture);
				}
			}
		}

		return ClassifiedTextures;
	}
};

/**
 * PBR材质自动生成器
 * 基于分类的纹理自动创建Material Instance
 */
class FUAL_PBRMaterialGenerator
{
public:
	/**
	 * 自动创建PBR材质实例
	 * 
	 * @param MaterialName 材质实例名称
	 * @param DestinationPath UE内部路径 (例如 /Game/Materials)
	 * @param Textures 分类好的纹理映射表
	 * @param MasterMaterialPath 基础PBR材质路径 (例如 /Game/MasterMaterials/M_PBR_Master)
	 * @return 创建的材质实例，失败则返回nullptr
	 */
	static UMaterialInstanceConstant* CreatePBRMaterialInstance(
		const FString& MaterialName,
		const FString& DestinationPath,
		const TMap<FUAL_PBRTextureClassifier::ETextureType, UTexture2D*>& Textures,
		const FString& MasterMaterialPath = TEXT("/Game/MasterMaterials/M_PBR_Master"))
	{
		// 1. 加载Master Material
		UMaterial* MasterMaterial = LoadObject<UMaterial>(nullptr, *MasterMaterialPath);
		if (!MasterMaterial)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to load Master Material: %s"), *MasterMaterialPath);
			return nullptr;
		}

		// 2. 创建Material Instance
		FString PackagePath = DestinationPath / MaterialName;
		UPackage* Package = CreatePackage(*PackagePath);

		UMaterialInstanceConstant* MatInst = NewObject<UMaterialInstanceConstant>(
			Package,
			FName(*MaterialName),
			RF_Public | RF_Standalone);

		if (!MatInst)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to create Material Instance"));
			return nullptr;
		}

		// 3. 设置Parent Material
		MatInst->SetParentEditorOnly(MasterMaterial);

		// 4. 设置纹理参数
		using ETexType = FUAL_PBRTextureClassifier::ETextureType;

		if (Textures.Contains(ETexType::Albedo))
		{
			MatInst->SetTextureParameterValueEditorOnly(FName("AlbedoTexture"), Textures[ETexType::Albedo]);
		}

		if (Textures.Contains(ETexType::Normal))
		{
			UTexture2D* NormalTex = Textures[ETexType::Normal];
			// 确保法线贴图的压缩设置正确
			NormalTex->CompressionSettings = TC_Normalmap;
			MatInst->SetTextureParameterValueEditorOnly(FName("NormalTexture"), NormalTex);
		}

		if (Textures.Contains(ETexType::Roughness))
		{
			MatInst->SetTextureParameterValueEditorOnly(FName("RoughnessTexture"), Textures[ETexType::Roughness]);
		}

		if (Textures.Contains(ETexType::Metallic))
		{
			MatInst->SetTextureParameterValueEditorOnly(FName("MetallicTexture"), Textures[ETexType::Metallic]);
		}

		if (Textures.Contains(ETexType::AO))
		{
			MatInst->SetTextureParameterValueEditorOnly(FName("AOTexture"), Textures[ETexType::AO]);
		}

		if (Textures.Contains(ETexType::Height))
		{
			MatInst->SetTextureParameterValueEditorOnly(FName("HeightTexture"), Textures[ETexType::Height]);
		}

		if (Textures.Contains(ETexType::Emissive))
		{
			MatInst->SetTextureParameterValueEditorOnly(FName("EmissiveTexture"), Textures[ETexType::Emissive]);
		}

		// 5. 保存材质实例
		Package->MarkPackageDirty();
		MatInst->PostEditChange();

		FAssetRegistryModule::AssetCreated(MatInst);

		UE_LOG(LogTemp, Log, TEXT("Created PBR Material Instance: %s"), *MaterialName);

		return MatInst;
	}

	/**
	 * 为静态网格体应用材质
	 * 
	 * @param StaticMesh 目标静态网格体
	 * @param Material 要应用的材质
	 * @param MaterialIndex 材质槽索引，默认0
	 */
	static void ApplyMaterialToMesh(UStaticMesh* StaticMesh, UMaterialInterface* Material, int32 MaterialIndex = 0)
	{
		if (StaticMesh && Material)
		{
			StaticMesh->SetMaterial(MaterialIndex, Material);
			StaticMesh->PostEditChange();
			UE_LOG(LogTemp, Log, TEXT("Applied material to mesh: %s"), *StaticMesh->GetName());
		}
	}
};

/**
 * 使用示例：在 Handle_ImportAssets 中集成
 */
/*
void FUAL_ContentBrowserCommands::Handle_ImportAssets_WithPBR(...)
{
	// ... 现有的导入逻辑 ...

	// 导入完成后，处理PBR材质
	TArray<UTexture2D*> ImportedTextures;
	TArray<UStaticMesh*> ImportedMeshes;

	for (UAssetImportTask* Task : ImportTasks)
	{
		if (Task->IsAsyncImportComplete() && Task->ImportedObjectPaths.Num() > 0)
		{
			for (const FString& ObjectPath : Task->ImportedObjectPaths)
			{
				UObject* ImportedAsset = LoadObject<UObject>(nullptr, *ObjectPath);
				
				// 收集纹理
				if (UTexture2D* Texture = Cast<UTexture2D>(ImportedAsset))
				{
					ImportedTextures.Add(Texture);
				}
				
				// 收集网格体
				if (UStaticMesh* Mesh = Cast<UStaticMesh>(ImportedAsset))
				{
					ImportedMeshes.Add(Mesh);
				}
			}
		}
	}

	// 如果同时导入了纹理和网格体，自动创建PBR材质
	if (ImportedTextures.Num() > 0 && ImportedMeshes.Num() > 0)
	{
		// 1. 分类纹理
		auto ClassifiedTextures = FUAL_PBRTextureClassifier::ClassifyTextures(ImportedTextures);
		
		// 2. 创建材质实例
		FString MaterialName = ImportedMeshes[0]->GetName() + TEXT("_Mat");
		UMaterialInstanceConstant* PBRMaterial = FUAL_PBRMaterialGenerator::CreatePBRMaterialInstance(
			MaterialName,
			DestinationPath,
			ClassifiedTextures);
		
		// 3. 应用到网格体
		if (PBRMaterial)
		{
			for (UStaticMesh* Mesh : ImportedMeshes)
			{
				FUAL_PBRMaterialGenerator::ApplyMaterialToMesh(Mesh, PBRMaterial);
			}
			
			UE_LOG(LogTemp, Log, TEXT("Auto-generated PBR material and applied to %d meshes"), 
				ImportedMeshes.Num());
		}
	}

	// ... 返回结果 ...
}
*/
