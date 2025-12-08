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
#include "AssetData.h"
#include "Engine/World.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"

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

	// 构造发送的 JSON 报文
	TArray<TSharedPtr<FJsonValue>> PathsArray;
	TArray<TSharedPtr<FJsonValue>> RealPathsArray;
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
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("paths"), PathsArray);
	if (RealPathsArray.Num() > 0)
	{
		Payload->SetArrayField(TEXT("real_paths"), RealPathsArray);
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
	UE_LOG(LogUALContentBrowser, Verbose, TEXT("%s: %s"),
		*LocalizedString(TEXT("已发送导入请求 JSON"), TEXT("Import folder request sent JSON")),
		*OutJson);
}

void FUAL_ContentBrowserExt::HandleImportAssets(const TArray<FAssetData>& SelectedAssets)
{
	TArray<TSharedPtr<FJsonValue>> AssetPathsArray;
	TArray<TSharedPtr<FJsonValue>> AssetRealPathsArray;

	for (const FAssetData& AssetData : SelectedAssets)
	{
		const FString PackagePath = AssetData.PackageName.ToString();
		AssetPathsArray.Add(MakeShared<FJsonValueString>(PackagePath));

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
			UE_LOG(LogUALContentBrowser, Log, TEXT("%s %s -> %s"),
				*LocalizedString(TEXT("导入资产"), TEXT("Import asset")),
				*PackagePath, *FullPath);
		}
		else
		{
			UE_LOG(LogUALContentBrowser, Warning, TEXT("%s: %s"),
				*LocalizedString(TEXT("无法转换资产包路径为文件路径"), TEXT("Failed to convert asset package path to file path")),
				*PackagePath);
		}
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("asset_paths"), AssetPathsArray);
	if (AssetRealPathsArray.Num() > 0)
	{
		Payload->SetArrayField(TEXT("asset_real_paths"), AssetRealPathsArray);
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
	UE_LOG(LogUALContentBrowser, Verbose, TEXT("%s: %s"),
		*LocalizedString(TEXT("已发送资产导入请求 JSON"), TEXT("Import assets request sent JSON")),
		*OutJson);
}

void FUAL_ContentBrowserExt::AddProjectMeta(TSharedPtr<FJsonObject>& Payload) const
{
	const FString ProjectName = FApp::GetProjectName();
	FString ProjectVersion(TEXT("unspecified"));
	bool bHasProjectVersion = false;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
	// UE5.1+ 提供版本号获取
	const FString RetrievedVersion = FApp::GetProjectVersion();
	if (!RetrievedVersion.IsEmpty())
	{
		ProjectVersion = RetrievedVersion;
		bHasProjectVersion = true;
	}
#else
	// UE5.0 无 GetProjectVersion，保持默认值
#endif
	if (!bHasProjectVersion && GConfig)
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

