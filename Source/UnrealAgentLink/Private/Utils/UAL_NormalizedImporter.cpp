// Copyright UnrealAgent. All Rights Reserved.

#include "Utils/UAL_NormalizedImporter.h"
#include "Utils/UAL_PackageReader.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ObjectResource.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/CoreRedirects.h"

#if WITH_EDITOR
#include "Editor.h"
#include "FileHelpers.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogNormalizedImport, Log, All);

// ============================================================================
// 辅助函数
// ============================================================================

FString FUALNormalizedImporter::ToPascalCase(const FString& Input)
{
    // 移除扩展名
    FString Name = FPaths::GetBaseFilename(Input);
    
    // 将分隔符（空格、下划线、连字符等）替换并转为 PascalCase
    TArray<FString> Words;
    Name.ParseIntoArray(Words, TEXT("_-. "), true);
    
    FString Result;
    for (const FString& Word : Words)
    {
        if (Word.Len() > 0)
        {
            // 保持原有大小写，只确保首字母大写
            // 例如: "BP" -> "BP", "easyFog" -> "EasyFog", "TEXTURE" -> "TEXTURE"
            FString FirstChar = Word.Left(1).ToUpper();
            FString Rest = Word.Mid(1);  // 保持原样，不强制小写
            Result += FirstChar + Rest;
        }
    }
    
    return Result;
}

bool FUALNormalizedImporter::IsSkeletalMesh(const FString& FilePath)
{
    // 简单的启发式判断：文件名中包含某些关键词
    FString FileName = FPaths::GetBaseFilename(FilePath).ToLower();
    
    // 常见的骨骼网格命名模式
    if (FileName.Contains(TEXT("skeletal")) ||
        FileName.Contains(TEXT("character")) ||
        FileName.Contains(TEXT("anim")) ||
        FileName.Contains(TEXT("sk_")))
    {
        return true;
    }
    
    // TODO: 更准确的方法是解析 FBX 文件检查是否有骨骼数据
    // 但这需要 FBX SDK 支持，暂时使用启发式方法
    
    return false;
}
/**
 * 从外部 .uasset/.umap 文件读取依赖信息
 * 使用 FUALPackageReader 正确解析包文件的 ImportMap
 * @param FilePath - 外部包文件的绝对路径
 * @param OutDependencies - 输出依赖的包路径（如 /Game/Folder/Asset）
 * @return 成功返回 true
 */
bool ReadPackageDependenciesFromFile(const FString& FilePath, TArray<FString>& OutDependencies)
{
    // 使用专门的包读取器
    FUALPackageReader PackageReader;
    
    if (!PackageReader.OpenPackageFile(FilePath))
    {
        UE_LOG(LogNormalizedImport, Warning, TEXT("无法打开包文件: %s"), *FilePath);
        return false;
    }
    
    TArray<FName> Dependencies;
    if (!PackageReader.ReadDependencies(Dependencies))
    {
        UE_LOG(LogNormalizedImport, Warning, TEXT("读取依赖失败: %s"), *FilePath);
        return false;
    }
    
    // 转换 FName 到 FString 并过滤
    OutDependencies.Reserve(Dependencies.Num());
    for (const FName& DepName : Dependencies)
    {
        FString DepStr = DepName.ToString();
        
        // 只保留 /Game/ 开头的包
        if (DepStr.StartsWith(TEXT("/Game/")))
        {
            OutDependencies.Add(DepStr);
            UE_LOG(LogNormalizedImport, Log, TEXT("发现依赖: %s"), *DepStr);
        }
    }
    
    UE_LOG(LogNormalizedImport, Log, TEXT("从 %s 读取到 %d 个依赖"), 
        *FPaths::GetCleanFilename(FilePath), OutDependencies.Num());
    
    return true;
}


// ============================================================================
// 生成目标路径信息
// ============================================================================

