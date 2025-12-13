#include "UAL_EditorCommands.h"
#include "UAL_CommandUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HighResScreenshot.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Misc/ConfigCacheIni.h"
#include "Interfaces/IPluginManager.h"
#include "UAL_VersionCompat.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALEditor, Log, All);

void FUAL_EditorCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("editor.screenshot"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_TakeScreenshot(Payload, RequestId);
	});

	CommandMap.Add(TEXT("take_screenshot"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_TakeScreenshot(Payload, RequestId);
	});

	CommandMap.Add(TEXT("project.info"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetProjectInfo(Payload, RequestId);
	});

	// 兼容别名：早期/上层工具可能使用 editor.get_project_info
	CommandMap.Add(TEXT("editor.get_project_info"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetProjectInfo(Payload, RequestId);
	});
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========
// 原始行号参考:
//   Handle_TakeScreenshot: 1984-2142
//   Handle_GetProjectInfo: 2144-2148
//   BuildProjectInfo:      2150-2278

void FUAL_EditorCommands::Handle_TakeScreenshot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 1) 选择视口
	FViewport* Viewport = nullptr;
#if WITH_EDITOR
	if (GEditor)
	{
		Viewport = GEditor->GetActiveViewport();
	}
#endif
	if (!Viewport && GEngine && GEngine->GameViewport)
	{
		Viewport = GEngine->GameViewport->Viewport;
	}

	if (!Viewport)
	{
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("No active viewport to capture"));
		return;
	}

	// 参数
	bool bShowUI = false;
	Payload->TryGetBoolField(TEXT("show_ui"), bShowUI);

	FIntPoint RequestedSize(0, 0);
	const TArray<TSharedPtr<FJsonValue>>* Resolution = nullptr;
	if (Payload->TryGetArrayField(TEXT("resolution"), Resolution) && Resolution && Resolution->Num() == 2)
	{
		const int32 Width = (int32)(*Resolution)[0]->AsNumber();
		const int32 Height = (int32)(*Resolution)[1]->AsNumber();
		if (Width > 0 && Height > 0)
		{
			RequestedSize = FIntPoint(Width, Height);
		}
	}

	// 2) 读取像素
	// 强制刷新编辑器视口，确保画面是最新的
	if (GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}
	Viewport->Draw(true); // true = bShouldPresent
	FlushRenderingCommands(); // 确保渲染命令执行完毕

	const FIntPoint SourceSize = Viewport->GetSizeXY();

	TArray<FColor> Bitmap;
	if (SourceSize.X <= 0 || SourceSize.Y <= 0)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Invalid viewport size"));
		return;
	}

	// 使用 ReadPixels 并处理 Alpha 通道
	if (!Viewport->ReadPixels(Bitmap, FReadSurfaceDataFlags(), FIntRect()))
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to read viewport pixels"));
		return;
	}

	// 检查并填充 Alpha，防止全透明导致黑屏（特别是编辑器视口下）
	bool bIsTransparent = true;
	for (const FColor& C : Bitmap)
	{
		if (C.A != 0)
		{
			bIsTransparent = false;
			break;
		}
	}
	if (bIsTransparent)
	{
		for (FColor& C : Bitmap)
		{
			C.A = 255;
		}
	}

	FIntPoint TargetSize = SourceSize;
	if (RequestedSize.X > 0 && RequestedSize.Y > 0)
	{
		TargetSize = RequestedSize;
	}

	// 可选缩放（最近邻，避免额外依赖）
	if (TargetSize != SourceSize)
	{
		TArray<FColor> Scaled;
		Scaled.SetNum(TargetSize.X * TargetSize.Y);

		for (int32 Y = 0; Y < TargetSize.Y; ++Y)
		{
			const int32 SrcY = FMath::Clamp(FMath::FloorToInt((float)Y * SourceSize.Y / TargetSize.Y), 0, SourceSize.Y - 1);
			for (int32 X = 0; X < TargetSize.X; ++X)
			{
				const int32 SrcX = FMath::Clamp(FMath::FloorToInt((float)X * SourceSize.X / TargetSize.X), 0, SourceSize.X - 1);
				Scaled[Y * TargetSize.X + X] = Bitmap[SrcY * SourceSize.X + SrcX];
			}
		}

		Bitmap = MoveTemp(Scaled);
	}

	// 3) PNG 编码
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!PngWrapper.IsValid() || !PngWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), TargetSize.X, TargetSize.Y, ERGBFormat::BGRA, 8))
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to encode screenshot"));
		return;
	}

	TArray<uint8> PngData;
	if (!UALCompat::GetCompressedPNG(PngWrapper, 100, PngData))
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to encode screenshot"));
		return;
	}

	// 4) 路径与文件名
	FString DesiredName;
	Payload->TryGetStringField(TEXT("filepath"), DesiredName);
	FString CleanName = FPaths::GetCleanFilename(DesiredName);
	if (CleanName.IsEmpty())
	{
		CleanName = FDateTime::Now().ToString(TEXT("UAL_Shot_%Y%m%d_%H%M%S"));
	}
	if (!CleanName.EndsWith(TEXT(".png")))
	{
		CleanName += TEXT(".png");
	}

	const FString OutputDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots/UAL")));
	IFileManager::Get().MakeDirectory(*OutputDir, true);
	const FString OutputPath = FPaths::Combine(OutputDir, CleanName);

	const bool bSaved = FFileHelper::SaveArrayToFile(PngData, *OutputPath);

	// 5) Base64 返回 (移除，避免大包传输超时)
	// const FString Base64Data = FBase64::Encode(PngData);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), OutputPath);
	Data->SetStringField(TEXT("filename"), CleanName);
	Data->SetNumberField(TEXT("width"), TargetSize.X);
	Data->SetNumberField(TEXT("height"), TargetSize.Y);
	Data->SetBoolField(TEXT("saved"), bSaved);
	Data->SetBoolField(TEXT("show_ui"), bShowUI);
	// Data->SetStringField(TEXT("base64"), Base64Data);

	if (!bSaved)
	{
		Data->SetStringField(TEXT("save_error"), TEXT("Failed to write screenshot to disk"));
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_EditorCommands::Handle_GetProjectInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// TODO: 从 UAL_CommandHandler.cpp 第 2144-2148 行迁移
	TSharedPtr<FJsonObject> Data = BuildProjectInfo();
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

TSharedPtr<FJsonObject> FUAL_EditorCommands::BuildProjectInfo()
{
	// 基础路径信息
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	Data->SetStringField(TEXT("projectPath"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Data->SetStringField(TEXT("projectFile"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	Data->SetStringField(TEXT("contentDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
	Data->SetStringField(TEXT("configDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()));
	Data->SetStringField(TEXT("savedDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
	Data->SetStringField(TEXT("pluginsDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir()));

	// 版本信息（兼容 5.0-5.7，直接读配置）
	FString ProjectVersion;
	GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), ProjectVersion, GGameIni);
	if (!ProjectVersion.IsEmpty())
	{
		Data->SetStringField(TEXT("projectVersion"), ProjectVersion);
	}

	Data->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());

	// GameMaps 设置
	FString GameDefaultMap;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameDefaultMap"), GameDefaultMap, GGameIni))
	{
		Data->SetStringField(TEXT("defaultMap"), GameDefaultMap);
	}
	FString EditorStartupMap;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("EditorStartupMap"), EditorStartupMap, GGameIni))
	{
		Data->SetStringField(TEXT("editorStartupMap"), EditorStartupMap);
	}

	// GeneralProjectSettings
	FString CompanyName;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("CompanyName"), CompanyName, GGameIni))
	{
		Data->SetStringField(TEXT("companyName"), CompanyName);
	}
	FString ProjectId;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectID"), ProjectId, GGameIni))
	{
		Data->SetStringField(TEXT("projectId"), ProjectId);
	}
	FString SupportContact;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("SupportContact"), SupportContact, GGameIni))
	{
		Data->SetStringField(TEXT("supportContact"), SupportContact);
	}

	// 读取 .uproject 以补充 EngineAssociation、TargetPlatforms、Modules
	FString ProjectFileContent;
	if (FFileHelper::LoadFileToString(ProjectFileContent, *FPaths::GetProjectFilePath()))
	{
		TSharedPtr<FJsonObject> ProjectJson;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ProjectFileContent);
		if (FJsonSerializer::Deserialize(Reader, ProjectJson) && ProjectJson.IsValid())
		{
			FString EngineAssociation;
			if (ProjectJson->TryGetStringField(TEXT("EngineAssociation"), EngineAssociation))
			{
				Data->SetStringField(TEXT("engineAssociation"), EngineAssociation);
			}

			const TArray<TSharedPtr<FJsonValue>>* TargetPlatforms = nullptr;
			if (ProjectJson->TryGetArrayField(TEXT("TargetPlatforms"), TargetPlatforms))
			{
				TArray<TSharedPtr<FJsonValue>> PlatformArray;
				for (const TSharedPtr<FJsonValue>& Value : *TargetPlatforms)
				{
					if (Value.IsValid() && Value->Type == EJson::String)
					{
						PlatformArray.Add(MakeShared<FJsonValueString>(Value->AsString()));
					}
				}
				if (PlatformArray.Num() > 0)
				{
					Data->SetArrayField(TEXT("targetPlatforms"), PlatformArray);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
			if (ProjectJson->TryGetArrayField(TEXT("Modules"), Modules))
			{
				TArray<TSharedPtr<FJsonValue>> ModuleArray;
				for (const TSharedPtr<FJsonValue>& Value : *Modules)
				{
					if (Value.IsValid() && Value->Type == EJson::Object)
					{
						const TSharedPtr<FJsonObject> ModuleObj = Value->AsObject();
						if (ModuleObj.IsValid())
						{
							FString ModuleName;
							if (ModuleObj->TryGetStringField(TEXT("Name"), ModuleName))
							{
								ModuleArray.Add(MakeShared<FJsonValueString>(ModuleName));
							}
						}
					}
				}
				if (ModuleArray.Num() > 0)
				{
					Data->SetArrayField(TEXT("modules"), ModuleArray);
				}
			}

			// 插件列表：仅发送 .uproject 文件中声明的 Plugins（避免把 Engine/Plugins 也全部发出去）
			// - projectPlugins: 完整列表（包含 enabled 标记）
			// - enabledPlugins: 仅 enabled=true 的条目（为兼容旧字段名）
			// - enabledPluginNames: 仅名称列表（便于前端/服务端快速使用）
			const TArray<TSharedPtr<FJsonValue>>* ProjectPlugins = nullptr;
			if (ProjectJson->TryGetArrayField(TEXT("Plugins"), ProjectPlugins) && ProjectPlugins)
			{
				IPluginManager& PluginManager = IPluginManager::Get();

				TArray<TSharedPtr<FJsonValue>> ProjectPluginArray;
				TArray<TSharedPtr<FJsonValue>> EnabledPluginArray;
				TArray<TSharedPtr<FJsonValue>> EnabledPluginNames;

				for (const TSharedPtr<FJsonValue>& Value : *ProjectPlugins)
				{
					if (!Value.IsValid() || Value->Type != EJson::Object)
					{
						continue;
					}

					const TSharedPtr<FJsonObject> PluginDecl = Value->AsObject();
					if (!PluginDecl.IsValid())
					{
						continue;
					}

					FString PluginName;
					// .uproject 标准字段为 Name/Enabled；这里也兼容小写 name/enabled
					if (!PluginDecl->TryGetStringField(TEXT("Name"), PluginName))
					{
						PluginDecl->TryGetStringField(TEXT("name"), PluginName);
					}
					if (PluginName.IsEmpty())
					{
						continue;
					}

					bool bEnabled = true;
					if (!PluginDecl->TryGetBoolField(TEXT("Enabled"), bEnabled))
					{
						PluginDecl->TryGetBoolField(TEXT("enabled"), bEnabled);
					}

					TSharedPtr<FJsonObject> PluginObj = MakeShared<FJsonObject>();
					PluginObj->SetStringField(TEXT("name"), PluginName);
					PluginObj->SetBoolField(TEXT("enabled"), bEnabled);

					// 尝试补全插件元信息（如果插件在当前环境可解析）
					{
						const TSharedPtr<IPlugin> Plugin = PluginManager.FindPlugin(PluginName);
						if (Plugin.IsValid())
						{
							PluginObj->SetStringField(TEXT("versionName"), Plugin->GetDescriptor().VersionName);
							PluginObj->SetStringField(TEXT("category"), Plugin->GetDescriptor().Category);
							PluginObj->SetStringField(TEXT("baseDir"), Plugin->GetBaseDir());
						}
						else
						{
							// 保持字段存在，避免消费端做大量判空
							PluginObj->SetStringField(TEXT("versionName"), TEXT(""));
							PluginObj->SetStringField(TEXT("category"), TEXT(""));
							PluginObj->SetStringField(TEXT("baseDir"), TEXT(""));
						}
					}

					ProjectPluginArray.Add(MakeShared<FJsonValueObject>(PluginObj));
					if (bEnabled)
					{
						EnabledPluginArray.Add(MakeShared<FJsonValueObject>(PluginObj));
						EnabledPluginNames.Add(MakeShared<FJsonValueString>(PluginName));
					}
				}

				if (ProjectPluginArray.Num() > 0)
				{
					Data->SetArrayField(TEXT("projectPlugins"), ProjectPluginArray);
				}
				if (EnabledPluginArray.Num() > 0)
				{
					Data->SetArrayField(TEXT("enabledPlugins"), EnabledPluginArray);
				}
				if (EnabledPluginNames.Num() > 0)
				{
					Data->SetArrayField(TEXT("enabledPluginNames"), EnabledPluginNames);
				}
			}
		}
	}

	return Data;
}
