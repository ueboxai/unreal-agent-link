#include "UAL_ContentBrowserCommands.h"
#include "UAL_CommandUtils.h"
#include "Utils/UAL_PBRMaterialHelper.h"
#include "Utils/UAL_NormalizedImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "AssetImportTask.h"
#include "Factories/FbxImportUI.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ConfigCacheIni.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Async/Async.h"
#include "FileMediaSource.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h" // UAL: 包含 GIsRunningUnattendedScript 用于无人值守模式删除

// 使用独立的 Log Category 名称，避免与 UAL_ContentBrowserExt 冲突
// 放在这里以便静态函数可以使用
DEFINE_LOG_CATEGORY_STATIC(LogUALContentCmd, Log, All);

// 视频文件扩展名集合（这些需要特殊处理：复制到 Movies 目录并创建 FileMediaSource）
static const TSet<FString> VideoFileExtensions = {
	TEXT("mp4"), TEXT("mov"), TEXT("avi"), TEXT("wmv"), TEXT("mkv"), TEXT("webm"),
	TEXT("m4v"), TEXT("flv"), TEXT("3gp"), TEXT("3g2"), TEXT("mxf"), TEXT("ts")
};

/**
 * 检查文件是否是视频文件
 * @param FilePath 文件路径
 * @return 如果是视频文件返回 true
 */
static bool IsVideoFile(const FString& FilePath)
{
	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	return VideoFileExtensions.Contains(Extension);
}

/**
 * 导入视频文件：复制到 Content/Movies 目录并创建 FileMediaSource 资产
 * 
 * @param SourceFilePath 源视频文件的绝对路径
 * @param DestinationPath UE 资产路径（如 /Game/Imported/Media/Video）
 * @param bOverwrite 是否覆盖已存在的文件
 * @param NormalizedAssetName 可选的规范化资产名称（如果为空则使用原始文件名）
 * @param OutImportedAsset 输出：创建的 FileMediaSource 资产
 * @param OutError 输出：错误信息（如果失败）
 * @return 如果成功返回 true
 */
static bool ImportVideoFile(
	const FString& SourceFilePath,
	const FString& DestinationPath,
	bool bOverwrite,
	const FString& NormalizedAssetName,
	UFileMediaSource*& OutImportedAsset,
	FString& OutError)
{
	OutImportedAsset = nullptr;
	OutError.Empty();
	
	// 1. 验证源文件存在
	if (!FPaths::FileExists(SourceFilePath))
	{
		OutError = FString::Printf(TEXT("Source video file not found: %s"), *SourceFilePath);
		return false;
	}
	
	// 2. 获取项目的 Content/Movies 目录
	// 注意：使用 FPaths::ProjectDir() 而不是 FPaths::ProjectContentDir()
	// 因为 ProjectContentDir() 在某些情况下可能返回相对路径导致解析错误
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString MoviesDir = FPaths::Combine(ProjectDir, TEXT("Content"), TEXT("Movies"));
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Video import - ProjectDir: %s, MoviesDir: %s"), *ProjectDir, *MoviesDir);
	
	// 确保 Movies 目录存在
	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*MoviesDir))
	{
		if (!FileManager.MakeDirectory(*MoviesDir, true))
		{
			OutError = FString::Printf(TEXT("Failed to create Movies directory: %s"), *MoviesDir);
			return false;
		}
		UE_LOG(LogUALContentCmd, Log, TEXT("Created Movies directory: %s"), *MoviesDir);
	}
	
	// 3. 确定资产名称（使用规范化名称或默认生成）
	FString AssetName;
	if (!NormalizedAssetName.IsEmpty())
	{
		// 使用前端提供的规范化名称
		AssetName = NormalizedAssetName;
	}
	else
	{
		// 默认使用 MS_ + 原始文件名
		AssetName = TEXT("MS_") + FPaths::GetBaseFilename(SourceFilePath);
	}
	
	// 4. 构建目标文件路径（使用规范化名称作为文件名）
	const FString Extension = FPaths::GetExtension(SourceFilePath);
	FString TargetFilePath = FPaths::Combine(MoviesDir, AssetName + TEXT(".") + Extension);
	
	// 检查目标文件是否已存在
	if (FPaths::FileExists(TargetFilePath))
	{
		if (bOverwrite)
		{
			// 删除已存在的文件
			if (!FileManager.Delete(*TargetFilePath))
			{
				OutError = FString::Printf(TEXT("Failed to delete existing file: %s"), *TargetFilePath);
				return false;
			}
		}
		else
		{
			// 生成唯一文件名
			int32 Counter = 1;
			FString BaseAssetName = AssetName;
			do
			{
				AssetName = FString::Printf(TEXT("%s_%d"), *BaseAssetName, Counter++);
				TargetFilePath = FPaths::Combine(MoviesDir, AssetName + TEXT(".") + Extension);
			} while (FPaths::FileExists(TargetFilePath) && Counter < 1000);
		}
	}
	
	// 5. 复制视频文件到 Movies 目录
	UE_LOG(LogUALContentCmd, Log, TEXT("Copying video file: %s -> %s"), *SourceFilePath, *TargetFilePath);
	
	const uint32 CopyResult = FileManager.Copy(*TargetFilePath, *SourceFilePath, true);
	if (CopyResult != COPY_OK)
	{
		OutError = FString::Printf(TEXT("Failed to copy video file. Error code: %d"), CopyResult);
		return false;
	}
	
	// 6. 创建 FileMediaSource 资产
	// 构建资产包路径
	FString PackagePath = DestinationPath;
	if (PackagePath.IsEmpty() || !PackagePath.StartsWith(TEXT("/Game")))
	{
		PackagePath = TEXT("/Game/Imported/Media/Video");
	}
	
	const FString FullPackageName = PackagePath / AssetName;
	
	// 检查资产是否已存在
	UPackage* Package = CreatePackage(*FullPackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Failed to create package: %s"), *FullPackageName);
		return false;
	}
	
	// 创建 FileMediaSource 对象
	UFileMediaSource* MediaSource = NewObject<UFileMediaSource>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!MediaSource)
	{
		OutError = TEXT("Failed to create FileMediaSource object");
		return false;
	}
	
	// 6. 设置 FilePath 属性
	// 注意：SetFilePath() 会将相对路径解析为绝对路径，但使用的是当前工作目录（引擎 Binaries）作为基准
	// 所以我们需要直接设置完整的绝对路径 TargetFilePath
	// UE 在打包时会自动处理路径，将 Content/Movies 下的文件包含进包中
	MediaSource->SetFilePath(TargetFilePath);
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Created FileMediaSource: %s with FilePath: %s"), 
		*FullPackageName, *TargetFilePath);
	
	// 7. 标记包为脏并保存
	Package->MarkPackageDirty();
	
	// 注册到 Asset Registry
	FAssetRegistryModule::AssetCreated(MediaSource);
	
	// 保存资产
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		FullPackageName, 
		FPackageName::GetAssetPackageExtension()
	);
	
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
	const FSavePackageResultStruct SaveResult = UPackage::Save(Package, MediaSource, *PackageFileName, SaveArgs);
	if (SaveResult.Result != ESavePackageResult::Success)
#else
	if (!UPackage::SavePackage(Package, MediaSource, *PackageFileName, SaveArgs))
#endif
	{
		UE_LOG(LogUALContentCmd, Warning, TEXT("Failed to save FileMediaSource: %s"), *PackageFileName);
		// 即使保存失败，资产仍然存在于内存中，用户可以稍后手动保存
	}
	
	OutImportedAsset = MediaSource;
	return true;
}

/**
 * 注册所有内容浏览器命令
 */