bool FUALNormalizedImporter::GenerateTargetInfo(
    const FString& SourceFilePath,
    const FUALImportRuleSet& RuleSet,
    FUALImportTargetInfo& OutTargetInfo)
{
    // 获取文件扩展名
    FString Extension = FPaths::GetExtension(SourceFilePath).ToLower();
    
    // 获取原始文件名（不含扩展名）
    FString OriginalName = FPaths::GetBaseFilename(SourceFilePath);
    
    // 尝试从文件路径推断资产类型和原包名
    FString AssetClass;
    FName OldPackageName;
    
    if (Extension == TEXT("uasset") || Extension == TEXT("umap"))
    {
        // 对于 uasset/umap，需要从文件内容或路径推断类型
        // 暂时根据扩展名判断
        // 如果是 uasset，尝试从文件头读取真实的 AssetClass
        if (Extension == TEXT("uasset"))
        {
            FUALPackageReader PackageReader;
            if (PackageReader.OpenPackageFile(SourceFilePath))
            {
                FString ClassName;
                if (PackageReader.GetAssetClass(ClassName))
                {
                    AssetClass = ClassName;
                    UE_LOG(LogNormalizedImport, Log, TEXT("识别资产类型: %s -> %s"), *OriginalName, *AssetClass);
                }
            }
        }
        
        if (AssetClass.IsEmpty())
        {
            // 启发式检测：基于文件名前缀推断资产类型
            // 参考 UE5 样式指南的命名规范
            FString UpperName = OriginalName.ToUpper();
            
            if (UpperName.StartsWith(TEXT("BP_")) || UpperName.Contains(TEXT("_BP_")))
            {
                AssetClass = TEXT("Blueprint");
            }
            else if (UpperName.StartsWith(TEXT("ABP_")))
            {
                AssetClass = TEXT("AnimBlueprint");
            }
            else if (UpperName.StartsWith(TEXT("SM_")))
            {
                AssetClass = TEXT("StaticMesh");
            }
            else if (UpperName.StartsWith(TEXT("SK_")))
            {
                AssetClass = TEXT("SkeletalMesh");
            }
            else if (UpperName.StartsWith(TEXT("M_")))
            {
                AssetClass = TEXT("Material");
            }
            else if (UpperName.StartsWith(TEXT("MI_")))
            {
                AssetClass = TEXT("MaterialInstanceConstant");
            }
            else if (UpperName.StartsWith(TEXT("T_")))
            {
                AssetClass = TEXT("Texture2D");
            }
            else if (UpperName.StartsWith(TEXT("A_")))
            {
                AssetClass = TEXT("SoundWave");
            }
            else if (UpperName.StartsWith(TEXT("AM_")))
            {
                AssetClass = TEXT("AnimMontage");
            }
            else if (UpperName.StartsWith(TEXT("NS_")))
            {
                AssetClass = TEXT("NiagaraSystem");
            }
            else if (UpperName.StartsWith(TEXT("PS_")))
            {
                AssetClass = TEXT("ParticleSystem");
            }
            else if (Extension == TEXT("umap"))
            {
                AssetClass = TEXT("World");
            }
            else
            {
                AssetClass = TEXT("Unknown");
            }
            
            if (AssetClass != TEXT("Unknown"))
            {
                UE_LOG(LogNormalizedImport, Log, TEXT("启发式识别资产类型: %s -> %s (基于文件名)"), 
                    *OriginalName, *AssetClass);
            }
        }
        
        // 尝试从路径推断原包名
        // 支持两种格式:
        // 1. .../Content/xxx/Asset.uasset -> /Game/xxx/Asset
        // 2. .../Game/xxx/Asset.uasset -> /Game/xxx/Asset (assetData 目录格式)
        FString RelativePath = SourceFilePath;
        bool bFoundPath = false;
        
        // 先尝试 /Game/ 格式 (assetData 目录)
        int32 GameIdx = RelativePath.Find(TEXT("/Game/"), ESearchCase::IgnoreCase);
        if (GameIdx == INDEX_NONE)
        {
            GameIdx = RelativePath.Find(TEXT("\\Game\\"), ESearchCase::IgnoreCase);
        }
        
        if (GameIdx != INDEX_NONE)
        {
            FString GamePath = RelativePath.Mid(GameIdx);
            GamePath = FPaths::ChangeExtension(GamePath, TEXT(""));
            GamePath.ReplaceInline(TEXT("\\"), TEXT("/"));
            OldPackageName = FName(*GamePath);
            bFoundPath = true;
            
            UE_LOG(LogNormalizedImport, Log, TEXT("从 /Game/ 路径推断: %s -> %s"), 
                *SourceFilePath, *OldPackageName.ToString());
        }
        
        // 再尝试 /Content/ 格式
        if (!bFoundPath)
        {
            int32 ContentIdx = RelativePath.Find(TEXT("/Content/"), ESearchCase::IgnoreCase);
            if (ContentIdx == INDEX_NONE)
            {
                ContentIdx = RelativePath.Find(TEXT("\\Content\\"), ESearchCase::IgnoreCase);
            }
            
            if (ContentIdx != INDEX_NONE)
            {
                FString GamePath = RelativePath.Mid(ContentIdx + 9); // 跳过 "/Content/"
                GamePath = FPaths::ChangeExtension(GamePath, TEXT(""));
                GamePath = TEXT("/Game/") + GamePath;
                GamePath.ReplaceInline(TEXT("\\"), TEXT("/"));
                OldPackageName = FName(*GamePath);
                bFoundPath = true;
                
                UE_LOG(LogNormalizedImport, Log, TEXT("从 /Content/ 路径推断: %s -> %s"), 
                    *SourceFilePath, *OldPackageName.ToString());
            }
        }
        
        if (!bFoundPath)
        {
            // 无法推断原包名，使用文件名
            OldPackageName = FName(*FString::Printf(TEXT("/Game/Unknown/%s"), *OriginalName));
            UE_LOG(LogNormalizedImport, Warning, TEXT("无法推断原包名，使用默认: %s"), *OldPackageName.ToString());
        }
    }
    else
    {
        // 其他类型（FBX, PNG 等）基于扩展名判断
        if (Extension == TEXT("fbx") || Extension == TEXT("obj") || 
            Extension == TEXT("glb") || Extension == TEXT("gltf"))
        {
            AssetClass = IsSkeletalMesh(SourceFilePath) ? TEXT("SkeletalMesh") : TEXT("StaticMesh");
        }
        else if (Extension == TEXT("png") || Extension == TEXT("jpg") || 
                 Extension == TEXT("jpeg") || Extension == TEXT("tga") ||
                 Extension == TEXT("exr") || Extension == TEXT("hdr"))
        {
            AssetClass = TEXT("Texture2D");
        }
        else if (Extension == TEXT("wav") || Extension == TEXT("mp3") || 
                 Extension == TEXT("ogg") || Extension == TEXT("flac"))
        {
            AssetClass = TEXT("SoundWave");
        }
        else if (Extension == TEXT("mp4") || Extension == TEXT("mov") || 
                 Extension == TEXT("avi") || Extension == TEXT("wmv"))
        {
            AssetClass = TEXT("FileMediaSource");
        }
        else
        {
            AssetClass = TEXT("Unknown");
        }
        
        // 外部文件没有原包名
        OldPackageName = NAME_None;
    }
    
    // ========================================
    // 提取源文件夹名称（用于语义后缀）
    // 例如: /Game/VikingPack/Textures/T_Wood -> "VikingPack"
    // ========================================
    FString SourceFolderName;
    if (!OldPackageName.IsNone())
    {
        FString PackagePath = OldPackageName.ToString();
        // 移除 /Game/ 前缀
        if (PackagePath.StartsWith(TEXT("/Game/")))
        {
            FString RelPath = PackagePath.RightChop(6); // 移除 "/Game/"
            // 获取第一个路径段（即源文件夹名称）
            int32 SlashIdx;
            if (RelPath.FindChar(TEXT('/'), SlashIdx))
            {
                SourceFolderName = RelPath.Left(SlashIdx);
            }
            else
            {
                // 没有子目录，使用整个路径（不含文件名）
                SourceFolderName = FPaths::GetBaseFilename(PackagePath);
            }
        }
    }
    
    // 如果无法从包名提取，尝试从源文件路径提取
    if (SourceFolderName.IsEmpty())
    {
        // 尝试从源文件路径提取
        // 例如: H:/Assets/VikingPack/Textures/T_Wood.uasset -> "VikingPack"
        FString Dir = FPaths::GetPath(SourceFilePath);
        Dir.ReplaceInline(TEXT("\\"), TEXT("/"));
        
        // 尝试找到 /Game/ 或 /Content/ 并提取其后的第一个文件夹
        int32 GameIdx = Dir.Find(TEXT("/Game/"), ESearchCase::IgnoreCase);
        if (GameIdx != INDEX_NONE)
        {
            FString RelPath = Dir.Mid(GameIdx + 6); // 跳过 "/Game/"
            int32 SlashIdx;
            if (RelPath.FindChar(TEXT('/'), SlashIdx))
            {
                SourceFolderName = RelPath.Left(SlashIdx);
            }
        }
        
        // 如果仍然为空，使用上上级目录名
        if (SourceFolderName.IsEmpty())
        {
            FString ParentDir = FPaths::GetPath(Dir);
            SourceFolderName = FPaths::GetBaseFilename(ParentDir);
        }
    }
    
    UE_LOG(LogNormalizedImport, Log, TEXT("源文件夹名称: %s (从 %s)"), 
        *SourceFolderName, *OldPackageName.ToString());
    
    // ========================================
    // 确定新包名和目标路径
    // ========================================
    
    FString NewPackagePath;
    FString NormalizedName;
    
    // 模式 1: 保持原路径结构（推荐用于 uasset 导入）
    if (RuleSet.bPreserveOriginalPath && !OldPackageName.IsNone())
    {
        // 直接使用原包名作为新包名（只是复制到 Content 目录）
        NewPackagePath = OldPackageName.ToString();
        NormalizedName = OriginalName;
        
        UE_LOG(LogNormalizedImport, Log, TEXT("保持原路径: %s"), *NewPackagePath);
    }
    // 模式 2: 使用规范化的目录结构
    else
    {
        // 确定目标子目录
        FString SubDirectory = RuleSet.ClassToSubDirectory.FindRef(AssetClass);
        if (SubDirectory.IsEmpty())
        {
            SubDirectory = TEXT("Misc");
            UE_LOG(LogNormalizedImport, Warning, TEXT("未找到类型 '%s' 的目录映射，使用 Misc 目录: %s"), 
                *AssetClass, *OriginalName);
        }
        
        // 确定名称前缀
        FString Prefix = RuleSet.ClassToPrefix.FindRef(AssetClass);
        
        // 检测原始名称是否已经有正确的前缀（避免重复添加）
        // 例如：BP_EasyFog 已经有 BP_ 前缀，不需要再添加
        bool bAlreadyHasPrefix = false;
        if (!Prefix.IsEmpty())
        {
            FString UpperOriginalName = OriginalName.ToUpper();
            FString UpperPrefix = Prefix.ToUpper();
            if (UpperOriginalName.StartsWith(UpperPrefix))
            {
                bAlreadyHasPrefix = true;
                UE_LOG(LogNormalizedImport, Log, TEXT("资产 '%s' 已有前缀 '%s'，跳过添加"), 
                    *OriginalName, *Prefix);
            }
        }
        
        // 生成规范化名称
        if (RuleSet.bUsePascalCase)
        {
            FString PascalName = ToPascalCase(OriginalName);
            // 如果已有前缀，ToPascalCase 会去掉分隔符但保留前缀内容
            // 例如：BP_EasyFog -> BPEasyFog
            // 此时不需要再添加前缀
            NormalizedName = bAlreadyHasPrefix ? PascalName : (Prefix + PascalName);
        }
        else
        {
            NormalizedName = bAlreadyHasPrefix ? OriginalName : (Prefix + OriginalName);
        }
        
        // 构建新包名
        NewPackagePath = FPaths::Combine(RuleSet.TargetRoot, SubDirectory, NormalizedName);
        NewPackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
    }
    
    FName NewPackageName = FName(*NewPackagePath);
    
    // 构建目标物理路径
    FString ContentDir = FPaths::ProjectContentDir();
    FString RelativePackagePath = NewPackagePath;
    RelativePackagePath.RemoveFromStart(TEXT("/Game/"));
    FString TargetFilePath = FPaths::Combine(ContentDir, RelativePackagePath);
    
    // 添加扩展名
    if (Extension == TEXT("umap"))
    {
        TargetFilePath += TEXT(".umap");
    }
    else
    {
        TargetFilePath += TEXT(".uasset");
    }
    
    // 填充输出结构
    OutTargetInfo.SourceFilePath = SourceFilePath;
    OutTargetInfo.OldPackageName = OldPackageName;
    OutTargetInfo.NewPackageName = NewPackageName;
    OutTargetInfo.TargetFilePath = TargetFilePath;
    OutTargetInfo.AssetClass = AssetClass;
    OutTargetInfo.OriginalAssetName = OriginalName;
    OutTargetInfo.NormalizedAssetName = NormalizedName;
    OutTargetInfo.SourceFolderName = SourceFolderName;
    
    return true;
}

