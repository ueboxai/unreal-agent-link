// Copyright UnrealAgent. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"

/**
 * 规范化导入的目标信息
 * 记录每个资产从原路径到新路径的映射
 */
struct FUALImportTargetInfo
{
    /** 源文件的物理路径 */
    FString SourceFilePath;

    /** 源资产的原始包名（虚拟路径）*/
    FName OldPackageName;

    /** 目标资产的新包名（虚拟路径）*/
    FName NewPackageName;

    /** 目标文件的物理路径 */
    FString TargetFilePath;

    /** 资产类型（用于确定目标目录）*/
    FString AssetClass;

    /** 原始资产名称 */
    FString OriginalAssetName;

    /** 规范化后的资产名称 */
    FString NormalizedAssetName;

    /** 
     * 源文件夹名称（用于语义后缀）
     * 例如：对于路径 /Game/VikingPack/Textures/T_Wood.uasset，这里存储 "VikingPack"
     */
    FString SourceFolderName;
};

/**
 * 导入会话数据
 * 记录一次规范化导入操作的所有信息
 */
struct FUALNormalizedImportSession
{
    /** 本次导入的所有目标信息 */
    TArray<FUALImportTargetInfo> TargetInfos;

    /** 包名重定向映射：旧包名 -> 新包名 */
    TMap<FName, FName> RedirectMap;

    /** 软路径重定向映射 */
    TMap<FSoftObjectPath, FSoftObjectPath> SoftPathRedirectMap;

    /** 导入完成后需要保存的包列表 */
    TArray<UPackage*> PackagesToSave;

    /** 注册的 PackageNameResolver 的索引（用于清理）*/
    int32 ResolverIndex = INDEX_NONE;

    /** 统计信息 */
    int32 TotalFiles = 0;
    int32 SuccessCount = 0;
    int32 FailedCount = 0;
    TArray<FString> Errors;
    TArray<FString> Warnings;
};

/**
 * 规范化导入规则配置
 * 定义如何将资产从源路径映射到规范化的目标路径
 */
struct FUALImportRuleSet
{
    /** 目标根目录（例如 /Game/Imported）*/
    FString TargetRoot = TEXT("/Game/Imported");

    /** 资产类型到子目录的映射 */
    TMap<FString, FString> ClassToSubDirectory;

    /** 资产名称前缀映射 */
    TMap<FString, FString> ClassToPrefix;

    /** 是否使用 PascalCase 命名 */
    bool bUsePascalCase = true;

    	/** 冲突时是否自动重命名 */
	bool bAutoRenameOnConflict = true;

	/**
	 * 冲突时是否使用语义后缀
	 * true: 使用源文件夹名称作为后缀（如 T_Wood_VikingPack）
	 * false: 使用简单的数字后缀（如 T_Wood_01）
	 */
	bool bUseSemanticSuffix = true;

	/** 
	 * 是否保持原路径结构
	 * true: 保持原始的 /Game/xxx 目录结构，只复制到 Content 目录
	 * false: 使用规范化的目录结构和命名（先复制到原位置，然后用 RenameAssets 移动）
	 */
	bool bPreserveOriginalPath = false;