void FUAL_ContentBrowserCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("content.search"), &Handle_SearchAssets);
	CommandMap.Add(TEXT("content.import"), &Handle_ImportAssets);
	CommandMap.Add(TEXT("content.move"), &Handle_MoveAsset);
	CommandMap.Add(TEXT("content.delete"), &Handle_DeleteAssets);
	CommandMap.Add(TEXT("content.describe"), &Handle_DescribeAsset);
	CommandMap.Add(TEXT("content.normalized_import"), &Handle_NormalizedImport);
	CommandMap.Add(TEXT("content.audit_optimization"), &Handle_AuditOptimization);
	
	UE_LOG(LogUALContentCmd, Log, TEXT("ContentBrowser commands registered: content.search, content.import, content.move, content.delete, content.describe, content.normalized_import, content.audit_optimization"));
}

// ============================================================================
// Handler 实现
// ============================================================================

/**
 * content.search - 搜索资产
 * 支持模糊匹配、类型过滤和目录限制
 * 
 * 参数:
 * - query: 搜索关键词（可选，默认 "*" 列出所有资产，使用 "*" 作为通配符）
 * - path: 目录路径限制，如 /Game/Blueprints（可选）
 * - filter_class: 类型过滤（可选）
 * - include_folders: 是否返回文件夹信息（可选，默认 false）
 * - limit: 返回数量限制（可选，默认 100，最大 500）
 */
void FUAL_ContentBrowserCommands::Handle_SearchAssets(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 解析参数
	FString Query;
	Payload->TryGetStringField(TEXT("query"), Query);
	
	// 目录路径限制（新增参数）
	FString SearchPath;
	Payload->TryGetStringField(TEXT("path"), SearchPath);
	
	FString FilterClass;
	Payload->TryGetStringField(TEXT("filter_class"), FilterClass);
	
	// 是否返回文件夹信息（新增参数）
	bool bIncludeFolders = false;
	Payload->TryGetBoolField(TEXT("include_folders"), bIncludeFolders);
	
	// limit 默认 100，最大 500（提升上限）
	int32 Limit = 100;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 500);
	
	// 支持通配符：如果 query 为空或为 "*"，则匹配所有资产
	bool bMatchAll = Query.IsEmpty() || Query == TEXT("*");
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.search: query=%s, path=%s, filter_class=%s, include_folders=%d, limit=%d, match_all=%d"),
		*Query, *SearchPath, *FilterClass, bIncludeFolders, Limit, bMatchAll);
	
	// 获取 Asset Registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 构建过滤器
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	
	// 路径限制：优先使用 path 参数，否则默认 /Game
	if (!SearchPath.IsEmpty() && SearchPath.StartsWith(TEXT("/Game")))
	{
		Filter.PackagePaths.Add(FName(*SearchPath));
	}
	else
	{
		Filter.PackagePaths.Add(TEXT("/Game"));
	}
	
	// 类型过滤 - UE 5.1+ 使用 ClassPaths，5.0 使用 ClassNames
	if (!FilterClass.IsEmpty())
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Engine"), *FilterClass));
#else
		Filter.ClassNames.Add(FName(*FilterClass));
#endif
	}
	
	// 执行搜索
	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);
	
	// 收集文件夹信息（如果需要）
	TSet<FString> FolderPaths;
	
	// 过滤匹配结果
	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : AssetList)
	{
		if (Results.Num() >= Limit) break;
		
		const FString AssetName = Asset.AssetName.ToString();
		const FString PackagePath = Asset.PackageName.ToString();
		
		// 通配符匹配或模糊匹配: 名称或路径包含查询字符串
		bool bMatches = bMatchAll || 
			AssetName.Contains(Query, ESearchCase::IgnoreCase) ||
			PackagePath.Contains(Query, ESearchCase::IgnoreCase);
		
		if (bMatches)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), AssetName);
			Item->SetStringField(TEXT("path"), PackagePath);
			
			// UE 5.1+ 使用 AssetClassPath，5.0 使用 AssetClass
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			Item->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
#else
			Item->SetStringField(TEXT("class"), Asset.AssetClass.ToString());
#endif
			
			Results.Add(MakeShared<FJsonValueObject>(Item));
			
			// 收集文件夹路径
			if (bIncludeFolders)
			{
				FString FolderPath = FPackageName::GetLongPackagePath(PackagePath);
				FolderPaths.Add(FolderPath);
			}
		}
	}
	
	// 返回结果
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetNumberField(TEXT("count"), Results.Num());
	Response->SetArrayField(TEXT("results"), Results);
	
	// 如果需要，添加文件夹信息
	if (bIncludeFolders && FolderPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FolderArray;
		for (const FString& Folder : FolderPaths)
		{
			FolderArray.Add(MakeShared<FJsonValueString>(Folder));
		}
		Response->SetArrayField(TEXT("folders"), FolderArray);
		Response->SetNumberField(TEXT("folder_count"), FolderPaths.Num());
	}
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Response);
}


/**
 * content.import - 导入外部文件
 * 将磁盘上的文件导入到 UE 项目中
 * 使用 UAssetImportTask 实现无弹窗自动化导入（类似 Quixel Bridge）
 */