// ============================================================================
// 依赖收集
// ============================================================================

void FUALNormalizedImporter::GatherDependencyClosure(
    const TArray<FString>& RootAssetPaths,
    TArray<FString>& OutDependencies,
    bool bIncludeSoftReferences)
{
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
    
    TSet<FName> VisitedPackages;
    TArray<FName> PackageQueue;
    
    // 初始化队列
    for (const FString& AssetPath : RootAssetPaths)
    {
        FName PackageName = FName(*FPackageName::ObjectPathToPackageName(AssetPath));
        if (!VisitedPackages.Contains(PackageName))
        {
            PackageQueue.Add(PackageName);
            VisitedPackages.Add(PackageName);
        }
    }
    
    // BFS 遍历依赖
    while (PackageQueue.Num() > 0)
    {
        FName CurrentPackage = PackageQueue.Pop();
        
        // 获取硬引用依赖
        TArray<FName> HardDependencies;
        AssetRegistry.GetDependencies(CurrentPackage, HardDependencies, UE::AssetRegistry::EDependencyCategory::Package);
        
        for (const FName& Dep : HardDependencies)
        {
            // 跳过引擎资产
            FString DepPath = Dep.ToString();
            if (DepPath.StartsWith(TEXT("/Engine")) || DepPath.StartsWith(TEXT("/Script")))
            {
                continue;
            }
            
            if (!VisitedPackages.Contains(Dep))
            {
                PackageQueue.Add(Dep);
                VisitedPackages.Add(Dep);
            }
        }
        
        // 如果需要，也获取软引用
        if (bIncludeSoftReferences)
        {
            TArray<FName> SoftDependencies;
            // UE5 中软引用的获取方式
            // 注意：这可能需要根据 UE 版本调整
            AssetRegistry.GetDependencies(CurrentPackage, SoftDependencies, 
                UE::AssetRegistry::EDependencyCategory::Package, 
                UE::AssetRegistry::EDependencyQuery::Soft);
            
            for (const FName& Dep : SoftDependencies)
            {
                FString DepPath = Dep.ToString();
                if (DepPath.StartsWith(TEXT("/Engine")) || DepPath.StartsWith(TEXT("/Script")))
                {
                    continue;
                }
                
                if (!VisitedPackages.Contains(Dep))
                {
                    PackageQueue.Add(Dep);
                    VisitedPackages.Add(Dep);
                }
            }
        }
    }
    
    // 转换为路径列表
    for (const FName& PackageName : VisitedPackages)
    {
        OutDependencies.Add(PackageName.ToString());
    }
}

