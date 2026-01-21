#include "UAL_ContentBrowserExt.h"

#include "UAL_NetworkManager.h"

#include "ContentBrowserModule.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Logging/LogMacros.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/Base64.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"

// 缩略图相关
#include "ObjectTools.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Engine/Texture2D.h"

#define LOCTEXT_NAMESPACE "FUAL_ContentBrowserExt"

DEFINE_LOG_CATEGORY_STATIC(LogUALContentBrowser, Log, All);

namespace
{
	// 简单的中英文本地化切换：根据当前文化选择中文或英文显示
	FText LocalizedText(const FString& Key, const FString& ZhText, const FString& EnText)
	{
		const FString CultureName = FInternationalization::Get().GetCurrentCulture()->GetName();
		const bool bIsZh = CultureName.StartsWith(TEXT("zh"));
		return FText::FromString(bIsZh ? ZhText : EnText);
	}

	FString LocalizedString(const FString& ZhText, const FString& EnText)
	{
		const FString CultureName = FInternationalization::Get().GetCurrentCulture()->GetName();
		const bool bIsZh = CultureName.StartsWith(TEXT("zh"));
		return bIsZh ? ZhText : EnText;
	}
	
	/**
	 * 获取资产缩略图并保存到临时文件
	 * @param AssetData 资产数据
	 * @param ThumbnailSize 缩略图大小（默认 128x128）
	 * @return PNG 文件的完整路径，失败返回空字符串
	 */
	FString SaveAssetThumbnailToFile(const FAssetData& AssetData, int32 ThumbnailSize = 128)
	{
		// 🚫 过滤掉蓝图 (Blueprint)，因为它们通常没有可视内容导致缩略图全黑
		// 用户更希望显示默认的占位图标
		if (AssetData.AssetClass == FName("Blueprint"))
		{
			return FString();
		}

		// 尝试从包中获取已有的缩略图
		FName PackageName = AssetData.PackageName;
		FString PackageString = PackageName.ToString();
		
		// 首先尝试获取已缓存的缩略图
		const FObjectThumbnail* ExistingThumbnail = ThumbnailTools::FindCachedThumbnail(PackageString);
		
		// 如果没有缓存的缩略图，尝试渲染
		FObjectThumbnail RenderedThumbnail;
		const FObjectThumbnail* ThumbnailToUse = ExistingThumbnail;
		
		if (ExistingThumbnail)
		{
			// 检查缓存缩略图的尺寸
			if (ExistingThumbnail->GetImageWidth() < ThumbnailSize || ExistingThumbnail->GetImageHeight() < ThumbnailSize)
			{
				UE_LOG(LogUALContentBrowser, Log, TEXT("缓存缩略图尺寸(%dx%d)小于请求尺寸(%d)，将强制重新渲染: %s"), 
					ExistingThumbnail->GetImageWidth(), ExistingThumbnail->GetImageHeight(), ThumbnailSize, *PackageString);
				// 忽略过小的缓存
				ThumbnailToUse = nullptr;
			}
			else
			{
				// UE_LOG(LogUALContentBrowser, Log, TEXT("使用缓存缩略图: %s"), *PackageString);
			}
		}
		
		if (!ThumbnailToUse || ThumbnailToUse->GetImageWidth() == 0)
		{
			// UE_LOG(LogUALContentBrowser, Log, TEXT("未找到有效缓存，尝试渲染缩略图: %s"), *PackageString);
			// 加载资产对象
			UObject* Asset = AssetData.GetAsset();
			if (!Asset)
			{
				return FString();
			}
			
			// 渲染缩略图
			ThumbnailTools::RenderThumbnail(
				Asset, 
				ThumbnailSize, 
				ThumbnailSize, 
				ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush, 
				nullptr, 
				&RenderedThumbnail
			);
			
			ThumbnailToUse = &RenderedThumbnail;
		}
		
		if (!ThumbnailToUse || ThumbnailToUse->GetImageWidth() == 0 || ThumbnailToUse->GetImageHeight() == 0)
		{
			UE_LOG(LogUALContentBrowser, Warning, TEXT("无法生成有效缩略图: %s"), *PackageString);
			return FString();
		}
		
		const int32 ImageWidth = ThumbnailToUse->GetImageWidth();
		const int32 ImageHeight = ThumbnailToUse->GetImageHeight();
		
		// 准备 PNG 数据
		TArray64<uint8> PngData;

		// 获取原始数据（无论源格式如何，都尝试转为 BGRA）
		TArray64<uint8> RawData;
		int32 Width = ImageWidth;
		int32 Height = ImageHeight;
		
		const TArray<uint8>& SourceData = ThumbnailToUse->AccessImageData();
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
		EImageFormat DetectedFormat = ImageWrapperModule.DetectImageFormat(SourceData.GetData(), SourceData.Num());
		
		bool bGotRawData = false;
		
		if (DetectedFormat != EImageFormat::Invalid)
		{
			// 压缩格式，需要解压
			TSharedPtr<IImageWrapper> SourceWrapper = ImageWrapperModule.CreateImageWrapper(DetectedFormat);
			if (SourceWrapper.IsValid() && SourceWrapper->SetCompressed(SourceData.GetData(), SourceData.Num()))
			{
				if (SourceWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
				{
					Width = SourceWrapper->GetWidth();
					Height = SourceWrapper->GetHeight();
					bGotRawData = true;
				}
			}
		}
		else
		{
			// 假设为未压缩的 BGRA
			if (SourceData.Num() == ImageWidth * ImageHeight * 4)
			{
				RawData.SetNumUninitialized(SourceData.Num());
				FMemory::Memcpy(RawData.GetData(), SourceData.GetData(), SourceData.Num());
				bGotRawData = true;
			}
		}

		if (bGotRawData && RawData.Num() > 0)
		{
			// 强制设置 Alpha 通道为 255 (从 3 开始，每 4 字节一个 Alpha)
			for (int32 i = 3; i < RawData.Num(); i += 4)
			{
				RawData[i] = 255;
			}
			
			// 重新压缩为 PNG
			TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
			if (PngWrapper.IsValid() && PngWrapper->SetRaw(RawData.GetData(), RawData.Num(), Width, Height, ERGBFormat::BGRA, 8))
			{
				PngData = PngWrapper->GetCompressed();
			}
		}
		else
		{
			UE_LOG(LogUALContentBrowser, Warning, TEXT("无法获取缩略图原始数据: %s"), *PackageString);
		}

		if (PngData.Num() > 0)
		{
			// 保存到临时目录
			FString TempDir = FPaths::ProjectSavedDir() / TEXT("UALinkThumbnails");
			IFileManager::Get().MakeDirectory(*TempDir, true);
			
			// 使用资产名称作为文件名
			FString SafeName = AssetData.AssetName.ToString();
			SafeName = SafeName.Replace(TEXT(" "), TEXT("_"));
			FString FilePath = TempDir / FString::Printf(TEXT("%s_%lld.png"), *SafeName, FDateTime::Now().GetTicks());
			
			// 写入文件
			if (FFileHelper::SaveArrayToFile(PngData, *FilePath))
			{
				UE_LOG(LogUALContentBrowser, Log, TEXT("✅ 缩略图已保存: %s"), *FilePath);
				return FilePath;
			}
		}
		
		return FString();
	}
}

void FUAL_ContentBrowserExt::Register()
{
	if (bRegistered)
	{
		return;
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	// 路径（文件夹）扩展
	{
		TArray<FContentBrowserMenuExtender_SelectedPaths>& Extenders = ContentBrowserModule.GetAllPathViewContextMenuExtenders();
		Extenders.Add(FContentBrowserMenuExtender_SelectedPaths::CreateRaw(this, &FUAL_ContentBrowserExt::OnExtendPathMenu));
		PathExtenderHandle = Extenders.Last().GetHandle();
	}
	// 资产扩展
	{
		TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
		Extenders.Add(FContentBrowserMenuExtender_SelectedAssets::CreateRaw(this, &FUAL_ContentBrowserExt::OnExtendAssetMenu));
		AssetExtenderHandle = Extenders.Last().GetHandle();
	}
	bRegistered = true;
}

void FUAL_ContentBrowserExt::Unregister()
{
	if (!bRegistered)
	{
		return;
	}

	if (FModuleManager::Get().IsModuleLoaded("ContentBrowser"))
	{
		FContentBrowserModule& ContentBrowserModule = FModuleManager::GetModuleChecked<FContentBrowserModule>("ContentBrowser");
		{
			TArray<FContentBrowserMenuExtender_SelectedPaths>& Extenders = ContentBrowserModule.GetAllPathViewContextMenuExtenders();
			Extenders.RemoveAll([Handle = PathExtenderHandle](const FContentBrowserMenuExtender_SelectedPaths& Delegate)
			{
				return Delegate.GetHandle() == Handle;
			});
		}
		{
			TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
			Extenders.RemoveAll([Handle = AssetExtenderHandle](const FContentBrowserMenuExtender_SelectedAssets& Delegate)
			{
				return Delegate.GetHandle() == Handle;
			});
		}
	}

	bRegistered = false;
}

TSharedRef<FExtender> FUAL_ContentBrowserExt::OnExtendPathMenu(const TArray<FString>& SelectedPaths)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	if (SelectedPaths.Num() > 0)
	{
		Extender->AddMenuExtension(
			"PathViewFolderOptions",
			EExtensionHook::Before,
			nullptr,
			FMenuExtensionDelegate::CreateRaw(this, &FUAL_ContentBrowserExt::AddMenuEntry, SelectedPaths));
	}

	return Extender;
}

TSharedRef<FExtender> FUAL_ContentBrowserExt::OnExtendAssetMenu(const TArray<FAssetData>& SelectedAssets)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	if (SelectedAssets.Num() > 0)
	{
		Extender->AddMenuExtension(
			"CommonAssetActions",
			EExtensionHook::Before,
			nullptr,
			FMenuExtensionDelegate::CreateRaw(this, &FUAL_ContentBrowserExt::AddAssetMenuEntry, SelectedAssets));
	}

