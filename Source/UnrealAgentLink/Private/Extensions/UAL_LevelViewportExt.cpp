#include "UAL_LevelViewportExt.h"
#include "UAL_NetworkManager.h"

#include "LevelEditor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Logging/LogMacros.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/ConfigCacheIni.h"
#include "GameFramework/Actor.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"

// 缩略图相关
#include "ObjectTools.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"

#define LOCTEXT_NAMESPACE "FUAL_LevelViewportExt"

DEFINE_LOG_CATEGORY_STATIC(LogUALViewport, Log, All);

namespace UALViewportUtils
{
	/**
	 * 简单的中英文本地化切换
	 */
	FText LText(const FString& Key, const FString& ZhText, const FString& EnText)
	{
		const FString CultureName = FInternationalization::Get().GetCurrentCulture()->GetName();
		const bool bIsZh = CultureName.StartsWith(TEXT("zh"));
		return FText::FromString(bIsZh ? ZhText : EnText);
	}

	FString LStr(const FString& ZhText, const FString& EnText)
	{
		const FString CultureName = FInternationalization::Get().GetCurrentCulture()->GetName();
		const bool bIsZh = CultureName.StartsWith(TEXT("zh"));
		return bIsZh ? ZhText : EnText;
	}

	/**
	 * 兼容 UE 5.0 和 5.1+ 的 GetAssetByObjectPath
	 */
	FAssetData GetAssetDataByObject(IAssetRegistry& AssetRegistry, UObject* Object)
	{
		if (!Object)
		{
			return FAssetData();
		}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		return AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Object));
#else
		// UE 5.0 使用 FName 版本
		FString ObjectPath = Object->GetPathName();
		return AssetRegistry.GetAssetByObjectPath(FName(*ObjectPath));
#endif
	}

	/**
	 * 获取资产缩略图并保存到临时文件
	 * @param AssetData 资产数据
	 * @param ThumbnailSize 缩略图大小（默认 512）
	 * @return PNG 文件的完整路径，失败返回空字符串
	 */
	FString SaveAssetThumbnailToFile(const FAssetData& AssetData, int32 ThumbnailSize = 512)
	{
		// 过滤掉蓝图，因为它们通常没有可视内容导致缩略图全黑
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		if (AssetData.AssetClassPath.GetAssetName() == FName("Blueprint"))
		{
			return FString();
		}
#else
		if (AssetData.AssetClass == FName("Blueprint"))
		{
			return FString();
		}
#endif

		FName PackageName = AssetData.PackageName;
		FString PackageString = PackageName.ToString();
		
		// 首先尝试获取已缓存的缩略图
		const FObjectThumbnail* ExistingThumbnail = ThumbnailTools::FindCachedThumbnail(PackageString);
		
		FObjectThumbnail RenderedThumbnail;
		const FObjectThumbnail* ThumbnailToUse = ExistingThumbnail;
		
		if (ExistingThumbnail)
		{
			if (ExistingThumbnail->GetImageWidth() < ThumbnailSize || ExistingThumbnail->GetImageHeight() < ThumbnailSize)
			{
				ThumbnailToUse = nullptr;
			}
		}
		
		if (!ThumbnailToUse || ThumbnailToUse->GetImageWidth() == 0)
		{
			UObject* Asset = AssetData.GetAsset();
			if (!Asset)
			{
				return FString();
			}
			
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
			return FString();
		}
		
		const int32 ImageWidth = ThumbnailToUse->GetImageWidth();
		const int32 ImageHeight = ThumbnailToUse->GetImageHeight();
		
		TArray64<uint8> PngData;
		TArray64<uint8> RawData;
		int32 Width = ImageWidth;
		int32 Height = ImageHeight;
		
		const TArray<uint8>& SourceData = ThumbnailToUse->AccessImageData();
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
		EImageFormat DetectedFormat = ImageWrapperModule.DetectImageFormat(SourceData.GetData(), SourceData.Num());
		
		bool bGotRawData = false;
		
		if (DetectedFormat != EImageFormat::Invalid)
		{
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
			if (SourceData.Num() == ImageWidth * ImageHeight * 4)
			{
				RawData.SetNumUninitialized(SourceData.Num());
				FMemory::Memcpy(RawData.GetData(), SourceData.GetData(), SourceData.Num());
				bGotRawData = true;
			}
		}

		if (bGotRawData && RawData.Num() > 0)
		{
			// 强制设置 Alpha 通道为 255
			for (int32 i = 3; i < RawData.Num(); i += 4)
			{
				RawData[i] = 255;
			}
			
			TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
			if (PngWrapper.IsValid() && PngWrapper->SetRaw(RawData.GetData(), RawData.Num(), Width, Height, ERGBFormat::BGRA, 8))
			{
				PngData = PngWrapper->GetCompressed();
			}
		}

		if (PngData.Num() > 0)
		{
			FString TempDir = FPaths::ProjectSavedDir() / TEXT("UALinkThumbnails");
			IFileManager::Get().MakeDirectory(*TempDir, true);
			
			FString SafeName = AssetData.AssetName.ToString();
			SafeName = SafeName.Replace(TEXT(" "), TEXT("_"));
			FString FilePath = TempDir / FString::Printf(TEXT("%s_%lld.png"), *SafeName, FDateTime::Now().GetTicks());
			
			if (FFileHelper::SaveArrayToFile(PngData, *FilePath))
			{
				UE_LOG(LogUALViewport, Log, TEXT("缩略图已保存: %s"), *FilePath);
				return FilePath;
			}
		}
		
		return FString();
	}
}