// ============================================================================
// 构造和析构
// ============================================================================

FUALNormalizedImporter::FUALNormalizedImporter()
{
}

FUALNormalizedImporter::~FUALNormalizedImporter()
{
}

// ============================================================================
// 执行规范化导入
// ============================================================================

bool FUALNormalizedImporter::ExecuteNormalizedImport(
    const TArray<FString>& SourceFiles,
    const FUALImportRuleSet& RuleSet,
    FUALNormalizedImportSession& OutSession)
{
    OutSession = FUALNormalizedImportSession();

    UE_LOG(LogNormalizedImport, Log, TEXT("开始规范化导入，共 %d 个初始文件"), SourceFiles.Num());

    // ============================================================================
    // 步骤 0: 从外部文件收集依赖闭包
    // ============================================================================
    
    // 构建「依赖包路径 -> 源文件路径」的映射
    // 这需要知道源文件的基目录，以便查找依赖文件
    
    TSet<FString> AllFilesToProcess;  // 收集所有需要处理的文件（包括依赖）
    TSet<FString> ProcessedPackages;  // 已处理的包路径，避免重复
    TArray<FString> FileQueue;        // BFS 队列
    
    // 首先从源文件中推断基目录
    // 假设源文件路径格式为: .../assetData/{id}/Game/{Path}/Asset.uasset
    // 基目录就是 .../assetData/{id}
    TMap<FString, FString> PackageToFileMap;  // 包路径 -> 文件路径的映射
    
    // 初始化：添加所有源文件到队列
    for (const FString& SourceFile : SourceFiles)
    {
        if (!FPaths::FileExists(SourceFile))
        {
            OutSession.Warnings.Add(FString::Printf(TEXT("源文件不存在: %s"), *SourceFile));
            continue;
        }
        
        AllFilesToProcess.Add(SourceFile);
        FileQueue.Add(SourceFile);
        
        // 从路径推断包名并建立映射
        // 查找 "/Game/" 在路径中的位置
        int32 GameIndex = SourceFile.Find(TEXT("/Game/"), ESearchCase::IgnoreCase);
        if (GameIndex == INDEX_NONE)
        {
            GameIndex = SourceFile.Find(TEXT("\\Game\\"), ESearchCase::IgnoreCase);
        }
        
        if (GameIndex != INDEX_NONE)
        {
            FString BaseDir = SourceFile.Left(GameIndex);
            FString RelativePath = SourceFile.Mid(GameIndex);
            RelativePath = RelativePath.Replace(TEXT("\\"), TEXT("/"));
            
            // 移除扩展名得到包路径
            FString PackagePath = FPaths::GetPath(RelativePath) / FPaths::GetBaseFilename(RelativePath);
            PackagePath = PackagePath.Replace(TEXT("\\"), TEXT("/"));
            
            PackageToFileMap.Add(PackagePath, SourceFile);
            
            UE_LOG(LogNormalizedImport, Log, TEXT("初始文件映射: %s -> %s"), *PackagePath, *SourceFile);
        }
    }
    
    // BFS 遍历收集依赖
    int32 MaxIterations = 100;
    int32 Iterations = 0;
    
    while (FileQueue.Num() > 0 && Iterations < MaxIterations)
    {
        Iterations++;
        FString CurrentFile = FileQueue.Pop();
        
        // 从文件读取依赖
        TArray<FString> Dependencies;
        if (ReadPackageDependenciesFromFile(CurrentFile, Dependencies))
        {
            for (const FString& DepPackage : Dependencies)
            {
                if (ProcessedPackages.Contains(DepPackage))
                {
                    continue;
                }
                ProcessedPackages.Add(DepPackage);
                
                // 尝试找到依赖文件
                // 根据源文件路径推断依赖文件路径
                // 假设依赖文件与源文件在同一个基目录下
                
                // 从 CurrentFile 推断基目录
                int32 GameIndex = CurrentFile.Find(TEXT("/Game/"), ESearchCase::IgnoreCase);
                if (GameIndex == INDEX_NONE)
                {
                    GameIndex = CurrentFile.Find(TEXT("\\Game\\"), ESearchCase::IgnoreCase);
                }
                
                if (GameIndex != INDEX_NONE)
                {
                    FString BaseDir = CurrentFile.Left(GameIndex);
                    
                    // 依赖包路径格式: /Game/Folder/SubFolder/Asset
                    // 转换为文件路径: {BaseDir}/Game/Folder/SubFolder/Asset.uasset
                    FString DepRelative = DepPackage;
                    DepRelative = DepRelative.Replace(TEXT("/"), TEXT("\\"));
                    
                    FString DepFilePath = BaseDir + DepRelative + TEXT(".uasset");
                    
                    // 检查文件是否存在
                    if (FPaths::FileExists(DepFilePath))
                    {
                        if (!AllFilesToProcess.Contains(DepFilePath))
                        {
                            AllFilesToProcess.Add(DepFilePath);
                            FileQueue.Add(DepFilePath);
                            PackageToFileMap.Add(DepPackage, DepFilePath);
                            
                            UE_LOG(LogNormalizedImport, Log, TEXT("发现依赖文件: %s -> %s"), 
                                *DepPackage, *DepFilePath);
                        }
                    }
                    else
                    {
                        UE_LOG(LogNormalizedImport, Warning, TEXT("依赖文件不存在: %s (包: %s)"), 
                            *DepFilePath, *DepPackage);
                    }
                }
            }
        }
    }
    
    UE_LOG(LogNormalizedImport, Log, TEXT("依赖收集完成，共 %d 个文件需要处理"), AllFilesToProcess.Num());
    
    // 更新统计
    OutSession.TotalFiles = AllFilesToProcess.Num();

    // ============================================================================
    // 步骤 1: 生成所有目标信息
    // ============================================================================
    for (const FString& SourceFile : AllFilesToProcess)
    {
        FUALImportTargetInfo TargetInfo;
        if (GenerateTargetInfo(SourceFile, RuleSet, TargetInfo))
        {
            OutSession.TargetInfos.Add(TargetInfo);

            UE_LOG(LogNormalizedImport, Log, TEXT("  %s -> %s (%s)"),
                *TargetInfo.OriginalAssetName,
                *TargetInfo.NewPackageName.ToString(),
                *TargetInfo.AssetClass);
        }
        else
        {
            OutSession.Errors.Add(FString::Printf(TEXT("无法生成目标信息: %s"), *SourceFile));
        }
    }

    // ============================================================================
    // 步骤 1.5: 检测冲突并使用语义后缀重命名
    // ============================================================================
    if (RuleSet.bAutoRenameOnConflict)
    {
        TMap<FName, int32> PackageNameCount;  // 统计每个目标包名的出现次数
        TMap<FName, TArray<int32>> PackageNameIndices;  // 记录使用相同包名的 TargetInfo 索引
        
        // 第一遍：统计重复
        for (int32 i = 0; i < OutSession.TargetInfos.Num(); ++i)
        {
            FName NewPkg = OutSession.TargetInfos[i].NewPackageName;
            int32& Count = PackageNameCount.FindOrAdd(NewPkg, 0);
            Count++;
            PackageNameIndices.FindOrAdd(NewPkg).Add(i);
        }
        
        // 第二遍：对重复的进行重命名
        for (const auto& Pair : PackageNameIndices)
        {
            if (Pair.Value.Num() <= 1)
            {
                continue;  // 没有冲突
            }
            
            UE_LOG(LogNormalizedImport, Log, TEXT("检测到重复包名 %s，共 %d 个资产"), 
                *Pair.Key.ToString(), Pair.Value.Num());
            
            // 对有冲突的资产进行重命名
            for (int32 Idx : Pair.Value)
            {
                FUALImportTargetInfo& Info = OutSession.TargetInfos[Idx];
                
                FString NewName;
                FString Extension = FPaths::GetExtension(Info.TargetFilePath);
                
                // 使用语义后缀（源文件夹名称）
                if (RuleSet.bUseSemanticSuffix && !Info.SourceFolderName.IsEmpty())
                {
                    // 语义后缀格式：AssetName_SourceFolderName
                    // 例如：T_Wood_VikingPack
                    NewName = Info.NormalizedAssetName + TEXT("_") + Info.SourceFolderName;
                    
                    UE_LOG(LogNormalizedImport, Log, TEXT("  语义重命名: %s -> %s (来源: %s)"),
                        *Info.NormalizedAssetName, *NewName, *Info.SourceFolderName);
                }
                else
                {
                    // 回退到数字后缀
                    static int32 NumericCounter = 1;
                    NewName = FString::Printf(TEXT("%s_%02d"), *Info.NormalizedAssetName, NumericCounter++);
                    
                    UE_LOG(LogNormalizedImport, Log, TEXT("  数字重命名: %s -> %s"),
                        *Info.NormalizedAssetName, *NewName);
                }
                
                // 更新目标信息
                FString OldPackagePath = Info.NewPackageName.ToString();
                FString PackageDir = FPaths::GetPath(OldPackagePath);
                FString NewPackagePath = FPaths::Combine(PackageDir, NewName);
                NewPackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
                
                Info.NormalizedAssetName = NewName;
                Info.NewPackageName = FName(*NewPackagePath);
                
                // 更新目标文件路径
                FString TargetDir = FPaths::GetPath(Info.TargetFilePath);
                Info.TargetFilePath = FPaths::Combine(TargetDir, NewName) + TEXT(".") + Extension;
            }
        }
    }
    
    // ============================================================================
    // 步骤 1.6: 建立重定向映射
    // ============================================================================
    for (const FUALImportTargetInfo& TargetInfo : OutSession.TargetInfos)
    {
        // 如果有旧包名，添加到重定向映射
        if (!TargetInfo.OldPackageName.IsNone())
        {
            OutSession.RedirectMap.Add(TargetInfo.OldPackageName, TargetInfo.NewPackageName);
            
            // 构建软路径重定向
            FSoftObjectPath OldPath(TargetInfo.OldPackageName.ToString());
            FSoftObjectPath NewPath(TargetInfo.NewPackageName.ToString());
            OutSession.SoftPathRedirectMap.Add(OldPath, NewPath);
        }
    }

    // 步骤 2: 复制文件
    if (!CopyFilesToTarget(OutSession))
    {
        UE_LOG(LogNormalizedImport, Error, TEXT("文件复制失败"));
        return false;
    }

    // 步骤 3: 设置 AssetRegistry 和 PackageNameResolver
    if (!SetupAssetRegistryAndResolver(OutSession))
    {
        UE_LOG(LogNormalizedImport, Error, TEXT("AssetRegistry 设置失败"));
        return false;
    }

    // 步骤 4: 加载包并修复引用
    if (!LoadAndFixReferences(OutSession))
    {
        UE_LOG(LogNormalizedImport, Warning, TEXT("引用修复过程中有警告"));
    }

    // 步骤 5: 保存并清理
    if (!SaveAndCleanup(OutSession))
    {
        UE_LOG(LogNormalizedImport, Error, TEXT("保存失败"));
        return false;
    }

    UE_LOG(LogNormalizedImport, Log, TEXT("规范化导入完成: 成功 %d, 失败 %d"),
        OutSession.SuccessCount, OutSession.FailedCount);

    return OutSession.FailedCount == 0;
}