	return Extender;
}

void FUAL_ContentBrowserExt::AddMenuEntry(FMenuBuilder& MenuBuilder, TArray<FString> SelectedPaths)
{
	MenuBuilder.BeginSection(NAME_None, LocalizedText(TEXT("UALSection"), TEXT("虚幻助手"), TEXT("Unreal Agent")));
	{
		MenuBuilder.AddMenuEntry(
			LocalizedText(TEXT("UALImportToAgent"), TEXT("导入到虚幻助手资产库"), TEXT("Import into Unreal Agent Asset Library")),
			LocalizedText(TEXT("UALImportToAgentTooltip"), TEXT("将选中的文件夹及其内容导入到虚幻助手中（虚幻助手需要处于打开状态）"), TEXT("Import selected folders and contents into Unreal Agent (Unreal Agent must be running)")),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"),
			FUIAction(FExecuteAction::CreateLambda([this, SelectedPaths]()
			{
				HandleImportToAgent(SelectedPaths);
			}))
		);
	}
	MenuBuilder.EndSection();
}

void FUAL_ContentBrowserExt::AddAssetMenuEntry(FMenuBuilder& MenuBuilder, TArray<FAssetData> SelectedAssets)
{
	MenuBuilder.BeginSection(NAME_None, LocalizedText(TEXT("UALSectionAsset"), TEXT("虚幻助手"), TEXT("Unreal Agent")));
	{
		MenuBuilder.AddMenuEntry(
			LocalizedText(TEXT("UALImportAssets"), TEXT("导入到虚幻助手资产库"), TEXT("Import into Unreal Agent Asset Library")),
			LocalizedText(TEXT("UALImportAssetsTooltip"), TEXT("将选中的资产导入到虚幻助手中（虚幻助手需要处于打开状态）"), TEXT("Import selected assets into Unreal Agent (Unreal Agent must be running)")),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"),
			FUIAction(FExecuteAction::CreateLambda([this, SelectedAssets]()
			{
				HandleImportAssets(SelectedAssets);
			}))
		);
	}
	MenuBuilder.EndSection();
}