void FUAL_LevelViewportExt::Register()
{
	if (bRegistered)
	{
		return;
	}

	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	
	// 注册Actor上下文菜单扩展
	TArray<FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors>& Extenders = 
		LevelEditorModule.GetAllLevelViewportContextMenuExtenders();
	
	Extenders.Add(FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors::CreateRaw(
		this, &FUAL_LevelViewportExt::OnExtendActorContextMenu));
	
	ExtenderHandle = Extenders.Last().GetHandle();
	bRegistered = true;

	UE_LOG(LogUALViewport, Log, TEXT("视口Actor右键菜单扩展已注册"));
}

void FUAL_LevelViewportExt::Unregister()
{
	if (!bRegistered)
	{
		return;
	}

	if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		TArray<FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors>& Extenders = 
			LevelEditorModule.GetAllLevelViewportContextMenuExtenders();
		
		Extenders.RemoveAll([Handle = ExtenderHandle](const FLevelEditorModule::FLevelViewportMenuExtender_SelectedActors& Delegate)
		{
			return Delegate.GetHandle() == Handle;
		});
	}

	bRegistered = false;
	UE_LOG(LogUALViewport, Log, TEXT("视口Actor右键菜单扩展已取消注册"));
}

TSharedRef<FExtender> FUAL_LevelViewportExt::OnExtendActorContextMenu(
	const TSharedRef<FUICommandList> CommandList,
	const TArray<AActor*> SelectedActors)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	if (SelectedActors.Num() > 0)
	{
		// 在 ActorPreview 之前添加菜单项
		Extender->AddMenuExtension(
			"ActorPreview",
			EExtensionHook::Before,
			nullptr,
			FMenuExtensionDelegate::CreateRaw(this, &FUAL_LevelViewportExt::AddMenuEntry, SelectedActors));
	}

	return Extender;
}