// ============================================================================
// 步骤 1: 复制文件到目标位置
// ============================================================================

bool FUALNormalizedImporter::CopyFilesToTarget(FUALNormalizedImportSession& Session)
{
    IFileManager& FileManager = IFileManager::Get();
    FString ContentDir = FPaths::ProjectContentDir();

    for (FUALImportTargetInfo& TargetInfo : Session.TargetInfos)
    {
        // 决定复制目标：
        // - 如果 OldPackageName == NewPackageName（保持原路径），直接复制到 TargetFilePath
        // - 如果路径不同（需要规范化），先复制到原路径位置，然后在 LoadAndFixReferences 中用 RenameAssets 移动
        
        FString ActualTargetPath;
        bool bNeedRename = (TargetInfo.OldPackageName != TargetInfo.NewPackageName) && !TargetInfo.OldPackageName.IsNone();
        
        if (bNeedRename)
        {
            // 先复制到原路径位置，确保内部包名与外部路径匹配
            FString OldRelativePath = TargetInfo.OldPackageName.ToString();
            OldRelativePath.RemoveFromStart(TEXT("/Game/"));
            
            FString Extension = FPaths::GetExtension(TargetInfo.SourceFilePath);
            ActualTargetPath = FPaths::Combine(ContentDir, OldRelativePath);
            ActualTargetPath += TEXT(".") + Extension;
            
            // 记录实际复制位置，供后续加载使用
            TargetInfo.TargetFilePath = ActualTargetPath;  // 更新为实际位置
            
            UE_LOG(LogNormalizedImport, Log, TEXT("规范化导入策略: 先复制到原位置"));
            UE_LOG(LogNormalizedImport, Log, TEXT("  源: %s"), *TargetInfo.SourceFilePath);
            UE_LOG(LogNormalizedImport, Log, TEXT("  临时目标: %s"), *ActualTargetPath);
            UE_LOG(LogNormalizedImport, Log, TEXT("  最终目标: /Game/... (将通过 RenameAssets 移动)"));
        }
        else
        {
            // 保持原路径或无法推断原路径，直接复制到目标位置
            ActualTargetPath = TargetInfo.TargetFilePath;
        }
        
        // 确保目标目录存在
        FString TargetDir = FPaths::GetPath(ActualTargetPath);
        if (!FileManager.DirectoryExists(*TargetDir))
        {
            if (!FileManager.MakeDirectory(*TargetDir, true))
            {
                Session.Errors.Add(FString::Printf(TEXT("无法创建目录: %s"), *TargetDir));
                Session.FailedCount++;
                continue;
            }
        }

        // 检查目标文件是否已存在
        if (FileManager.FileExists(*ActualTargetPath))
        {
            if (bNeedRename)
            {
                // 原位置的文件可能是上次未完成的导入留下的
                // 删除旧文件，用新的源文件覆盖
                UE_LOG(LogNormalizedImport, Log, TEXT("原位置已存在文件，将覆盖: %s"), *ActualTargetPath);
                
                if (!FileManager.Delete(*ActualTargetPath))
                {
                    Session.Warnings.Add(FString::Printf(TEXT("无法删除旧文件: %s，将尝试使用现有资产"), *ActualTargetPath));
                    Session.SuccessCount++;
                    continue;
                }
                // 继续执行复制操作
            }
            else
            {
                Session.Warnings.Add(FString::Printf(TEXT("文件已存在，将跳过: %s"), *ActualTargetPath));
                Session.SuccessCount++; // 视为成功（已存在）
                continue;
            }
        }

        // 复制文件
        uint32 CopyResult = FileManager.Copy(*ActualTargetPath, *TargetInfo.SourceFilePath, true, true);
        if (CopyResult == COPY_OK)
        {
            UE_LOG(LogNormalizedImport, Log, TEXT("复制成功: %s -> %s"), 
                *TargetInfo.OriginalAssetName, *ActualTargetPath);
            Session.SuccessCount++;
        }
        else
        {
            Session.Errors.Add(FString::Printf(TEXT("复制失败 (%d): %s -> %s"),
                CopyResult, *TargetInfo.SourceFilePath, *ActualTargetPath));
            Session.FailedCount++;
        }
    }

    return Session.FailedCount == 0;
}