void FUAL_ContentBrowserCommands::Handle_ImportAssets(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 解析 files 数组
	const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("files"), FilesArray) || !FilesArray || FilesArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty 'files' array"));
		return;
	}
	
	// 解析目标路径
	FString DestinationPath = TEXT("/Game/Imported");
	Payload->TryGetStringField(TEXT("destination_path"), DestinationPath);
	
	// 是否覆盖
	bool bOverwrite = false;
	Payload->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	
	// 解析 normalized_names 数组，建立文件名到规范化名称的映射
	// 格式: [{ "original": "原始文件名.ext", "normalized": "规范化名称" }, ...]
	TMap<FString, FString> NormalizedNameMap;
	const TArray<TSharedPtr<FJsonValue>>* NormalizedNamesArray = nullptr;
	if (Payload->TryGetArrayField(TEXT("normalized_names"), NormalizedNamesArray) && NormalizedNamesArray)
	{
		for (const TSharedPtr<FJsonValue>& Item : *NormalizedNamesArray)
		{
			const TSharedPtr<FJsonObject>* ItemObj = nullptr;
			if (Item->TryGetObject(ItemObj) && ItemObj)
			{
				FString Original, Normalized;
				(*ItemObj)->TryGetStringField(TEXT("original"), Original);
				(*ItemObj)->TryGetStringField(TEXT("normalized"), Normalized);
				if (!Original.IsEmpty() && !Normalized.IsEmpty())
				{
					// 移除扩展名，用文件名（不含扩展名）作为键
					FString OriginalBaseName = FPaths::GetBaseFilename(Original);
					NormalizedNameMap.Add(OriginalBaseName, Normalized);
					UE_LOG(LogUALContentCmd, Log, TEXT("Name mapping: %s -> %s"), *OriginalBaseName, *Normalized);
				}
			}
		}
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.import: %d files -> %s, overwrite=%d, name_mappings=%d"),
		FilesArray->Num(), *DestinationPath, bOverwrite, NormalizedNameMap.Num());
	
	// 收集导入结果（提前声明，用于汇总视频和普通文件的导入结果）
	TArray<TSharedPtr<FJsonValue>> ImportedResults;
	int32 SuccessCount = 0;
	int32 TotalRequestCount = 0;
	
	// === 第一阶段：分离视频文件和其他文件 ===
	TArray<FString> VideoFiles;
	TArray<FString> OtherFiles;
	
	for (const TSharedPtr<FJsonValue>& FileValue : *FilesArray)
	{
		FString FilePath;
		if (FileValue->TryGetString(FilePath) && !FilePath.IsEmpty())
		{
			// 验证文件存在
			if (!FPaths::FileExists(FilePath))
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("File not found: %s"), *FilePath);
				continue;
			}
			
			TotalRequestCount++;
			
			// 检查是否是视频文件
			if (IsVideoFile(FilePath))
			{
				VideoFiles.Add(FilePath);
				UE_LOG(LogUALContentCmd, Log, TEXT("Detected video file: %s"), *FilePath);
			}
			else
			{
				OtherFiles.Add(FilePath);
			}
		}
	}
	
	// === 第二阶段：导入视频文件（特殊处理） ===
	if (VideoFiles.Num() > 0)
	{
		UE_LOG(LogUALContentCmd, Log, TEXT("Processing %d video file(s) with special import logic..."), VideoFiles.Num());
		
		for (const FString& VideoFilePath : VideoFiles)
		{
			UFileMediaSource* ImportedMediaSource = nullptr;
			FString ImportError;
			
			// 查找该视频文件的规范化名称
			FString VideoBaseName = FPaths::GetBaseFilename(VideoFilePath);
			const FString* NormalizedVideoName = NormalizedNameMap.Find(VideoBaseName);
			FString FinalVideoAssetName = NormalizedVideoName ? *NormalizedVideoName : FString();
			
			if (!FinalVideoAssetName.IsEmpty())
			{
				UE_LOG(LogUALContentCmd, Log, TEXT("Video file normalized name: %s -> %s"), *VideoBaseName, *FinalVideoAssetName);
			}
			
			if (ImportVideoFile(VideoFilePath, DestinationPath, bOverwrite, FinalVideoAssetName, ImportedMediaSource, ImportError))
			{
				if (ImportedMediaSource)
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), ImportedMediaSource->GetName());
					Item->SetStringField(TEXT("path"), ImportedMediaSource->GetPathName());
					Item->SetStringField(TEXT("class"), TEXT("FileMediaSource"));
					Item->SetStringField(TEXT("source_file"), VideoFilePath);
					ImportedResults.Add(MakeShared<FJsonValueObject>(Item));
					SuccessCount++;
					
					UE_LOG(LogUALContentCmd, Log, TEXT("Successfully imported video: %s -> %s"), 
						*VideoFilePath, *ImportedMediaSource->GetPathName());
				}
			}
			else
			{
				UE_LOG(LogUALContentCmd, Error, TEXT("Failed to import video file: %s - %s"), 
					*VideoFilePath, *ImportError);
			}
		}
	}
	
	// === 第三阶段：导入其他文件（标准导入流程） ===
	TArray<UAssetImportTask*> ImportTasks;
	
	for (const FString& FilePath : OtherFiles)
	{
		// 创建导入任务
		UAssetImportTask* Task = NewObject<UAssetImportTask>();
		Task->Filename = FilePath;
		Task->DestinationPath = DestinationPath;
		
		// 关键设置：禁用所有UI，实现无弹窗导入
		Task->bAutomated = true;
		// 不自动保存，避免触发源码管理检出对话框
		// 资产将保持未保存状态，用户可稍后手动保存
		Task->bSave = false;
		Task->bReplaceExisting = bOverwrite;
		
		// 获取文件扩展名
		FString Extension = FPaths::GetExtension(FilePath).ToLower();
		
		// 为 FBX 文件配置自动导入选项
		if (Extension == TEXT("fbx"))
		{
			UFbxImportUI* ImportUI = NewObject<UFbxImportUI>();
			
			// 禁用自动检测，明确指定为静态网格体
			ImportUI->bAutomatedImportShouldDetectType = false;
			ImportUI->MeshTypeToImport = FBXIT_StaticMesh;
			
			// 自动导入材质和纹理
			ImportUI->bImportMaterials = true;
			ImportUI->bImportTextures = true;
			
			// 应用到任务
			Task->Options = ImportUI;
			
			UE_LOG(LogUALContentCmd, Log, TEXT("Configured FBX import for: %s"), *FilePath);
		}
		else
		{
			UE_LOG(LogUALContentCmd, Log, TEXT("Using default import settings for: %s (Extension: %s)"), *FilePath, *Extension);
		}
		
		ImportTasks.Add(Task);
	}
	
	if (ImportTasks.Num() == 0 && VideoFiles.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("No valid files to import"));
		return;
	}
	// === 第四阶段：执行标准导入任务 ===
	if (ImportTasks.Num() > 0)
	{
		// 获取 AssetTools
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		IAssetTools& AssetTools = AssetToolsModule.Get();
		
		// 执行批量导入任务（无弹窗）
		UE_LOG(LogUALContentCmd, Log, TEXT("Executing %d automated import tasks..."), ImportTasks.Num());
		AssetTools.ImportAssetTasks(ImportTasks);
	}
	
	// 收集标准导入的结果（追加到 ImportedResults）
	TArray<UTexture2D*> ImportedTextures;
	TArray<UStaticMesh*> ImportedMeshes;
	
	for (UAssetImportTask* Task : ImportTasks)
	{
		// 检查任务是否成功（通过ImportedObjectPaths检查）
		if (Task->ImportedObjectPaths.Num() > 0)
		{
			// 获取源文件名（不含扩展名），用于查找规范化名称
			const FString SourceBaseName = FPaths::GetBaseFilename(Task->Filename);
			
			// 复制数组以避免在重命名操作中修改原数组导致崩溃
			// ("Array has changed during ranged-for iteration" bug fix)
			TArray<FString> ObjectPathsCopy = Task->ImportedObjectPaths;
			for (const FString& ObjectPath : ObjectPathsCopy)
			{
				// 检查路径是否为空
				if (ObjectPath.IsEmpty())
				{
					UE_LOG(LogUALContentCmd, Warning, TEXT("Skipping empty ObjectPath in import task for: %s"), *Task->Filename);
					continue;
				}
				
				// 加载导入的资产
				UObject* ImportedAsset = LoadObject<UObject>(nullptr, *ObjectPath);
				if (ImportedAsset)
				{
					FString FinalAssetName = ImportedAsset->GetName();
					FString FinalAssetPath = ImportedAsset->GetPathName();
					
					// 检查是否需要重命名（如果有规范化名称映射）
					// 首先尝试用当前资产名查找，然后尝试用源文件名查找
					const FString* NormalizedName = NormalizedNameMap.Find(ImportedAsset->GetName());
					if (!NormalizedName)
					{
						NormalizedName = NormalizedNameMap.Find(SourceBaseName);
					}
					
					if (NormalizedName && !NormalizedName->IsEmpty() && *NormalizedName != ImportedAsset->GetName())
					{
						UE_LOG(LogUALContentCmd, Log, TEXT("Renaming asset: %s -> %s"), 
							*ImportedAsset->GetName(), **NormalizedName);
						
						// 获取资产所在的包路径
						FString PackagePath = FPackageName::GetLongPackagePath(ImportedAsset->GetOutermost()->GetName());
						
						// 使用 AssetTools 重命名资产
						FAssetToolsModule& AssetToolsMod = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
						IAssetTools& AssetToolsRef = AssetToolsMod.Get();
						
						TArray<FAssetRenameData> RenameData;
// 使用 TWeakObjectPtr<UObject> 构造函数，兼容所有 UE5 版本
						RenameData.Add(FAssetRenameData(ImportedAsset, PackagePath, *NormalizedName));
						
						if (RenameData.Num() > 0)
						{
							bool bRenameSuccess = AssetToolsRef.RenameAssets(RenameData);
							if (bRenameSuccess)
							{
								FinalAssetName = *NormalizedName;
								FinalAssetPath = PackagePath / *NormalizedName;
								UE_LOG(LogUALContentCmd, Log, TEXT("Successfully renamed asset to: %s"), *FinalAssetPath);
							}
							else
							{
								UE_LOG(LogUALContentCmd, Warning, TEXT("Failed to rename asset: %s -> %s"), 
									*ImportedAsset->GetName(), **NormalizedName);
							}
						}
					}
					
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), FinalAssetName);
					Item->SetStringField(TEXT("path"), FinalAssetPath);
					Item->SetStringField(TEXT("class"), ImportedAsset->GetClass()->GetName());
					ImportedResults.Add(MakeShared<FJsonValueObject>(Item));
					SuccessCount++;
					
					// 🎨 收集纹理和网格体，用于PBR材质生成
					if (UTexture2D* Texture = Cast<UTexture2D>(ImportedAsset))
					{
						ImportedTextures.Add(Texture);
					}
					else if (UStaticMesh* Mesh = Cast<UStaticMesh>(ImportedAsset))
					{
						ImportedMeshes.Add(Mesh);
					}
				}
			}
		}
		else
		{
			UE_LOG(LogUALContentCmd, Warning, TEXT("No assets imported from: %s"), *Task->Filename);
		}
	}
	
	// 🚀 自动生成PBR材质（如果导入了纹理）
	TArray<UMaterialInstanceConstant*> CreatedMaterials;
	if (ImportedTextures.Num() > 0)
	{
		UE_LOG(LogUALContentCmd, Log, 
			TEXT("Starting automatic PBR material generation for %d textures..."), 
			ImportedTextures.Num());
		
		// 配置PBR处理选项
		FUAL_PBRMaterialOptions PBROptions;
		PBROptions.bApplyToMesh = true;           // 自动应用到网格体
		PBROptions.bUseStandardNaming = true;     // 使用标准命名（MI_前缀）
		PBROptions.bAutoConfigureTextures = true;  // 自动配置纹理设置
		
		// 批量处理PBR资产
		int32 MaterialCount = FUAL_PBRMaterialHelper::BatchProcessPBRAssets(
			ImportedTextures,
			ImportedMeshes,
			DestinationPath,
			PBROptions,
			CreatedMaterials);
		
		if (MaterialCount > 0)
		{
			UE_LOG(LogUALContentCmd, Log, 
				TEXT("✨ Successfully created %d PBR material(s) automatically!"), 
				MaterialCount);
			
			// 将创建的材质也添加到返回结果中
			for (UMaterialInstanceConstant* Material : CreatedMaterials)
			{
				if (Material)
				{
					TSharedPtr<FJsonObject> MatItem = MakeShared<FJsonObject>();
					MatItem->SetStringField(TEXT("name"), Material->GetName());
					MatItem->SetStringField(TEXT("path"), Material->GetPathName());
					MatItem->SetStringField(TEXT("class"), TEXT("MaterialInstanceConstant"));
					MatItem->SetBoolField(TEXT("auto_generated"), true);
					ImportedResults.Add(MakeShared<FJsonValueObject>(MatItem));
					SuccessCount++;
				}
			}
		}
	}
	
	// Show notification (Ensure logic runs on GameThread)
	if (SuccessCount > 0)
	{
		// Capture by value
		AsyncTask(ENamedThreads::GameThread, [SuccessCount]()
		{
			UE_LOG(LogUALContentCmd, Log, TEXT("Handle_ImportAssets: Attempting to show success notification for %d assets"), SuccessCount);

			FString Title = UAL_CommandUtils::LStr(TEXT("导入成功"), TEXT("Import Successful"));
			FString Msg = FString::Printf(TEXT("%s: %d"), *UAL_CommandUtils::LStr(TEXT("成功导入资产数"), TEXT("Assets imported")), SuccessCount);
			
			FNotificationInfo Info(FText::FromString(Title));
			Info.SubText = FText::FromString(Msg);
			Info.ExpireDuration = 3.0f;
			Info.bFireAndForget = true;
			Info.bUseLargeFont = false;
			
			TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
			if (NotificationItem.IsValid())
			{
				NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
			}
			else
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("Handle_ImportAssets: Failed to create notification item"));
			}
		});
	}

	// 返回结果
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), SuccessCount > 0);
	if (SuccessCount == 0)
	{
		Response->SetStringField(TEXT("error"), TEXT("Failed to import assets. Possible reasons: 1) File type not supported by installed plugins, 2) Invalid file path. Check Output Log for details."));
	}
	Response->SetNumberField(TEXT("imported_count"), SuccessCount);
	Response->SetNumberField(TEXT("requested_count"), TotalRequestCount);
	Response->SetArrayField(TEXT("imported"), ImportedResults);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Response);
}