void FUAL_LevelViewportExt::AddMenuEntry(FMenuBuilder& MenuBuilder, TArray<AActor*> SelectedActors)
{
	MenuBuilder.BeginSection(NAME_None, UALViewportUtils::LText(TEXT("UALSectionActor"), TEXT("虚幻助手"), TEXT("Unreal Agent")));
	{
		MenuBuilder.AddMenuEntry(
			UALViewportUtils::LText(TEXT("UALImportActorAssets"), TEXT("导入到虚幻助手资产库"), TEXT("Import into Unreal Agent Asset Library")),
			UALViewportUtils::LText(TEXT("UALImportActorAssetsTooltip"), TEXT("将选中Actor引用的资产导入到虚幻助手中（虚幻助手需要处于打开状态）"), TEXT("Import assets referenced by selected actors into Unreal Agent (Unreal Agent must be running)")),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Import"),
			FUIAction(FExecuteAction::CreateLambda([this, SelectedActors]()
			{
				HandleImportActorAssets(SelectedActors);
			}))
		);
	}
	MenuBuilder.EndSection();
}

void FUAL_LevelViewportExt::HandleImportActorAssets(const TArray<AActor*>& SelectedActors)
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	// 第一步：收集用户选中的 Actor 引用的主资产
	TSet<FName> UserSelectedPackages;  // 用户选中的主资产包名
	TSet<FName> ProcessedPackages;     // 已处理的包
	TArray<FName> PackageQueue;        // BFS 队列

	for (AActor* Actor : SelectedActors)
	{
		if (!Actor) continue;

		// 获取 StaticMesh
		if (AStaticMeshActor* StaticMeshActor = Cast<AStaticMeshActor>(Actor))
		{
			if (UStaticMeshComponent* MeshComp = StaticMeshActor->GetStaticMeshComponent())
			{
				if (UStaticMesh* Mesh = MeshComp->GetStaticMesh())
				{
					FAssetData AssetData = UALViewportUtils::GetAssetDataByObject(AssetRegistry, Mesh);
					if (AssetData.IsValid() && !ProcessedPackages.Contains(AssetData.PackageName))
					{
						PackageQueue.Add(AssetData.PackageName);
						ProcessedPackages.Add(AssetData.PackageName);
						UserSelectedPackages.Add(AssetData.PackageName);
					}
				}
			}
		}
	}

	// 第二步：BFS 遍历依赖图（与内容浏览器逻辑一致）
	int32 QueueIndex = 0;
	while (QueueIndex < PackageQueue.Num())
	{
		FName CurrentPackage = PackageQueue[QueueIndex++];
		TArray<FName> Dependencies;
		AssetRegistry.GetDependencies(CurrentPackage, Dependencies);

		for (const FName& DepPackage : Dependencies)
		{
			FString DepPath = DepPackage.ToString();
			// 🚀关键修正：只处理 /Game/ 路径（项目内资产），过滤掉 /Engine/ 等内容
			// 这样就能和【内容浏览器】的导入逻辑保持一致，数量也会从 33 变成 19
			if (DepPath.StartsWith(TEXT("/Game/")) && !ProcessedPackages.Contains(DepPackage))
			{
				PackageQueue.Add(DepPackage);
				ProcessedPackages.Add(DepPackage);
			}
		}
	}

	if (PackageQueue.Num() == 0)
	{
		UE_LOG(LogUALViewport, Warning, TEXT("选中的Actor没有可导入的资产"));
		return;
	}

	UE_LOG(LogUALViewport, Log, TEXT("📦 依赖闭包收集完成: 主资产 %d 个, 总共 %d 个(含依赖)"),
		UserSelectedPackages.Num(), PackageQueue.Num());

	// 构建输出数组
	TArray<TSharedPtr<FJsonValue>> AssetPathsArray;
	TArray<TSharedPtr<FJsonValue>> AssetRealPathsArray;
	TArray<TSharedPtr<FJsonValue>> AssetMetadataArray;

	// 辅助 lambda：构建单个资产的元数据
	auto BuildAssetMetadata = [&](const FAssetData& AssetData, bool bIsSelected) -> TSharedPtr<FJsonObject>
	{
		const FString PackagePath = AssetData.PackageName.ToString();

		TSharedPtr<FJsonObject> MetadataObj = MakeShared<FJsonObject>();
		MetadataObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		MetadataObj->SetStringField(TEXT("package"), PackagePath);

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		MetadataObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.GetAssetName().ToString());
#else
		MetadataObj->SetStringField(TEXT("class"), AssetData.AssetClass.ToString());