void FUAL_ContentBrowserExt::HandleImportToAgent(const TArray<FString>& SelectedPaths)
{
	const FString PathsStr = FString::Join(SelectedPaths, TEXT(", "));

	// 获取 AssetRegistry 用于扫描文件夹内资产
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// 构造发送的 JSON 报文
	TArray<TSharedPtr<FJsonValue>> PathsArray;
	TArray<TSharedPtr<FJsonValue>> RealPathsArray;
	
	// 收集文件夹内所有资产用于生成元数据
	TSet<FName> ProcessedPackages;
	TSet<FName> UserSelectedPackages;  // 用户直接选中的资产（文件夹内的）
	TArray<FName> PackageQueue;
	
	for (const FString& Path : SelectedPaths)
	{
		PathsArray.Add(MakeShared<FJsonValueString>(Path));

		// 将长包路径转换为文件系统路径（目录）
		FString Filename;
		if (FPackageName::TryConvertLongPackageNameToFilename(Path, Filename, TEXT("")))
		{
			// NormalizeDirectoryName 需要传入可写字符串
			FPaths::NormalizeDirectoryName(Filename);
			const FString FullPath = FPaths::ConvertRelativePathToFull(Filename);
			RealPathsArray.Add(MakeShared<FJsonValueString>(FullPath));
			UE_LOG(LogUALContentBrowser, Log, TEXT("%s"), *FString::Printf(TEXT("%s %s -> %s"),
				*LocalizedString(TEXT("导入到虚幻助手资产库"), TEXT("Import into Unreal Agent Asset Library")),
				*Path, *FullPath));
		}
		else
		{
			UE_LOG(LogUALContentBrowser, Warning, TEXT("%s: %s"),
				*LocalizedString(TEXT("无法转换包路径为文件路径"), TEXT("Failed to convert package path to file path")),
				*Path);
		}
		
		// 🔍 扫描文件夹内所有资产（递归）
		TArray<FAssetData> FolderAssets;
		AssetRegistry.GetAssetsByPath(FName(*Path), FolderAssets, true);  // true = 递归扫描
		
		for (const FAssetData& AssetData : FolderAssets)
		{
			if (!ProcessedPackages.Contains(AssetData.PackageName))
			{
				PackageQueue.Add(AssetData.PackageName);
				ProcessedPackages.Add(AssetData.PackageName);
				UserSelectedPackages.Add(AssetData.PackageName);  // 文件夹内的资产都标记为用户选中
			}
		}
	}
	
	// BFS 遍历依赖图，收集所有 /Game 路径的依赖
	int32 QueueIndex = 0;
	while (QueueIndex < PackageQueue.Num())
	{
		FName CurrentPackage = PackageQueue[QueueIndex++];
		
		// 获取当前包的所有依赖
		TArray<FName> Dependencies;
		AssetRegistry.GetDependencies(CurrentPackage, Dependencies);
		
		for (const FName& DepPackage : Dependencies)
		{
			FString DepPath = DepPackage.ToString();
			
			// 只处理 /Game 路径的依赖（项目内资产）
			if (DepPath.StartsWith(TEXT("/Game/")) && !ProcessedPackages.Contains(DepPackage))
			{
				PackageQueue.Add(DepPackage);
				ProcessedPackages.Add(DepPackage);
				// 依赖资产不加入 UserSelectedPackages
			}
		}
	}
	
	UE_LOG(LogUALContentBrowser, Log, TEXT("📁 文件夹扫描完成: 选中 %d 个, 总共 %d 个资产(含依赖)"), 
		UserSelectedPackages.Num(), PackageQueue.Num());
	
	// 构建资产元数据数组
	TArray<TSharedPtr<FJsonValue>> AssetMetadataArray;
	
	for (const FName& PackageName : PackageQueue)
	{
		const FString PackagePath = PackageName.ToString();
		
		// 获取包中的资产数据
		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(PackageName, PackageAssets);
		
		if (PackageAssets.Num() == 0)
		{
			UE_LOG(LogUALContentBrowser, Warning, TEXT("包 %s 中没有找到资产"), *PackagePath);
			continue;
		}
		
		const FAssetData& AssetData = PackageAssets[0];
		
		// 判断是否是地图（World），决定扩展名
		bool bIsWorld = false;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		bIsWorld = AssetData.AssetClassPath == UWorld::StaticClass()->GetClassPathName();
#else
		bIsWorld = AssetData.AssetClass == UWorld::StaticClass()->GetFName();
#endif
		const FString TargetExtension = bIsWorld ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();

		FString AssetFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePath, AssetFilename, TargetExtension))
		{
			UE_LOG(LogUALContentBrowser, Warning, TEXT("无法转换包路径: %s"), *PackagePath);
			continue;
		}
		
		// 构建资产元数据
		TSharedPtr<FJsonObject> MetadataObj = MakeShared<FJsonObject>();
		
		MetadataObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		MetadataObj->SetStringField(TEXT("package"), PackagePath);
		
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		MetadataObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
#else
		MetadataObj->SetStringField(TEXT("class"), AssetData.AssetClass.ToString());