/**
 * content.move - 移动/重命名资产
 * 移动资产或通过修改目标路径实现重命名
 */
void FUAL_ContentBrowserCommands::Handle_MoveAsset(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 解析参数
	FString SourcePath, DestinationPath;
	
	if (!Payload->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required parameter: source_path"));
		return;
	}
	
	if (!Payload->TryGetStringField(TEXT("destination_path"), DestinationPath) || DestinationPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required parameter: destination_path"));
		return;
	}
	
	bool bAutoRename = false;
	Payload->TryGetBoolField(TEXT("auto_rename"), bAutoRename);

	UE_LOG(LogUALContentCmd, Log, TEXT("content.move: %s -> %s, auto_rename=%d"), 
		*SourcePath, *DestinationPath, bAutoRename);
	
	// 加载源资产 - 支持 PackageName 和 ObjectPath 两种格式
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	FAssetData SourceAsset;
	
	// 尝试1: 直接作为 ObjectPath（格式: /Game/xxx/Asset.Asset）
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	SourceAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(SourcePath));
#else
	SourceAsset = AssetRegistry.GetAssetByObjectPath(FName(*SourcePath));
#endif
	
	// 尝试2: 如果失败，尝试构造 ObjectPath（格式: /Game/xxx/Asset -> /Game/xxx/Asset.Asset）
	if (!SourceAsset.IsValid())
	{
		FString AssetName = FPaths::GetBaseFilename(SourcePath);
		FString FullObjectPath = SourcePath + TEXT(".") + AssetName;
		UE_LOG(LogUALContentCmd, Log, TEXT("Trying full object path: %s"), *FullObjectPath);
		
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		SourceAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
#else
		SourceAsset = AssetRegistry.GetAssetByObjectPath(FName(*FullObjectPath));
#endif
	}
	
	// 尝试3: 通过 PackageName 查找
	if (!SourceAsset.IsValid())
	{
		TArray<FAssetData> AssetList;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetRegistry.GetAssetsByPackageName(FName(*SourcePath), AssetList);
#else
		AssetRegistry.GetAssetsByPackageName(FName(*SourcePath), AssetList);
#endif
		if (AssetList.Num() > 0)
		{
			SourceAsset = AssetList[0];
			UE_LOG(LogUALContentCmd, Log, TEXT("Found via PackageName: %s"), *SourceAsset.PackageName.ToString());
		}
	}
	
	if (!SourceAsset.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Source asset not found: %s (tried ObjectPath, FullObjectPath, and PackageName)"), *SourcePath));
		return;
	}
	
	// 获取 AssetTools
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();
	
	// 解析目标路径
	FString DestPackagePath, DestAssetName;
	DestinationPath.Split(TEXT("/"), &DestPackagePath, &DestAssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	
	if (DestPackagePath.IsEmpty() || DestAssetName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Invalid destination_path format"));
		return;
	}
	
	// 检查目标是否存在，处理自动重命名
	FString FinalDestAssetName = DestAssetName;
	bool bRenamed = false;
	
	auto CheckAssetExists = [&](const FString& PackagePath, const FString& AssetName) -> bool {
		FString FullPath = PackagePath / AssetName + TEXT(".") + AssetName;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		FAssetData ExistingAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FullPath));
		if (!ExistingAsset.IsValid())
		{
			ExistingAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(PackagePath / AssetName));
		}
#else
		FAssetData ExistingAsset = AssetRegistry.GetAssetByObjectPath(FName(*FullPath));
		if (!ExistingAsset.IsValid())
		{
			ExistingAsset = AssetRegistry.GetAssetByObjectPath(FName(*(PackagePath / AssetName)));
		}