#endif

		// 🚀 修复：正确获取并填充依赖关系
		TArray<FName> DirectDeps;
		AssetRegistry.GetDependencies(AssetData.PackageName, DirectDeps);

		TArray<TSharedPtr<FJsonValue>> DepsArray;
		for (const FName& DepName : DirectDeps)
		{
			FString DepPath = DepName.ToString();
			// 只记录项目内的引用 (/Game/)，与收集逻辑保持一致
			if (DepPath.StartsWith(TEXT("/Game/")))
			{
				DepsArray.Add(MakeShared<FJsonValueString>(DepPath));
			}
		}
		MetadataObj->SetArrayField(TEXT("dependencies"), DepsArray);

		// 标记是否为用户选中的资产（主资产）
		MetadataObj->SetBoolField(TEXT("is_selected"), bIsSelected);

		// 获取资产缩略图（保存到临时文件，返回路径）
		FString ThumbnailPath = UALViewportUtils::SaveAssetThumbnailToFile(AssetData, 512);
		if (!ThumbnailPath.IsEmpty())
		{
			MetadataObj->SetStringField(TEXT("thumbnail_path"), ThumbnailPath);
		}

		return MetadataObj;
	};

	// 辅助 lambda：添加资产到输出数组
	auto AddAssetToArrays = [&](const FAssetData& AssetData, bool bIsSelected)
	{
		const FString PackagePath = AssetData.PackageName.ToString();
		AssetPathsArray.Add(MakeShared<FJsonValueString>(PackagePath));

		// 获取文件路径
		FString Filename;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, FPackageName::GetAssetPackageExtension()))
		{
			const FString FullPath = FPaths::ConvertRelativePathToFull(Filename);
			AssetRealPathsArray.Add(MakeShared<FJsonValueString>(FullPath));
		}

		// 构建并添加元数据
		TSharedPtr<FJsonObject> MetadataObj = BuildAssetMetadata(AssetData, bIsSelected);
		AssetMetadataArray.Add(MakeShared<FJsonValueObject>(MetadataObj));
	};

	// 遍历所有收集到的包
	for (const FName& PackageName : PackageQueue)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(PackageName, Assets);

		if (Assets.Num() > 0)
		{
			// 一个包通常只有一个主资产，取第一个即可
			const FAssetData& AssetData = Assets[0];
			bool bIsSelected = UserSelectedPackages.Contains(PackageName);
			AddAssetToArrays(AssetData, bIsSelected);
		}
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
	UE_LOG(LogUALViewport, Log, TEXT("%s: 已发送 %d 个资产的导入请求"),
		*UALViewportUtils::LStr(TEXT("已发送资产导入请求"), TEXT("Import assets request sent")),
		AssetPathsArray.Num());
}

void FUAL_LevelViewportExt::AddProjectMeta(TSharedPtr<FJsonObject>& Payload) const
{
	const FString ProjectName = FApp::GetProjectName();
	FString ProjectVersion(TEXT("unspecified"));

	// 从项目设置中读取版本号
	if (GConfig)
	{
		FString IniVersion;
		if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), IniVersion, GGameIni) && !IniVersion.IsEmpty())
		{
			ProjectVersion = IniVersion;
		}
	}

	const FString EngineVersion = FEngineVersion::Current().ToString();

	Payload->SetStringField(TEXT("project_name"), ProjectName);
	Payload->SetStringField(TEXT("project_version"), ProjectVersion);
	Payload->SetStringField(TEXT("engine_version"), EngineVersion);
}

#undef LOCTEXT_NAMESPACE