#endif
		
		// 获取直接依赖（用于元数据记录）
		TArray<FName> DirectDeps;
		AssetRegistry.GetDependencies(PackageName, DirectDeps);
		
		TArray<TSharedPtr<FJsonValue>> DepsArray;
		for (const FName& DepName : DirectDeps)
		{
			FString DepPath = DepName.ToString();
			if (DepPath.StartsWith(TEXT("/Game/")))
			{
				DepsArray.Add(MakeShared<FJsonValueString>(DepPath));
			}
		}
		MetadataObj->SetArrayField(TEXT("dependencies"), DepsArray);
		
		// 标记是否为用户选中的资产（主资产 vs 依赖资产）
		const bool bIsSelected = UserSelectedPackages.Contains(PackageName);
		MetadataObj->SetBoolField(TEXT("is_selected"), bIsSelected);
		
		// 获取文件大小
		if (!AssetFilename.IsEmpty())
		{
			int64 FileSize = IFileManager::Get().FileSize(*AssetFilename);
			if (FileSize >= 0)
			{
				MetadataObj->SetNumberField(TEXT("size"), (double)FileSize);
			}
		}
		
		// 🖼️ 获取资产缩略图（保存到临时文件，返回路径）
		// 请求 512x512 的高清缩略图
		FString ThumbnailPath = SaveAssetThumbnailToFile(AssetData, 512);
		if (!ThumbnailPath.IsEmpty())
		{
			MetadataObj->SetStringField(TEXT("thumbnail_path"), ThumbnailPath);
		}
		
		AssetMetadataArray.Add(MakeShared<FJsonValueObject>(MetadataObj));
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("paths"), PathsArray);
	if (RealPathsArray.Num() > 0)
	{
		Payload->SetArrayField(TEXT("real_paths"), RealPathsArray);
	}
	
	// 添加资产元数据
	if (AssetMetadataArray.Num() > 0)
	{
		Payload->SetArrayField(TEXT("asset_metadata"), AssetMetadataArray);
	}

	AddProjectMeta(Payload);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ver"), TEXT("1.0"));
	Root->SetStringField(TEXT("type"), TEXT("evt"));
	Root->SetStringField(TEXT("method"), TEXT("content.import_folder"));
	Root->SetObjectField(TEXT("payload"), Payload);

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	FUAL_NetworkManager::Get().SendMessage(OutJson);
	UE_LOG(LogUALContentBrowser, Log, TEXT("%s: 共 %d 个资产"),
		*LocalizedString(TEXT("已发送文件夹导入请求"), TEXT("Import folder request sent")),
		AssetMetadataArray.Num());
}

