/**
 * UnrealAgentLink - PBR材质自动生成助手
 * 
 * 功能：
 * - 智能识别纹理类型（基于命名约定）
 * - 自动创建PBR材质实例
 * - 自动应用材质到网格体
 * - 标准化资产命名
 * 
 * 超越Quixel的特性：
 * - 支持更多命名约定（Substance、Megascans、自定义）
 * - Agent友好的批量处理
 * - 详细的操作反馈
 * - 可配置的材质参数
 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/StaticMesh.h"

/**
 * 纹理类型枚举
 */
enum class EUAL_PBRTextureType : uint8
{
	Albedo,          // 基础颜色/漫反射
	Normal,          // 法线贴图
	Roughness,       // 粗糙度
	Metallic,        // 金属度
	AO,              // 环境光遮蔽
	Height,          // 高度图/置换
	Emissive,        // 自发光
	Opacity,         // 透明度
	Specular,        // 高光
	Subsurface,      // 次表面散射
	Unknown          // 未知类型
};

/**
 * 纹理分组结果
 * 表示一组属于同一资产的纹理
 */
struct FUAL_TextureGroup
{
	FString BaseName;                                              // 资产基础名称
	TMap<EUAL_PBRTextureType, UTexture2D*> Textures;              // 分类后的纹理
	
	bool IsValid() const { return Textures.Num() > 0; }
};

/**
 * PBR材质创建选项
 */
struct FUAL_PBRMaterialOptions
{
	bool bApplyToMesh = true;              // 是否自动应用到网格体
	bool bUseStandardNaming = true;        // 是否使用标准命名
	bool bAutoConfigureTextures = true;    // 是否自动配置纹理设置
	FString MasterMaterialPath;            // Master Material路径
	
	FUAL_PBRMaterialOptions()
	{
		// 默认使用插件自带的 PBR 母材质
		MasterMaterialPath = TEXT("/UnrealAgentLink/Materials/M_UAMaster");
	}
};

/**
 * PBR材质助手类
 */
class FUAL_PBRMaterialHelper
{
public:
	/**
	 * 智能识别纹理类型
	 * 支持多种命名约定：Quixel、Substance、通用等
	 * 
	 * @param TextureName 纹理名称
	 * @return 识别的纹理类型
	 */
	static EUAL_PBRTextureType ClassifyTexture(const FString& TextureName);
	
	/**
	 * 从纹理名称提取资产基础名称
	 * 例如: "Hero_Albedo" -> "Hero"
	 * 
	 * @param TextureName 纹理名称
	 * @return 资产基础名称
	 */
	static FString ExtractBaseName(const FString& TextureName);
	
	/**
	 * 将纹理数组按资产分组
	 * 智能识别哪些纹理属于同一个资产
	 * 
	 * @param Textures 纹理数组
	 * @return 分组后的纹理集合
	 */
	static TArray<FUAL_TextureGroup> GroupTexturesByAsset(const TArray<UTexture2D*>& Textures);
	
	/**
	 * 创建PBR材质实例
	 * 
	 * @param MaterialName 材质名称
	 * @param DestinationPath UE内部路径
	 * @param TextureGroup 纹理分组
	 * @param Options 创建选项
	 * @return 创建的材质实例，失败返回nullptr
	 */
	static UMaterialInstanceConstant* CreatePBRMaterialInstance(
		const FString& MaterialName,
		const FString& DestinationPath,
		const FUAL_TextureGroup& TextureGroup,
		const FUAL_PBRMaterialOptions& Options);
	
	/**
	 * 为静态网格体应用材质
	 * 
	 * @param StaticMesh 目标网格体
	 * @param Material 材质
	 * @param MaterialIndex 材质槽索引
	 */
	static void ApplyMaterialToMesh(
		UStaticMesh* StaticMesh,
		UMaterialInterface* Material,
		int32 MaterialIndex = 0);
	
	/**
	 * 标准化资产名称
	 * 根据资产类型添加UE标准前缀
	 * 
	 * @param BaseName 基础名称
	 * @param AssetType 资产类型 ("Texture", "Material", "StaticMesh"等)
	 * @return 标准化后的名称
	 */
	static FString StandardizeAssetName(const FString& BaseName, const FString& AssetType);
	
	/**
	 * 配置纹理设置
	 * 根据纹理类型自动配置压缩、sRGB等设置
	 * 
	 * @param Texture 纹理资产
	 * @param Type 纹理类型
	 */
	static void ConfigureTextureSettings(UTexture2D* Texture, EUAL_PBRTextureType Type);
	
	/**
	 * 批量处理：为导入的资产自动创建PBR材质
	 * Agent友好的一站式处理函数
	 * 
	 * @param ImportedTextures 导入的纹理
	 * @param ImportedMeshes 导入的网格体
	 * @param DestinationPath 目标路径
	 * @param Options 处理选项
	 * @param OutCreatedMaterials 输出：创建的材质列表
	 * @return 处理成功的数量
	 */
	static int32 BatchProcessPBRAssets(
		const TArray<UTexture2D*>& ImportedTextures,
		const TArray<UStaticMesh*>& ImportedMeshes,
		const FString& DestinationPath,
		const FUAL_PBRMaterialOptions& Options,
		TArray<UMaterialInstanceConstant*>& OutCreatedMaterials);

private:
	/**
	 * 检查字符串是否包含任意关键词（不区分大小写）
	 */
	static bool ContainsAnyKeyword(const FString& Text, const TArray<FString>& Keywords);
	
	/**
	 * 计算两个字符串的相似度（用于纹理分组）
	 */
	static float CalculateNameSimilarity(const FString& Name1, const FString& Name2);
	
	/**
	 * 移除纹理名称中的类型后缀
	 */
	static FString RemoveTypeSuffix(const FString& TextureName);
};