    /**
     * 初始化默认规则
     * 按照 UE5 样式指南设置默认的目录结构和命名规则
     */
    void InitDefaults()
    {
        TargetRoot = TEXT("/Game/Imported");

        // 资产类型到子目录的映射
        ClassToSubDirectory.Add(TEXT("StaticMesh"), TEXT("Meshes/Static"));
        ClassToSubDirectory.Add(TEXT("SkeletalMesh"), TEXT("Meshes/Skeletal"));
        ClassToSubDirectory.Add(TEXT("Texture"), TEXT("Textures"));  // 基类兜底
        ClassToSubDirectory.Add(TEXT("Texture2D"), TEXT("Textures"));
        ClassToSubDirectory.Add(TEXT("TextureCube"), TEXT("Textures/Cubemaps"));
        ClassToSubDirectory.Add(TEXT("Material"), TEXT("Materials"));
        ClassToSubDirectory.Add(TEXT("MaterialInstance"), TEXT("Materials/Instances"));
        ClassToSubDirectory.Add(TEXT("MaterialInstanceConstant"), TEXT("Materials/Instances"));
        ClassToSubDirectory.Add(TEXT("MaterialFunction"), TEXT("Materials/Functions"));
        ClassToSubDirectory.Add(TEXT("MaterialParameterCollection"), TEXT("Materials/Parameters"));
        ClassToSubDirectory.Add(TEXT("SoundWave"), TEXT("Audio/SFX"));
        ClassToSubDirectory.Add(TEXT("SoundCue"), TEXT("Audio/Cues"));
        ClassToSubDirectory.Add(TEXT("MediaSource"), TEXT("Media/Video"));
        ClassToSubDirectory.Add(TEXT("FileMediaSource"), TEXT("Media/Video"));
        ClassToSubDirectory.Add(TEXT("Blueprint"), TEXT("Blueprints"));
        ClassToSubDirectory.Add(TEXT("World"), TEXT("Maps"));
        ClassToSubDirectory.Add(TEXT("AnimSequence"), TEXT("Animations"));
        ClassToSubDirectory.Add(TEXT("AnimMontage"), TEXT("Animations/Montages"));
        ClassToSubDirectory.Add(TEXT("AnimBlueprint"), TEXT("Animations/Blueprints"));
        ClassToSubDirectory.Add(TEXT("Skeleton"), TEXT("Meshes/Skeletons"));
        ClassToSubDirectory.Add(TEXT("PhysicsAsset"), TEXT("Meshes/Physics"));
        ClassToSubDirectory.Add(TEXT("ParticleSystem"), TEXT("Effects/Particles"));
        ClassToSubDirectory.Add(TEXT("NiagaraSystem"), TEXT("Effects/Niagara"));
        ClassToSubDirectory.Add(TEXT("NiagaraEmitter"), TEXT("Effects/Niagara/Emitters"));

        // 资产类型到名称前缀的映射
        ClassToPrefix.Add(TEXT("StaticMesh"), TEXT("SM_"));
        ClassToPrefix.Add(TEXT("SkeletalMesh"), TEXT("SK_"));
        ClassToPrefix.Add(TEXT("Texture"), TEXT("T_"));  // 基类兜底
        ClassToPrefix.Add(TEXT("Texture2D"), TEXT("T_"));
        ClassToPrefix.Add(TEXT("TextureCube"), TEXT("TC_"));
        ClassToPrefix.Add(TEXT("Material"), TEXT("M_"));
        ClassToPrefix.Add(TEXT("MaterialInstance"), TEXT("MI_"));
        ClassToPrefix.Add(TEXT("MaterialInstanceConstant"), TEXT("MI_"));
        ClassToPrefix.Add(TEXT("MaterialFunction"), TEXT("MF_"));
        ClassToPrefix.Add(TEXT("MaterialParameterCollection"), TEXT("MPC_"));
        ClassToPrefix.Add(TEXT("SoundWave"), TEXT("A_"));
        ClassToPrefix.Add(TEXT("SoundCue"), TEXT("A_"));
        ClassToPrefix.Add(TEXT("MediaSource"), TEXT("MS_"));
        ClassToPrefix.Add(TEXT("FileMediaSource"), TEXT("MS_"));
        ClassToPrefix.Add(TEXT("Blueprint"), TEXT("BP_"));
        ClassToPrefix.Add(TEXT("World"), TEXT("L_"));
        ClassToPrefix.Add(TEXT("AnimSequence"), TEXT("A_"));
        ClassToPrefix.Add(TEXT("AnimMontage"), TEXT("AM_"));
        ClassToPrefix.Add(TEXT("AnimBlueprint"), TEXT("ABP_"));
        ClassToPrefix.Add(TEXT("Skeleton"), TEXT("SKEL_"));
        ClassToPrefix.Add(TEXT("PhysicsAsset"), TEXT("PA_"));
        ClassToPrefix.Add(TEXT("ParticleSystem"), TEXT("PS_"));
        ClassToPrefix.Add(TEXT("NiagaraSystem"), TEXT("NS_"));
        ClassToPrefix.Add(TEXT("NiagaraEmitter"), TEXT("NE_"));
    }
};

/**
 * 规范化导入器
 * 实现 uasset/umap 文件的规范化导入功能
 */
class UNREALAGENTLINK_API FUALNormalizedImporter
{
public:
    FUALNormalizedImporter();
    ~FUALNormalizedImporter();

    /**
     * 执行规范化导入
     * @param SourceFiles - 要导入的源文件路径列表
     * @param RuleSet - 导入规则配置
     * @param OutSession - 输出的导入会话信息
     * @return 是否成功
     */
    bool ExecuteNormalizedImport(
        const TArray<FString>& SourceFiles,
        const FUALImportRuleSet& RuleSet,
        FUALNormalizedImportSession& OutSession
    );

    /**
     * 收集资产的依赖闭包
     * @param RootAssetPaths - 根资产路径列表
     * @param OutDependencies - 输出所有依赖的资产路径
     * @param bIncludeSoftReferences - 是否包含软引用
     */
    static void GatherDependencyClosure(
        const TArray<FString>& RootAssetPaths,
        TArray<FString>& OutDependencies,
        bool bIncludeSoftReferences = true
    );

    /**
     * 根据规则生成目标路径信息
     * @param SourceFilePath - 源文件路径
     * @param RuleSet - 导入规则
     * @param OutTargetInfo - 输出的目标信息
     * @return 是否成功
     */
    static bool GenerateTargetInfo(
        const FString& SourceFilePath,
        const FUALImportRuleSet& RuleSet,
        FUALImportTargetInfo& OutTargetInfo
    );

    /**
     * 将字符串转换为 PascalCase
     */
    static FString ToPascalCase(const FString& Input);

    /**
     * 检测是否为骨骼网格（通过检查是否有骨骼数据）
     */
    static bool IsSkeletalMesh(const FString& FilePath);

private:
    /**
     * 步骤1：复制文件到目标位置
     */
    bool CopyFilesToTarget(FUALNormalizedImportSession& Session);

    /**
     * 步骤2：同步 AssetRegistry 并注册 PackageNameResolver
     */
    bool SetupAssetRegistryAndResolver(FUALNormalizedImportSession& Session);

    /**
     * 步骤3：加载包并修复引用
     */
    bool LoadAndFixReferences(FUALNormalizedImportSession& Session);

    /**
     * 步骤4：保存修改并清理
     */
    bool SaveAndCleanup(FUALNormalizedImportSession& Session);

    /**
     * 清理注册的 PackageNameResolver
     */
    void CleanupResolver(FUALNormalizedImportSession& Session);
};