void FUAL_ContentBrowserExt::HandleImportAssets(const TArray<FAssetData>& SelectedAssets)
{
	// 获取 AssetRegistry
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	
	// 🚀 收集依赖闭包：类似虚幻引擎的迁移功能
	// 使用 Set 来追踪已处理的包，避免循环依赖
	TSet<FName> ProcessedPackages;
	TSet<FName> UserSelectedPackages;  // 用户直接选中的资产包
	TArray<FName> PackageQueue;
	
	// 初始化队列：添加用户选中的资产
	for (const FAssetData& AssetData : SelectedAssets)
	{
		if (!ProcessedPackages.Contains(AssetData.PackageName))
		{
			PackageQueue.Add(AssetData.PackageName);
			ProcessedPackages.Add(AssetData.PackageName);
			UserSelectedPackages.Add(AssetData.PackageName);  // 标记为用户选中的主资产
		}
	}
	
	// BFS 遍历依赖图，收集所有 /Game 路径的依赖
	int32 QueueIndex = 0;
	while (QueueIndex < PackageQueue.Num())
	{
		FName CurrentPackage = PackageQueue[QueueIndex++];
		
		// 获取当前包的所有依赖
		TArray<FName> Dependencies;
		AssetRegistry.GetDependencies(CurrentPackage, Dependencies);
		
		for (const FName& DepPackage : Dependencies)
		{
			FString DepPath = DepPackage.ToString();
			
			// 只处理 /Game 路径的依赖（项目内资产）
			if (DepPath.StartsWith(TEXT("/Game/")) && !ProcessedPackages.Contains(DepPackage))
			{
				PackageQueue.Add(DepPackage);
				ProcessedPackages.Add(DepPackage);
				// 依赖资产不加入 UserSelectedPackages
			}
		}
	}
	
	UE_LOG(LogUALContentBrowser, Log, TEXT("📦 依赖闭包收集完成: 用户选择 %d 个, 总共 %d 个资产(含依赖)"), 
		UserSelectedPackages.Num(), PackageQueue.Num());
	
	// 构建输出数组
	TArray<TSharedPtr<FJsonValue>> AssetPathsArray;
	TArray<TSharedPtr<FJsonValue>> AssetRealPathsArray;
	TArray<TSharedPtr<FJsonValue>> AssetMetadataArray;
	
	// 处理所有收集到的包
	for (const FName& PackageName : PackageQueue)
	{
		const FString PackagePath = PackageName.ToString();
		AssetPathsArray.Add(MakeShared<FJsonValueString>(PackagePath));
		
		// 获取包中的资产数据
		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(PackageName, PackageAssets);
		
		if (PackageAssets.Num() == 0)
		{
			UE_LOG(LogUALContentBrowser, Warning, TEXT("包 %s 中没有找到资产"), *PackagePath);
			continue;
		}
		
		const FAssetData& AssetData = PackageAssets[0];
		
		// 判断是否是地图（World），决定扩展名
		bool bIsWorld = false;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		bIsWorld = AssetData.AssetClassPath == UWorld::StaticClass()->GetClassPathName();
#else
		bIsWorld = AssetData.AssetClass == UWorld::StaticClass()->GetFName();
#endif
		const FString TargetExtension = bIsWorld ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();

		FString Filename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, TargetExtension))
		{
			const FString FullPath = FPaths::ConvertRelativePathToFull(Filename);
			AssetRealPathsArray.Add(MakeShared<FJsonValueString>(FullPath));
			UE_LOG(LogUALContentBrowser, Verbose, TEXT("添加资产: %s -> %s"), *PackagePath, *FullPath);
		}
		else
		{
			UE_LOG(LogUALContentBrowser, Warning, TEXT("无法转换包路径: %s"), *PackagePath);
			continue;
		}
		
		// 构建资产元数据
		TSharedPtr<FJsonObject> MetadataObj = MakeShared<FJsonObject>();
		
		MetadataObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		MetadataObj->SetStringField(TEXT("package"), PackagePath);
		
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		MetadataObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
#else
		MetadataObj->SetStringField(TEXT("class"), AssetData.AssetClass.ToString());