#endif
		// 还要检查是否只是包存在但没有资产
		if (!ExistingAsset.IsValid())
		{
			TArray<FAssetData> PkgAssets;
			AssetRegistry.GetAssetsByPackageName(FName(*(PackagePath / AssetName)), PkgAssets);
			return PkgAssets.Num() > 0;
		}
		
		return ExistingAsset.IsValid();
	};
	
	if (CheckAssetExists(DestPackagePath, FinalDestAssetName))
	{
		if (bAutoRename)
		{
			int32 Suffix = 1;
			FString BaseName = DestAssetName;
			while (CheckAssetExists(DestPackagePath, FinalDestAssetName))
			{
				FinalDestAssetName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
				if (Suffix > 1000) break;
			}
			bRenamed = true;
			UE_LOG(LogUALContentCmd, Log, TEXT("Auto-renamed collision: %s -> %s"), *DestAssetName, *FinalDestAssetName);
		}
		else
		{
			UAL_CommandUtils::SendError(RequestId, 409, 
				FString::Printf(TEXT("Asset already exists at destination: %s/%s"), *DestPackagePath, *DestAssetName));
			return;
		}
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Move asset: %s -> %s/%s"), 
		*SourcePath, *DestPackagePath, *FinalDestAssetName);
	
	// 加载源资产对象
	UObject* SourceObject = SourceAsset.GetAsset();
	if (!SourceObject)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to load source asset object"));
		return;
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Source object loaded: %s (Class: %s)"), 
		*SourceObject->GetPathName(), *SourceObject->GetClass()->GetName());
	
	// 构建完整的新路径
	FString NewPackageName = DestPackagePath / FinalDestAssetName;
	
	UE_LOG(LogUALContentCmd, Log, TEXT("New package path: %s, New asset name: %s, Full new path: %s"), 
		*DestPackagePath, *FinalDestAssetName, *NewPackageName);
	
	// 构建重命名数据
	TArray<FAssetRenameData> RenameData;
	
	// 确保资产在内存中被正确标记
	SourceObject->MarkPackageDirty();
	
// 使用 TWeakObjectPtr<UObject> 构造函数，兼容所有 UE5 版本
	FAssetRenameData RenameItem(SourceObject, DestPackagePath, FinalDestAssetName);
	RenameData.Add(RenameItem);
	UE_LOG(LogUALContentCmd, Log, TEXT("FAssetRenameData: Object=%s, NewPath=%s, NewName=%s"), 
		*SourceObject->GetPathName(), *DestPackagePath, *FinalDestAssetName);
	
	// 执行移动/重命名
	bool bSuccess = AssetTools.RenameAssets(RenameData);
	
	// 验证移动是否真正成功（检查目标位置是否存在资产）
	FString NewAssetPath = DestPackagePath / FinalDestAssetName;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	FAssetData NewAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NewAssetPath + TEXT(".") + FinalDestAssetName));
#else
	FAssetData NewAssetData = AssetRegistry.GetAssetByObjectPath(FName(*(NewAssetPath + TEXT(".") + FinalDestAssetName)));
#endif
	
	// 如果标准路径找不到，尝试直接路径
	if (!NewAssetData.IsValid())
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		NewAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NewAssetPath));
#else
		NewAssetData = AssetRegistry.GetAssetByObjectPath(FName(*NewAssetPath));
#endif
	}
	
	bool bActuallyMoved = NewAssetData.IsValid();
	
	// 如果移动成功，保存新位置的资产包
	bool bSaved = false;
	if (bSuccess && bActuallyMoved)
	{
		UObject* MovedAsset = NewAssetData.GetAsset();
		if (MovedAsset)
		{
			UPackage* Package = MovedAsset->GetOutermost();
			if (Package)
			{
			FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
				
				// 使用新的 SavePackageArgs API（UE 5.0+ 统一使用）
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3
				FSavePackageResultStruct Result = UPackage::Save(Package, MovedAsset, *PackageFileName, SaveArgs);
				bSaved = Result.Result == ESavePackageResult::Success;
#else
				bSaved = UPackage::SavePackage(Package, MovedAsset, *PackageFileName, SaveArgs);
#endif
				UE_LOG(LogUALContentCmd, Log, TEXT("Saved moved asset: %s (Success: %s)"), *PackageFileName, bSaved ? TEXT("true") : TEXT("false"));
			}
		}
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("RenameAssets returned: %s, Asset at new location: %s, Saved: %s"), 
		bSuccess ? TEXT("true") : TEXT("false"),
		bActuallyMoved ? TEXT("found") : TEXT("not found"),
		bSaved ? TEXT("true") : TEXT("false"));
	
	// 返回结果
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), bSuccess && bActuallyMoved);
	Response->SetStringField(TEXT("source_path"), SourcePath);
	Response->SetStringField(TEXT("destination_path"), DestPackagePath / FinalDestAssetName);
	
	if (bRenamed)
	{
		Response->SetBoolField(TEXT("renamed"), true);
		Response->SetStringField(TEXT("original_destination"), DestinationPath);
	}
	
	Response->SetBoolField(TEXT("saved"), bSaved);
	
	if (bSuccess && bActuallyMoved)
	{
		FString Msg = bRenamed 
			? FString::Printf(TEXT("Asset moved and auto-renamed: %s -> %s"), *SourcePath, *FinalDestAssetName)
			: TEXT("Asset moved/renamed successfully");
		Response->SetStringField(TEXT("message"), Msg);
	}
	else if (bSuccess && !bActuallyMoved)
	{
		Response->SetBoolField(TEXT("ok"), false);
		Response->SetStringField(TEXT("error"), TEXT("RenameAssets returned success but asset was not found at new location. Check if target folder exists."));
	}
	else
	{
		Response->SetStringField(TEXT("error"), TEXT("Failed to move/rename asset"));
	}
	
	UAL_CommandUtils::SendResponse(RequestId, (bSuccess && bActuallyMoved) ? 200 : 500, Response);
}

/**
 * content.delete - 删除资产
 * 彻底删除资产或文件夹（无对话框，使用 ForceDeleteObjects）
 */
void FUAL_ContentBrowserCommands::Handle_DeleteAssets(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 🔑 关键：进入无人值守模式，跳过所有交互式对话框
	// 这与 UE5.3 的 UEditorAssetSubsystem::DeleteAsset 使用相同的策略
	TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
	
	// 解析 paths 数组
	const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("paths"), PathsArray) || !PathsArray || PathsArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty 'paths' array"));
		return;
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.delete: %d paths (Unattended mode enabled)"), PathsArray->Num());
	
	// 获取 Asset Registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 收集要删除的资产
	TArray<UObject*> ObjectsToDelete;
	TArray<FString> DeletedPaths;
	TArray<FString> FailedPaths;
	
	for (const TSharedPtr<FJsonValue>& PathValue : *PathsArray)
	{
		FString AssetPath;
		if (!PathValue->TryGetString(AssetPath) || AssetPath.IsEmpty())
		{
			continue;
		}
		
		// 尝试多种方式查找资产
		FAssetData AssetData;
		
		// 尝试1: 直接作为 ObjectPath
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
#else
		AssetData = AssetRegistry.GetAssetByObjectPath(FName(*AssetPath));
#endif
		
		// 尝试2: 构造完整 ObjectPath
		if (!AssetData.IsValid())
		{
			FString AssetName = FPaths::GetBaseFilename(AssetPath);
			FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
			AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
#else
			AssetData = AssetRegistry.GetAssetByObjectPath(FName(*FullObjectPath));
#endif
		}
		
		// 尝试3: 通过 PackageName 查找
		if (!AssetData.IsValid())
		{
			TArray<FAssetData> AssetList;
			AssetRegistry.GetAssetsByPackageName(FName(*AssetPath), AssetList);
			if (AssetList.Num() > 0)
			{
				AssetData = AssetList[0];
			}
		}
		
		if (AssetData.IsValid())
		{
			UE_LOG(LogUALContentCmd, Log, TEXT("Found valid AssetData for: %s, PackageName: %s"), 
				*AssetPath, *AssetData.PackageName.ToString());
			
			UObject* Asset = AssetData.GetAsset();
			if (Asset)
			{
				UE_LOG(LogUALContentCmd, Log, TEXT("Successfully loaded asset: %s"), *Asset->GetPathName());
				ObjectsToDelete.Add(Asset);
				DeletedPaths.Add(AssetPath);
			}
			else
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("Failed to load asset object for: %s"), *AssetPath);
				FailedPaths.Add(AssetPath);
			}
		}
		else
		{
			// 可能是文件夹路径，记录失败
			FailedPaths.Add(AssetPath);
			UE_LOG(LogUALContentCmd, Warning, TEXT("Asset not found: %s"), *AssetPath);
		}
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Collected %d objects to delete, %d failed paths"), 
		ObjectsToDelete.Num(), FailedPaths.Num());
	
	// 使用 ForceDeleteObjects 执行删除（配合 UnattendedScriptGuard 完全无对话框）
	int32 DeletedCount = 0;
	if (ObjectsToDelete.Num() > 0)
	{
		UE_LOG(LogUALContentCmd, Log, TEXT("Calling ForceDeleteObjects with %d objects..."), ObjectsToDelete.Num());
		const bool bShowConfirmation = false; // 不显示确认对话框
		DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, bShowConfirmation);
		UE_LOG(LogUALContentCmd, Log, TEXT("ForceDeleteObjects returned: %d deleted"), DeletedCount);
	}
	else
	{
		UE_LOG(LogUALContentCmd, Warning, TEXT("No valid objects collected for deletion"));
	}
	
	// 返回结果
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), DeletedCount > 0);
	Response->SetNumberField(TEXT("deleted_count"), DeletedCount);
	Response->SetNumberField(TEXT("requested_count"), PathsArray->Num());
	
	// 添加删除的路径列表
	TArray<TSharedPtr<FJsonValue>> DeletedArray;
	for (const FString& Path : DeletedPaths)
	{
		DeletedArray.Add(MakeShared<FJsonValueString>(Path));
	}
	Response->SetArrayField(TEXT("deleted"), DeletedArray);
	
	// 添加失败的路径列表
	if (FailedPaths.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> FailedArray;
		for (const FString& Path : FailedPaths)
		{
			FailedArray.Add(MakeShared<FJsonValueString>(Path));
		}
		Response->SetArrayField(TEXT("failed"), FailedArray);
	}
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Response);
}