// ============================================================================
// 步骤 2: 设置 AssetRegistry 和 PackageNameResolver
// ============================================================================

bool FUALNormalizedImporter::SetupAssetRegistryAndResolver(FUALNormalizedImportSession& Session)
{
#if WITH_EDITOR
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    // 收集所有新导入的文件路径
    TArray<FString> ImportedFilePaths;
    for (const FUALImportTargetInfo& TargetInfo : Session.TargetInfos)
    {
        if (IFileManager::Get().FileExists(*TargetInfo.TargetFilePath))
        {
            ImportedFilePaths.Add(TargetInfo.TargetFilePath);
        }
    }

    // 同步扫描 AssetRegistry
    if (ImportedFilePaths.Num() > 0)
    {
        UE_LOG(LogNormalizedImport, Log, TEXT("同步扫描 AssetRegistry，共 %d 个文件"), ImportedFilePaths.Num());
        AssetRegistry.ScanFilesSynchronous(ImportedFilePaths);
    }

    // 注册 PackageNameResolver
    // 用于在加载过程中将旧包名解析为新包名
    if (Session.RedirectMap.Num() > 0)
    {
        UE_LOG(LogNormalizedImport, Log, TEXT("注册 PackageNameResolver，共 %d 个重定向"), Session.RedirectMap.Num());

        // 创建一个引用 Session 的闭包
        TMap<FName, FName>* RedirectMapPtr = &Session.RedirectMap;

        // FResolvePackageNameDelegate 的签名是 bool(const FString&, FString&)
        // 第一个参数是请求的包名，第二个参数是输出的解析后包名
        // 返回 true 表示成功解析，false 表示未解析
        FCoreDelegates::FResolvePackageNameDelegate ResolverDelegate = 
            FCoreDelegates::FResolvePackageNameDelegate::CreateLambda(
                [RedirectMapPtr](const FString& RequestedPackageName, FString& OutResolvedName) -> bool
                {
                    FName RequestedName(*RequestedPackageName);
                    if (const FName* NewName = RedirectMapPtr->Find(RequestedName))
                    {
                        UE_LOG(LogNormalizedImport, Verbose, TEXT("PackageNameResolver: %s -> %s"),
                            *RequestedPackageName, *NewName->ToString());
                        OutResolvedName = NewName->ToString();
                        return true;
                    }
                    return false; // 未解析
                }
            );

        FCoreDelegates::PackageNameResolvers.Add(ResolverDelegate);
        
        // 同时也注册 CoreRedirects，这对于 LinkerLoad 更有效
        TArray<FCoreRedirect> PackageRedirects;
        for (const auto& Pair : Session.RedirectMap)
        {
            PackageRedirects.Emplace(ECoreRedirectFlags::Type_Package, Pair.Key.ToString(), Pair.Value.ToString());
        }
        FCoreRedirects::AddRedirectList(PackageRedirects, TEXT("UAL_NormalizedImporter"));
        
        Session.ResolverIndex = FCoreDelegates::PackageNameResolvers.Num() - 1; // 仅作为参考，实际可能不准确
    }

    return true;
#else
    return false;
#endif
}

