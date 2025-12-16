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
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Async/Async.h"

// 使用独立的 Log Category 名称，避免与 UAL_ContentBrowserExt 冲突
DEFINE_LOG_CATEGORY_STATIC(LogUALContentCmd, Log, All);

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
	
	UE_LOG(LogUALContentCmd, Log, TEXT("ContentBrowser commands registered: content.search, content.import, content.move, content.delete, content.describe, content.normalized_import"));
}

// ============================================================================
// Handler 实现
// ============================================================================

/**
 * content.search - 搜索资产
 * 支持模糊匹配和类型过滤
 */
void FUAL_ContentBrowserCommands::Handle_SearchAssets(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 解析参数
	FString Query;
	Payload->TryGetStringField(TEXT("query"), Query);
	
	FString FilterClass;
	Payload->TryGetStringField(TEXT("filter_class"), FilterClass);
	
	int32 Limit = 50;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	Limit = FMath::Clamp(Limit, 1, 200);
	
	if (Query.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing required parameter: query"));
		return;
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.search: query=%s, filter_class=%s, limit=%d"),
		*Query, *FilterClass, Limit);
	
	// 获取 Asset Registry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 构建过滤器
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	
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
	
	// 过滤匹配结果
	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : AssetList)
	{
		if (Results.Num() >= Limit) break;
		
		const FString AssetName = Asset.AssetName.ToString();
		const FString PackagePath = Asset.PackageName.ToString();
		
		// 模糊匹配: 名称或路径包含查询字符串
		if (AssetName.Contains(Query, ESearchCase::IgnoreCase) ||
			PackagePath.Contains(Query, ESearchCase::IgnoreCase))
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
		}
	}
	
	// 返回结果
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetNumberField(TEXT("count"), Results.Num());
	Response->SetArrayField(TEXT("results"), Results);
	
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
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.import: %d files -> %s, overwrite=%d"),
		FilesArray->Num(), *DestinationPath, bOverwrite);
	
	// 收集文件路径并创建导入任务
	TArray<UAssetImportTask*> ImportTasks;
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
			
			ImportTasks.Add(Task);
		}
	}
	
	if (ImportTasks.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("No valid files to import"));
		return;
	}
	
	// 获取 AssetTools
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();
	
	// 执行批量导入任务（无弹窗）
	UE_LOG(LogUALContentCmd, Log, TEXT("Executing %d automated import tasks..."), ImportTasks.Num());
	AssetTools.ImportAssetTasks(ImportTasks);
	
	// 收集导入结果
	TArray<TSharedPtr<FJsonValue>> ImportedResults;
	TArray<UTexture2D*> ImportedTextures;
	TArray<UStaticMesh*> ImportedMeshes;
	int32 SuccessCount = 0;
	
	for (UAssetImportTask* Task : ImportTasks)
	{
		// 检查任务是否成功（通过ImportedObjectPaths检查）
		if (Task->ImportedObjectPaths.Num() > 0)
		{
			for (const FString& ObjectPath : Task->ImportedObjectPaths)
			{
				// 加载导入的资产
				UObject* ImportedAsset = LoadObject<UObject>(nullptr, *ObjectPath);
				if (ImportedAsset)
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), ImportedAsset->GetName());
					Item->SetStringField(TEXT("path"), ImportedAsset->GetPathName());
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
	Response->SetNumberField(TEXT("imported_count"), SuccessCount);
	Response->SetNumberField(TEXT("requested_count"), ImportTasks.Num());
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
	
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	// UE 5.1+ 使用 SoftObjectPath 构造
	RenameData.Add(FAssetRenameData(SourceAsset.ToSoftObjectPath(), DestPackagePath, FinalDestAssetName));
	UE_LOG(LogUALContentCmd, Log, TEXT("Using UE 5.1+ FAssetRenameData constructor with SoftObjectPath"));
#else
	// UE 5.0: 使用 TWeakObjectPtr 正确初始化
	// 关键：使用 FAssetRenameData(UObject*, FString, FString) 构造函数
	FAssetRenameData RenameItem(SourceObject, DestPackagePath, FinalDestAssetName);
	RenameData.Add(RenameItem);
	UE_LOG(LogUALContentCmd, Log, TEXT("Using UE 5.0 FAssetRenameData with direct constructor: Object=%s, NewPath=%s, NewName=%s"), 
		*SourceObject->GetPathName(), *DestPackagePath, *FinalDestAssetName);
#endif
	
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
				FSavePackageResultStruct Result = UPackage::Save(Package, MovedAsset, *PackageFileName, SaveArgs);
				bSaved = Result.Result == ESavePackageResult::Success;
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
 * 彻底删除资产或文件夹
 */
void FUAL_ContentBrowserCommands::Handle_DeleteAssets(
	const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 解析 paths 数组
	const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
	if (!Payload->TryGetArrayField(TEXT("paths"), PathsArray) || !PathsArray || PathsArray->Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing or empty 'paths' array"));
		return;
	}
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.delete: %d paths"), PathsArray->Num());
	
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
			UObject* Asset = AssetData.GetAsset();
			if (Asset)
			{
				ObjectsToDelete.Add(Asset);
				DeletedPaths.Add(AssetPath);
			}
			else
			{
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
	
	// 执行删除
	int32 DeletedCount = 0;
	if (ObjectsToDelete.Num() > 0)
	{
		DeletedCount = ObjectTools::DeleteObjects(ObjectsToDelete, true);
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