/**
 * content.describe - 获取资产详情
 * 返回资产的完整信息，包括依赖项和被引用项
 * 
 * 请求参数:
 *   - path: 资产路径（必填）
 *   - include_dependencies: 是否包含依赖项（可选，默认 true）
 *   - include_referencers: 是否包含被引用项（可选，默认 true）
 * 
 * 响应:
 *   - ok: 是否成功
 *   - name: 资产名称
 *   - path: 资产完整路径
 *   - class: 资产类型
 *   - package_size: 资产包大小（字节）
 *   - dependencies: 依赖的资产列表
 *   - referencers: 引用此资产的资产列表
 */
void FUAL_ContentBrowserCommands::Handle_DescribeAsset(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1. 解析参数
	FString AssetPath;
	if (!Payload->TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required parameter: path"));
		return;
	}
	
	bool bIncludeDependencies = true;
	bool bIncludeReferencers = true;
	Payload->TryGetBoolField(TEXT("include_dependencies"), bIncludeDependencies);
	Payload->TryGetBoolField(TEXT("include_referencers"), bIncludeReferencers);
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.describe: path=%s, deps=%d, refs=%d"),
		*AssetPath, bIncludeDependencies, bIncludeReferencers);
	
	// 2. 获取 Asset Registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 3. 查找资产
	FAssetData AssetData;
	
	// 尝试1: 直接作为 ObjectPath
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
#else
	AssetData = AssetRegistry.GetAssetByObjectPath(FName(*AssetPath));
#endif
	
	// 尝试2: 构造完整 ObjectPath
	if (!AssetData.IsValid())
	{
		FString AssetName = FPaths::GetBaseFilename(AssetPath);
		FString FullObjectPath = AssetPath + TEXT(".") + AssetName;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
#else
		AssetData = AssetRegistry.GetAssetByObjectPath(FName(*FullObjectPath));
#endif
	}
	
	// 尝试3: 通过 PackageName 查找
	if (!AssetData.IsValid())
	{
		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssetsByPackageName(FName(*AssetPath), AssetList);
		if (AssetList.Num() > 0)
		{
			AssetData = AssetList[0];
		}
	}
	
	if (!AssetData.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 404, 
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
		return;
	}
	
	// 4. 构建响应
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
	
	// 获取完整路径
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	Response->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
	Response->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
#else
	Response->SetStringField(TEXT("path"), AssetData.ObjectPath.ToString());
	Response->SetStringField(TEXT("class"), AssetData.AssetClass.ToString());
#endif
	
	Response->SetStringField(TEXT("package"), AssetData.PackageName.ToString());
	
	// 5. 获取包大小（估算）
	int64 PackageSize = 0;
	FString PackageFileName;
	if (FPackageName::DoesPackageExist(AssetData.PackageName.ToString(), &PackageFileName))
	{
		PackageSize = IFileManager::Get().FileSize(*PackageFileName);
	}
	Response->SetNumberField(TEXT("package_size_bytes"), (double)PackageSize);
	
	// 6. 获取依赖项
	if (bIncludeDependencies)
	{
		TArray<TSharedPtr<FJsonValue>> DepsArray;
		TArray<FName> Dependencies;
		
		AssetRegistry.GetDependencies(AssetData.PackageName, Dependencies);
		
		for (const FName& DepName : Dependencies)
		{
			FString DepPath = DepName.ToString();
			// 过滤掉引擎内置资产和脚本
			if (DepPath.StartsWith(TEXT("/Game/")) || DepPath.StartsWith(TEXT("/Content/")))
			{
				TSharedPtr<FJsonObject> DepObj = MakeShared<FJsonObject>();
				DepObj->SetStringField(TEXT("path"), DepPath);
				
				// 尝试获取依赖资产的类型
				TArray<FAssetData> DepAssets;
				AssetRegistry.GetAssetsByPackageName(DepName, DepAssets);
				if (DepAssets.Num() > 0)
				{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
					DepObj->SetStringField(TEXT("class"), DepAssets[0].AssetClassPath.GetAssetName().ToString());
#else
					DepObj->SetStringField(TEXT("class"), DepAssets[0].AssetClass.ToString());
#endif
					DepObj->SetStringField(TEXT("name"), DepAssets[0].AssetName.ToString());
				}
				
				DepsArray.Add(MakeShared<FJsonValueObject>(DepObj));
			}
		}
		
		Response->SetArrayField(TEXT("dependencies"), DepsArray);
		Response->SetNumberField(TEXT("dependencies_count"), DepsArray.Num());
		
		UE_LOG(LogUALContentCmd, Log, TEXT("Found %d dependencies for %s"), DepsArray.Num(), *AssetPath);
	}
	
	// 7. 获取被引用项（哪些资产引用了这个资产）
	if (bIncludeReferencers)
	{
		TArray<TSharedPtr<FJsonValue>> RefsArray;
		TArray<FName> Referencers;
		
		AssetRegistry.GetReferencers(AssetData.PackageName, Referencers);
		
		for (const FName& RefName : Referencers)
		{
			FString RefPath = RefName.ToString();
			// 过滤掉引擎内置资产
			if (RefPath.StartsWith(TEXT("/Game/")) || RefPath.StartsWith(TEXT("/Content/")))
			{
				TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
				RefObj->SetStringField(TEXT("path"), RefPath);
				
				// 尝试获取引用资产的类型
				TArray<FAssetData> RefAssets;
				AssetRegistry.GetAssetsByPackageName(RefName, RefAssets);
				if (RefAssets.Num() > 0)
				{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
					RefObj->SetStringField(TEXT("class"), RefAssets[0].AssetClassPath.GetAssetName().ToString());
#else
					RefObj->SetStringField(TEXT("class"), RefAssets[0].AssetClass.ToString());
#endif
					RefObj->SetStringField(TEXT("name"), RefAssets[0].AssetName.ToString());
				}
				
				RefsArray.Add(MakeShared<FJsonValueObject>(RefObj));
			}
		}
		
		Response->SetArrayField(TEXT("referencers"), RefsArray);
		Response->SetNumberField(TEXT("referencers_count"), RefsArray.Num());
		
		UE_LOG(LogUALContentCmd, Log, TEXT("Found %d referencers for %s"), RefsArray.Num(), *AssetPath);
	}
	
	// 8. 添加迁移提示
	bool bHasDependencies = Response->HasField(TEXT("dependencies")) && 
		Response->GetArrayField(TEXT("dependencies")).Num() > 0;
	bool bHasReferencers = Response->HasField(TEXT("referencers")) && 
		Response->GetArrayField(TEXT("referencers")).Num() > 0;
	
	FString MigrationHint;
	if (bHasDependencies && bHasReferencers)
	{
		MigrationHint = TEXT("This asset has both dependencies and referencers. To migrate safely, include all dependencies. Referencers may need to be updated.");
	}
	else if (bHasDependencies)
	{
		MigrationHint = TEXT("This asset has dependencies. Include all listed dependencies when migrating.");
	}
	else if (bHasReferencers)
	{
		MigrationHint = TEXT("This asset is referenced by other assets. Deleting or moving may break references.");
	}
	else
	{
		MigrationHint = TEXT("This asset is self-contained with no dependencies or referencers.");
	}
	Response->SetStringField(TEXT("migration_hint"), MigrationHint);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Response);
}