#endif
		
		// 获取直接依赖（用于元数据记录）
		TArray<FName> DirectDeps;
		AssetRegistry.GetDependencies(PackageName, DirectDeps);
		
		TArray<TSharedPtr<FJsonValue>> DepsArray;
		for (const FName& DepName : DirectDeps)
		{
			FString DepPath = DepName.ToString();
			if (DepPath.StartsWith(TEXT("/Game/")))
			{
				DepsArray.Add(MakeShared<FJsonValueString>(DepPath));
			}
		}
		MetadataObj->SetArrayField(TEXT("dependencies"), DepsArray);
		
		// 标记是否为用户选中的资产（主资产 vs 依赖资产）
		const bool bIsSelected = UserSelectedPackages.Contains(PackageName);
		MetadataObj->SetBoolField(TEXT("is_selected"), bIsSelected);
		
		// 获取文件大小
		if (!Filename.IsEmpty())
		{
			int64 FileSize = IFileManager::Get().FileSize(*Filename);
			if (FileSize >= 0)
			{
				MetadataObj->SetNumberField(TEXT("size"), (double)FileSize);
			}
		}
		
		// 🖼️ 获取资产缩略图（保存到临时文件，返回路径）
		// 请求 512x512 的高清缩略图
		FString ThumbnailPath = SaveAssetThumbnailToFile(AssetData, 512);
		if (!ThumbnailPath.IsEmpty())
		{
			MetadataObj->SetStringField(TEXT("thumbnail_path"), ThumbnailPath);
		}
		
		AssetMetadataArray.Add(MakeShared<FJsonValueObject>(MetadataObj));
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("asset_paths"), AssetPathsArray);
	if (AssetRealPathsArray.Num() > 0)
	{
		Payload->SetArrayField(TEXT("asset_real_paths"), AssetRealPathsArray);
	}
	if (AssetMetadataArray.Num() > 0)
	{
		Payload->SetArrayField(TEXT("asset_metadata"), AssetMetadataArray);
	}

	AddProjectMeta(Payload);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ver"), TEXT("1.0"));
	Root->SetStringField(TEXT("type"), TEXT("evt"));
	Root->SetStringField(TEXT("method"), TEXT("content.import_assets"));
	Root->SetObjectField(TEXT("payload"), Payload);

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	FUAL_NetworkManager::Get().SendMessage(OutJson);
	UE_LOG(LogUALContentBrowser, Log, TEXT("%s: 共 %d 个资产"),
		*LocalizedString(TEXT("已发送资产导入请求"), TEXT("Import assets request sent")),
		AssetMetadataArray.Num());
}

void FUAL_ContentBrowserExt::AddProjectMeta(TSharedPtr<FJsonObject>& Payload) const
{
	const FString ProjectName = FApp::GetProjectName();
	FString ProjectVersion(TEXT("unspecified"));
	bool bHasProjectVersion = false;

	// 从项目设置中读取版本号
	if (GConfig)
	{
		FString IniVersion;
		if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), IniVersion, GGameIni) && !IniVersion.IsEmpty())
		{
			ProjectVersion = IniVersion;
			bHasProjectVersion = true;
		}
	}
	if (!bHasProjectVersion)
	{
		UE_LOG(LogUALContentBrowser, Verbose, TEXT("%s: %s"),
			*LocalizedString(TEXT("未在项目设置中找到 ProjectVersion，使用默认值"), TEXT("ProjectVersion not found, using default")),
			*ProjectVersion);
	}

	const FString EngineVersion = FEngineVersion::Current().ToString();

	Payload->SetStringField(TEXT("project_name"), ProjectName);
	Payload->SetStringField(TEXT("project_version"), ProjectVersion);
	Payload->SetStringField(TEXT("engine_version"), EngineVersion);
}

#undef LOCTEXT_NAMESPACE

