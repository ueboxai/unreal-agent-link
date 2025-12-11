#include "UAL_ContentBrowserCommands.h"
#include "UAL_CommandUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

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
	
	UE_LOG(LogUALContentCmd, Log, TEXT("ContentBrowser commands registered: content.search, content.import, content.move, content.delete"));
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
	
	// 收集文件路径
	TArray<FString> FilesToImport;
	for (const TSharedPtr<FJsonValue>& FileValue : *FilesArray)
	{
		FString FilePath;
		if (FileValue->TryGetString(FilePath) && !FilePath.IsEmpty())
		{
			// 验证文件存在
			if (FPaths::FileExists(FilePath))
			{
				FilesToImport.Add(FilePath);
			}
			else
			{
				UE_LOG(LogUALContentCmd, Warning, TEXT("File not found: %s"), *FilePath);
			}
		}
	}
	
	if (FilesToImport.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("No valid files to import"));
		return;
	}
	
	// 获取 AssetTools
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();
	
	// 执行导入
	TArray<UObject*> ImportedAssets = AssetTools.ImportAssets(FilesToImport, DestinationPath);
	
	// 构建结果
	TArray<TSharedPtr<FJsonValue>> ImportedResults;
	for (UObject* Asset : ImportedAssets)
	{
		if (Asset)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Asset->GetName());
			Item->SetStringField(TEXT("path"), Asset->GetPathName());
			Item->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
			ImportedResults.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
	
	// 返回结果
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), ImportedResults.Num() > 0);
	Response->SetNumberField(TEXT("imported_count"), ImportedResults.Num());
	Response->SetNumberField(TEXT("requested_count"), FilesToImport.Num());
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
	
	UE_LOG(LogUALContentCmd, Log, TEXT("content.move: %s -> %s"), *SourcePath, *DestinationPath);
	
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
	
	UE_LOG(LogUALContentCmd, Log, TEXT("Move asset: %s -> %s/%s"), 
		*SourcePath, *DestPackagePath, *DestAssetName);
	
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
	FString NewPackageName = DestPackagePath / DestAssetName;
	
	UE_LOG(LogUALContentCmd, Log, TEXT("New package path: %s, New asset name: %s, Full new path: %s"), 
		*DestPackagePath, *DestAssetName, *NewPackageName);
	
	// 构建重命名数据
	TArray<FAssetRenameData> RenameData;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	RenameData.Add(FAssetRenameData(SourceAsset.ToSoftObjectPath(), DestPackagePath, DestAssetName));
	UE_LOG(LogUALContentCmd, Log, TEXT("Using UE 5.1+ FAssetRenameData constructor with SoftObjectPath"));
#else
	FAssetRenameData RenameItem;
	RenameItem.Asset = SourceObject;
	RenameItem.NewPackagePath = DestPackagePath;
	RenameItem.NewName = DestAssetName;
	RenameData.Add(RenameItem);
	UE_LOG(LogUALContentCmd, Log, TEXT("Using UE 5.0 FAssetRenameData with Asset pointer"));
#endif
	
	// 执行移动/重命名
	bool bSuccess = AssetTools.RenameAssets(RenameData);
	
	// 验证移动是否真正成功（检查目标位置是否存在资产）
	FString NewAssetPath = DestPackagePath / DestAssetName;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	FAssetData NewAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NewAssetPath + TEXT(".") + DestAssetName));
#else
	FAssetData NewAssetData = AssetRegistry.GetAssetByObjectPath(FName(*(NewAssetPath + TEXT(".") + DestAssetName)));
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
	
	UE_LOG(LogUALContentCmd, Log, TEXT("RenameAssets returned: %s, Asset at new location: %s"), 
		bSuccess ? TEXT("true") : TEXT("false"),
		bActuallyMoved ? TEXT("found") : TEXT("not found"));
	
	// 返回结果
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), bSuccess && bActuallyMoved);
	Response->SetStringField(TEXT("source_path"), SourcePath);
	Response->SetStringField(TEXT("destination_path"), DestinationPath);
	
	if (bSuccess && bActuallyMoved)
	{
		Response->SetStringField(TEXT("message"), TEXT("Asset moved/renamed successfully"));
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