/**
 * content.normalized_import - 规范化导入 uasset/umap 资产
 * 将外部工程的资产导入到规范化的目录结构中
 * 自动处理依赖闭包、包名重映射和引用修复
 */
void FUAL_ContentBrowserCommands::Handle_NormalizedImport(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 解析 files 数组
	const TArray<TSharedPtr<FJsonValue>>* FilesArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("files"), FilesArray) || !FilesArray || FilesArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty 'files' array"));
		return;
	}
	
	// 解析可选参数
	FString TargetRoot = TEXT("/Game/Imported");
	Payload->TryGetStringField(TEXT("target_root"), TargetRoot);
	
	bool bUsePascalCase = true;
	Payload->TryGetBoolField(TEXT("use_pascal_case"), bUsePascalCase);
	
	bool bAutoRenameOnConflict = true;
	Payload->TryGetBoolField(TEXT("auto_rename_on_conflict"), bAutoRenameOnConflict);
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.normalized_import: %d files -> %s"),
		FilesArray->Num(), *TargetRoot);
	
	// 收集文件路径
	TArray<FString> FilePaths;
	for (const TSharedPtr<FJsonValue>& FileValue : *FilesArray)
	{
		FString FilePath;
		if (FileValue->TryGetString(FilePath) && !FilePath.IsEmpty())
		{
			// 验证文件存在
			if (FPaths::FileExists(FilePath))
			{
				FilePaths.Add(FilePath);
			}
			else
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("File not found: %s"), *FilePath);
			}
		}
	}
	
	if (FilePaths.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("No valid files to import"));
		return;
	}
	
	// 解析语义后缀选项
	bool bUseSemanticSuffix = true;
	Payload->TryGetBoolField(TEXT("use_semantic_suffix"), bUseSemanticSuffix);
	
	// 配置导入规则
	FUALImportRuleSet RuleSet;
	RuleSet.InitDefaults();
	RuleSet.TargetRoot = TargetRoot;
	RuleSet.bUsePascalCase = bUsePascalCase;
	RuleSet.bAutoRenameOnConflict = bAutoRenameOnConflict;
	RuleSet.bUseSemanticSuffix = bUseSemanticSuffix;
	
	// 执行规范化导入
	FUALNormalizedImporter Importer;
	FUALNormalizedImportSession Session;
	
	bool bSuccess = Importer.ExecuteNormalizedImport(FilePaths, RuleSet, Session);

	// Show notification (Ensure logic runs on GameThread)
	if (bSuccess && Session.SuccessCount > 0)
	{
		int32 Count = Session.SuccessCount; // Capture by value
		AsyncTask(ENamedThreads::GameThread, [Count]()
		{
			UE_LOG(LogUALContentCmd, Log, TEXT("Handle_NormalizedImport: Attempting to show success notification for %d assets"), Count);

			FString Title = UAL_CommandUtils::LStr(TEXT("规范化导入成功"), TEXT("Normalized Import Successful"));
			FString Msg = FString::Printf(TEXT("%s: %d"), *UAL_CommandUtils::LStr(TEXT("成功处理"), TEXT("Processed")), Count);

			FNotificationInfo Info(FText::FromString(Title));
			Info.SubText = FText::FromString(Msg);
			Info.ExpireDuration = 3.0f;
			Info.bFireAndForget = true;
			Info.bUseLargeFont = false;
			
			TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
			if (NotificationItem.IsValid())
			{
				NotificationItem->SetCompletionState(SNotificationItem::CS_Success);
			}
			else
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("Handle_NormalizedImport: Failed to create notification item"));
			}
		});
	}
	
	// 构建响应
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), bSuccess);
	Response->SetNumberField(TEXT("total_files"), Session.TotalFiles);
	Response->SetNumberField(TEXT("success_count"), Session.SuccessCount);
	Response->SetNumberField(TEXT("failed_count"), Session.FailedCount);
	
	// 添加导入的资产信息
	TArray<TSharedPtr<FJsonValue>> ImportedArray;
	for (const FUALImportTargetInfo& Info : Session.TargetInfos)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("original_name"), Info.OriginalAssetName);
		Item->SetStringField(TEXT("normalized_name"), Info.NormalizedAssetName);
		Item->SetStringField(TEXT("old_path"), Info.OldPackageName.ToString());
		Item->SetStringField(TEXT("new_path"), Info.NewPackageName.ToString());
		Item->SetStringField(TEXT("class"), Info.AssetClass);
		ImportedArray.Add(MakeShared<FJsonValueObject>(Item));
	}
	Response->SetArrayField(TEXT("imported"), ImportedArray);
	
	// 添加重定向映射
	TArray<TSharedPtr<FJsonValue>> RedirectArray;
	for (const auto& Pair : Session.RedirectMap)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("from"), Pair.Key.ToString());
		Item->SetStringField(TEXT("to"), Pair.Value.ToString());
		RedirectArray.Add(MakeShared<FJsonValueObject>(Item));
	}
	Response->SetArrayField(TEXT("redirects"), RedirectArray);
	
	// 添加错误和警告
	if (Session.Errors.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrorArray;
		for (const FString& Error : Session.Errors)
		{
			ErrorArray.Add(MakeShared<FJsonValueString>(Error));
		}
		Response->SetArrayField(TEXT("errors"), ErrorArray);
	}
	
	if (Session.Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningArray;
		for (const FString& Warning : Session.Warnings)
		{
			WarningArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		Response->SetArrayField(TEXT("warnings"), WarningArray);
	}
	
	UAL_CommandUtils::SendResponse(RequestId, bSuccess ? 200 : 500, Response);
}