// ============================================================================
// 步骤 3: 加载包并修复引用
// ============================================================================

bool FUALNormalizedImporter::LoadAndFixReferences(FUALNormalizedImportSession& Session)
{
#if WITH_EDITOR
    UE_LOG(LogNormalizedImport, Log, TEXT("开始加载包并修复引用"));

    // 获取 AssetTools
    IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    // 收集需要重命名的资产
    TArray<FAssetRenameData> RenameData;

    for (FUALImportTargetInfo& TargetInfo : Session.TargetInfos)
    {
        // 加载资产（此时文件在原路径位置，内部包名与外部路径一致）
        FString OldPackageName = TargetInfo.OldPackageName.ToString();
        FString NewPackageName = TargetInfo.NewPackageName.ToString();
        
        // 使用 OldPackageName 加载（因为文件当前在原位置）
        UPackage* Package = LoadPackage(nullptr, *OldPackageName, LOAD_None);
        if (Package)
        {
            UE_LOG(LogNormalizedImport, Log, TEXT("成功加载包: %s"), *OldPackageName);
            
            // 如果需要移动到新位置
            if (TargetInfo.OldPackageName != TargetInfo.NewPackageName)
            {
                // 获取包中的资产
                TArray<UObject*> ObjectsInPackage;
                GetObjectsWithOuter(Package, ObjectsInPackage, false);
                
                // 找到主资产（与包名同名的资产）
                UObject* MainAsset = nullptr;
                for (UObject* Obj : ObjectsInPackage)
                {
                    if (Obj && Obj->GetName() == FPaths::GetBaseFilename(OldPackageName))
                    {
                        MainAsset = Obj;
                        break;
                    }
                }
                
                // 如果找不到同名资产，取第一个非 Class 资产
                if (!MainAsset)
                {
                    for (UObject* Obj : ObjectsInPackage)
                    {
                        if (Obj && !Obj->IsA<UClass>())
                        {
                            MainAsset = Obj;
                            break;
                        }
                    }
                }
                
                if (MainAsset)
                {
                    // 解析新路径
                    FString NewPath = FPaths::GetPath(NewPackageName);
                    FString NewAssetName = TargetInfo.NormalizedAssetName;
                    
                    UE_LOG(LogNormalizedImport, Log, TEXT("准备移动资产: %s -> %s/%s"), 
                        *MainAsset->GetPathName(), *NewPath, *NewAssetName);
                    
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
                    RenameData.Add(FAssetRenameData(MainAsset->GetPathName(), NewPath, NewAssetName));
#else
                    RenameData.Add(FAssetRenameData(MainAsset, NewPath, NewAssetName));
#endif
                    Session.PackagesToSave.Add(Package);
                }
                else
                {
                    Session.Warnings.Add(FString::Printf(TEXT("未找到主资产: %s"), *OldPackageName));
                }
            }
            else
            {
                // 保持原路径，直接加入保存列表
                Session.PackagesToSave.Add(Package);
            }
        }
        else
        {
            Session.Warnings.Add(FString::Printf(TEXT("无法加载包: %s"), *OldPackageName));
        }
    }

    // 执行批量重命名
    if (RenameData.Num() > 0)
    {
        UE_LOG(LogNormalizedImport, Log, TEXT("执行资产移动，共 %d 个"), RenameData.Num());
        bool bSuccess = AssetTools.RenameAssets(RenameData);
        
        if (!bSuccess)
        {
            Session.Warnings.Add(TEXT("部分资产移动失败，请检查 UE 输出日志"));
        }
    }

    // 修复软引用
    if (Session.SoftPathRedirectMap.Num() > 0 && Session.PackagesToSave.Num() > 0)
    {
        UE_LOG(LogNormalizedImport, Log, TEXT("修复软引用，共 %d 个映射"), Session.SoftPathRedirectMap.Num());
        AssetTools.RenameReferencingSoftObjectPaths(Session.PackagesToSave, Session.SoftPathRedirectMap);
    }

    return true;
#else
    return false;
#endif
}