void FUAL_ContentBrowserCommands::Handle_AuditOptimization(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString CheckType = TEXT("All");
	Payload->TryGetStringField(TEXT("check_type"), CheckType);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Nanite 使用情况检测
	if (CheckType.Equals(TEXT("NaniteUsage"), ESearchCase::IgnoreCase) || CheckType.Equals(TEXT("All"), ESearchCase::IgnoreCase))
	{
#if ENGINE_MAJOR_VERSION >= 5
		TSharedPtr<FJsonObject> NaniteData = MakeShared<FJsonObject>();
		
		// 检查配置
		FString NaniteEnabled;
		GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.Nanite.ProjectEnabled"), NaniteEnabled, GEngineIni);
		bool bNaniteEnabledInConfig = NaniteEnabled.Equals(TEXT("True"), ESearchCase::IgnoreCase) || NaniteEnabled.Equals(TEXT("1"), ESearchCase::IgnoreCase);
		NaniteData->SetBoolField(TEXT("enabled_in_config"), bNaniteEnabledInConfig);

		// 扫描资产
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

		// 确保资产注册表已完全加载
		AssetRegistry.WaitForCompletion();

		TArray<FAssetData> MeshAssets;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetRegistry.GetAssetsByClass(UStaticMesh::StaticClass()->GetClassPathName(), MeshAssets, true);
#else
		AssetRegistry.GetAssetsByClass(UStaticMesh::StaticClass()->GetFName(), MeshAssets, true);
#endif

		int32 MeshesWithNanite = 0;
		for (const FAssetData& AssetData : MeshAssets)
		{
			// 加载资产并检查 Nanite 设置
			UStaticMesh* Mesh = Cast<UStaticMesh>(AssetData.GetAsset());
			if (Mesh && Mesh->HasValidNaniteData())
			{
				MeshesWithNanite++;
			}
		}

		NaniteData->SetNumberField(TEXT("mesh_count"), MeshAssets.Num());
		NaniteData->SetNumberField(TEXT("meshes_with_nanite"), MeshesWithNanite);

		if (bNaniteEnabledInConfig && MeshesWithNanite == 0)
		{
			NaniteData->SetStringField(TEXT("suggestion"), TEXT("检测到您开启了 Nanite 支持，但场景中没有任何模型使用了 Nanite。建议在 Project Settings 中关闭 Nanite 以剔除相关着色器变体，可显著提升构建速度。"));
		}
		else if (bNaniteEnabledInConfig && MeshesWithNanite > 0)
		{
			NaniteData->SetStringField(TEXT("suggestion"), FString::Printf(TEXT("检测到 %d 个模型使用了 Nanite，Nanite 功能正在被使用。"), MeshesWithNanite));
		}

		Result->SetObjectField(TEXT("nanite_usage"), NaniteData);
#endif
	}

	// Lumen 使用情况检测
	if (CheckType.Equals(TEXT("LumenMaterials"), ESearchCase::IgnoreCase) || CheckType.Equals(TEXT("All"), ESearchCase::IgnoreCase))
	{
#if ENGINE_MAJOR_VERSION >= 5
		TSharedPtr<FJsonObject> LumenData = MakeShared<FJsonObject>();
		
		// 检查配置
		FString LumenEnabled;
		FString DynamicGI;
		GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.Lumen.Enabled"), LumenEnabled, GEngineIni);
		GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.DynamicGlobalIlluminationMethod"), DynamicGI, GEngineIni);
		
		bool bLumenEnabledInConfig = LumenEnabled.Equals(TEXT("True"), ESearchCase::IgnoreCase) || LumenEnabled.Equals(TEXT("1"), ESearchCase::IgnoreCase);
		bool bUsingLumenGI = DynamicGI.Contains(TEXT("Lumen"), ESearchCase::IgnoreCase);
		
		LumenData->SetBoolField(TEXT("enabled_in_config"), bLumenEnabledInConfig);
		LumenData->SetBoolField(TEXT("using_lumen_gi"), bUsingLumenGI);

		// 扫描材质中的自发光使用（Lumen 特征）
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		AssetRegistry.WaitForCompletion();

		TArray<FAssetData> MaterialAssets;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		FARFilter MaterialFilter;
		MaterialFilter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
		MaterialFilter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
		AssetRegistry.GetAssets(MaterialFilter, MaterialAssets);
#else
		FARFilter MaterialFilter;
		MaterialFilter.ClassNames.Add(UMaterial::StaticClass()->GetFName());
		MaterialFilter.ClassNames.Add(UMaterialInstanceConstant::StaticClass()->GetFName());
		AssetRegistry.GetAssets(MaterialFilter, MaterialAssets);
#endif

		int32 MaterialsWithEmissive = 0;
		for (const FAssetData& AssetData : MaterialAssets)
		{
			// 检查 TagsAndValues 中是否有 Emissive 相关的标签
			// 或者尝试加载材质检查
			UMaterialInterface* Material = Cast<UMaterialInterface>(AssetData.GetAsset());
			if (Material)
			{
				// 简单检查：如果有自发光颜色或强度不为零，则认为使用了自发光
				// 注意：更精确的检查需要遍历材质节点图，这里使用简化方法
				FLinearColor EmissiveColor;
				float EmissiveStrength = 0.0f;
				
				if (Material->GetVectorParameterValue(TEXT("EmissiveColor"), EmissiveColor) ||
					Material->GetScalarParameterValue(TEXT("EmissiveStrength"), EmissiveStrength))
				{
					if (EmissiveColor.R > 0.01f || EmissiveColor.G > 0.01f || EmissiveColor.B > 0.01f || EmissiveStrength > 0.01f)
					{
						MaterialsWithEmissive++;
					}
				}
			}
		}

		LumenData->SetNumberField(TEXT("materials_with_emissive"), MaterialsWithEmissive);

		if (bLumenEnabledInConfig || bUsingLumenGI)
		{
			if (MaterialsWithEmissive > 0)
			{
				LumenData->SetStringField(TEXT("suggestion"), FString::Printf(TEXT("检测到 %d 个材质使用了自发光，Lumen 功能正在被使用。"), MaterialsWithEmissive));
			}
			else
			{
				LumenData->SetStringField(TEXT("suggestion"), TEXT("Lumen 已启用，但未检测到使用自发光的材质。如果不需要全局光照，可以考虑禁用 Lumen 以减小包体。"));
			}
		}

		Result->SetObjectField(TEXT("lumen_usage"), LumenData);
#endif
	}

	// 纹理大小分析
	if (CheckType.Equals(TEXT("TextureSize"), ESearchCase::IgnoreCase) || CheckType.Equals(TEXT("All"), ESearchCase::IgnoreCase))
	{
		TSharedPtr<FJsonObject> TextureData = MakeShared<FJsonObject>();
		
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		AssetRegistry.WaitForCompletion();

		TArray<FAssetData> TextureAssets;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		AssetRegistry.GetAssetsByClass(UTexture2D::StaticClass()->GetClassPathName(), TextureAssets, true);
#else
		AssetRegistry.GetAssetsByClass(UTexture2D::StaticClass()->GetFName(), TextureAssets, true);
#endif

		int32 TotalTextures = TextureAssets.Num();
		int32 LargeTextures4K = 0;
		int64 TotalTextureMemory = 0;

		for (const FAssetData& AssetData : TextureAssets)
		{
			UTexture2D* Texture = Cast<UTexture2D>(AssetData.GetAsset());
			if (Texture)
			{
				int32 Width = Texture->GetSizeX();
				int32 Height = Texture->GetSizeY();
				
				if (Width >= 4096 || Height >= 4096)
				{
					LargeTextures4K++;
				}

				// 估算纹理内存（简化计算：RGBA8 = 4 bytes per pixel）
				int64 TextureMemory = (int64)Width * Height * 4;
				TotalTextureMemory += TextureMemory;
			}
		}

		TextureData->SetNumberField(TEXT("total_textures"), TotalTextures);
		TextureData->SetNumberField(TEXT("large_textures_4k"), LargeTextures4K);
		TextureData->SetNumberField(TEXT("estimated_memory_bytes"), TotalTextureMemory);
		TextureData->SetNumberField(TEXT("estimated_memory_mb"), TotalTextureMemory / (1024 * 1024));

		if (LargeTextures4K > 0)
		{
			TextureData->SetStringField(TEXT("suggestion"), FString::Printf(TEXT("发现 %d 个 4K 或更大的纹理，考虑压缩或降低分辨率以减少包体大小。"), LargeTextures4K));
		}

		Result->SetObjectField(TEXT("texture_analysis"), TextureData);
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}