// ============================================================================
// 步骤 4: 保存修改并清理
// ============================================================================

bool FUALNormalizedImporter::SaveAndCleanup(FUALNormalizedImportSession& Session)
{
#if WITH_EDITOR
    UE_LOG(LogNormalizedImport, Log, TEXT("保存修改的包，共 %d 个"), Session.PackagesToSave.Num());

    bool bAllSaved = true;

    for (UPackage* Package : Session.PackagesToSave)
    {
        if (Package && Package->IsDirty())
        {
            FString PackageFilename;
            if (FPackageName::DoesPackageExist(Package->GetName(), &PackageFilename))
            {
                FSavePackageArgs SaveArgs;
                SaveArgs.TopLevelFlags = RF_Standalone;
                SaveArgs.SaveFlags = SAVE_NoError;

                FSavePackageResultStruct Result = UPackage::Save(Package, nullptr, *PackageFilename, SaveArgs);
                
                if (Result.Result == ESavePackageResult::Success)
                {
                    UE_LOG(LogNormalizedImport, Log, TEXT("保存成功: %s"), *Package->GetName());
                }
                else
                {
                    Session.Errors.Add(FString::Printf(TEXT("保存失败: %s"), *Package->GetName()));
                    bAllSaved = false;
                }
            }
        }
    }

    // 清理 PackageNameResolver
    CleanupResolver(Session);

    return bAllSaved;
#else
    return false;
#endif
}

// ============================================================================
// 清理 PackageNameResolver
// ============================================================================

void FUALNormalizedImporter::CleanupResolver(FUALNormalizedImportSession& Session)
{
    if (Session.RedirectMap.Num() > 0)
    {
        UE_LOG(LogNormalizedImport, Log, TEXT("清理 CoreRedirects"));
        TArray<FCoreRedirect> PackageRedirects;
        for (const auto& Pair : Session.RedirectMap)
        {
            PackageRedirects.Emplace(ECoreRedirectFlags::Type_Package, Pair.Key.ToString(), Pair.Value.ToString());
        }
        FCoreRedirects::RemoveRedirectList(PackageRedirects, TEXT("UAL_NormalizedImporter"));
    }

    if (Session.ResolverIndex != INDEX_NONE)
    {
        UE_LOG(LogNormalizedImport, Log, TEXT("清理 PackageNameResolver"));
        
        // 注意：直接移除可能会影响其他 resolver 的索引
        // 更安全的做法是将对应的 resolver 设置为空函数
        if (FCoreDelegates::PackageNameResolvers.IsValidIndex(Session.ResolverIndex))
        {
            FCoreDelegates::PackageNameResolvers.RemoveAt(Session.ResolverIndex);
        }
        
        Session.ResolverIndex = INDEX_NONE;
    }
}
