#include "UAL_EditorCommands.h"
#include "UAL_CommandUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "LevelEditorViewport.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Slate/SceneViewport.h"
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
#include "GameMapsSettings.h"
#include "UAL_VersionCompat.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALEditor, Log, All);

/**
 * 截图任务上下文结构体（用于异步定时器回调）
 */
struct FScreenshotTaskContext
{
	FString RequestId;
	FString ScreenshotDir;
	TMap<FString, FDateTime> FilesBeforeMap;
	FDateTime CommandExecuteTime;
	int32 Width;
	int32 Height;
	int32 RetryCount;
	int32 MaxRetries;
	FTimerHandle TimerHandle;
};

// 静态任务列表（存储正在进行的截图任务）
static TMap<FString, TSharedPtr<FScreenshotTaskContext>> PendingScreenshotTasks;

/**
 * 定时器回调：检查截图文件是否生成
 */
static void CheckScreenshotFile(FString TaskId)
{
	TSharedPtr<FScreenshotTaskContext>* ContextPtr = PendingScreenshotTasks.Find(TaskId);
	if (!ContextPtr || !ContextPtr->IsValid())
	{
		UE_LOG(LogUALEditor, Warning, TEXT("Screenshot task %s not found"), *TaskId);
		return;
	}
	
	TSharedPtr<FScreenshotTaskContext> Context = *ContextPtr;
	Context->RetryCount++;
	
	UE_LOG(LogUALEditor, Log, TEXT("Checking screenshot... retry %d/%d"), Context->RetryCount, Context->MaxRetries);
	
	// 查找新生成的截图文件
	TArray<FString> FilesAfter;
	IFileManager::Get().FindFiles(FilesAfter, *FPaths::Combine(Context->ScreenshotDir, TEXT("HighresScreenshot*.png")), true, false);
	
	UE_LOG(LogUALEditor, Log, TEXT("Found %d files in dir, BeforeMap has %d entries, CommandTime: %s"), 
		FilesAfter.Num(), Context->FilesBeforeMap.Num(), *Context->CommandExecuteTime.ToString());
	
	FString NewScreenshotPath;
	
	// 策略 1: 优先查找新增的文件名
	for (const FString& File : FilesAfter)
	{
		if (!Context->FilesBeforeMap.Contains(File))
		{
			FString FullPath = FPaths::Combine(Context->ScreenshotDir, File);
			FDateTime FileTime = IFileManager::Get().GetTimeStamp(*FullPath);
			int64 FileSize = IFileManager::Get().FileSize(*FullPath);
			
			UE_LOG(LogUALEditor, Log, TEXT("New file candidate: %s, FileTime: %s, Size: %lld, CommandTime: %s"), 
				*File, *FileTime.ToString(), FileSize, *Context->CommandExecuteTime.ToString());
			
			if (FileTime >= Context->CommandExecuteTime && FileSize > 0)
			{
				NewScreenshotPath = FullPath;
				UE_LOG(LogUALEditor, Log, TEXT("Found NEW file: %s (size: %lld)"), *File, FileSize);
				break;
			}
			else
			{
				UE_LOG(LogUALEditor, Warning, TEXT("Skipping file %s: FileTime < CommandTime or Size <= 0"), *File);
			}
		}
	}
	
	// 策略 2: 查找时间戳被更新的文件
	if (NewScreenshotPath.IsEmpty())
	{
		for (const FString& File : FilesAfter)
		{
			FString FullPath = FPaths::Combine(Context->ScreenshotDir, File);
			FDateTime CurrentTime = IFileManager::Get().GetTimeStamp(*FullPath);
			FDateTime* PreviousTime = Context->FilesBeforeMap.Find(File);
			
			if (PreviousTime && CurrentTime > *PreviousTime && CurrentTime >= Context->CommandExecuteTime)
			{
				int64 FileSize = IFileManager::Get().FileSize(*FullPath);
				if (FileSize > 0)
				{
					NewScreenshotPath = FullPath;
					UE_LOG(LogUALEditor, Log, TEXT("Found UPDATED file: %s (size: %lld)"), *File, FileSize);
					break;
				}
			}
		}
	}
	
	// 找到文件 - 发送成功响应
	if (!NewScreenshotPath.IsEmpty())
	{
		UE_LOG(LogUALEditor, Log, TEXT("Screenshot captured: %s"), *NewScreenshotPath);
		
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("path"), NewScreenshotPath);
		Data->SetStringField(TEXT("filename"), FPaths::GetCleanFilename(NewScreenshotPath));
		Data->SetNumberField(TEXT("width"), Context->Width);
		Data->SetNumberField(TEXT("height"), Context->Height);
		Data->SetBoolField(TEXT("saved"), true);
		Data->SetBoolField(TEXT("restore_app_window"), true); // 通知客户端恢复应用窗口
		
		UAL_CommandUtils::SendResponse(Context->RequestId, 200, Data);
		
		// 清理定时器和任务
		if (GEditor)
		{
			GEditor->GetTimerManager()->ClearTimer(Context->TimerHandle);
		}
		PendingScreenshotTasks.Remove(TaskId);
		return;
	}
	
	// 超时 - 发送错误响应
	if (Context->RetryCount >= Context->MaxRetries)
	{
		UE_LOG(LogUALEditor, Error, TEXT("Screenshot timeout after %d retries"), Context->MaxRetries);
		UAL_CommandUtils::SendError(Context->RequestId, 500, TEXT("截图超时：HighResShot 未生成截图文件。请确保已打开一个关卡/场景（Level）视口并置于前台，而非材质、蓝图等编辑器窗口。"));
		
		// 清理定时器和任务
		if (GEditor)
		{
			GEditor->GetTimerManager()->ClearTimer(Context->TimerHandle);
		}
		PendingScreenshotTasks.Remove(TaskId);
		return;
	}
	
	// 继续等待（定时器会自动触发下一次检查）
}
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

	CommandMap.Add(TEXT("project.get_config"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetConfig(Payload, RequestId);
	});

	CommandMap.Add(TEXT("project.set_config"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetConfig(Payload, RequestId);
	});

	CommandMap.Add(TEXT("project.analyze_uproject"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AnalyzeUProject(Payload, RequestId);
	});

	CommandMap.Add(TEXT("editor.capture_app_window"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CaptureAppWindow(Payload, RequestId);
	});

	// editor.get_focus_context - 获取当前焦点编辑器上下文（蓝图/材质/关卡等）
	CommandMap.Add(TEXT("editor.get_focus_context"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetFocusContext(Payload, RequestId);
	});
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========
// 原始行号参考:
//   Handle_TakeScreenshot: 1984-2142
//   Handle_GetProjectInfo: 2144-2148
//   BuildProjectInfo:      2150-2278

void FUAL_EditorCommands::Handle_TakeScreenshot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 使用 HighResShot 控制台命令截图（UE 编辑器内置方式，最可靠）
#if WITH_EDITOR
	// 1) 解析分辨率参数
	int32 Width = 1920;
	int32 Height = 1080;
	const TArray<TSharedPtr<FJsonValue>>* Resolution = nullptr;
	if (Payload->TryGetArrayField(TEXT("resolution"), Resolution) && Resolution && Resolution->Num() == 2)
	{
		Width = FMath::Max((int32)(*Resolution)[0]->AsNumber(), 64);
		Height = FMath::Max((int32)(*Resolution)[1]->AsNumber(), 64);
	}
	
	// 2) 获取截图目录，找到最新文件用于对比
	const FString ScreenshotDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots/WindowsEditor")));
	IFileManager::Get().MakeDirectory(*ScreenshotDir, true);
	
	// 记录执行前目录中已有文件及其时间戳
	TMap<FString, FDateTime> FilesBeforeMap;
	TArray<FString> FilesBefore;
	IFileManager::Get().FindFiles(FilesBefore, *FPaths::Combine(ScreenshotDir, TEXT("HighresScreenshot*.png")), true, false);
	for (const FString& File : FilesBefore)
	{
		FString FullPath = FPaths::Combine(ScreenshotDir, File);
		FilesBeforeMap.Add(File, IFileManager::Get().GetTimeStamp(*FullPath));
	}
	
	// 3) 记录命令执行时间点（使用 UTC 时间与文件系统时间戳一致，减去 2 秒容差避免精度问题）
	FDateTime CommandExecuteTime = FDateTime::UtcNow() - FTimespan::FromSeconds(2);
	UE_LOG(LogUALEditor, Log, TEXT("Command execute time (with tolerance): %s"), *CommandExecuteTime.ToString());
	
	// 3.5) 将 UE 编辑器窗口恢复并置顶（确保截图时窗口在前台）
	if (FSlateApplication::IsInitialized())
	{
		TSharedPtr<SWindow> MainWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (!MainWindow.IsValid())
		{
			// 如果没有活动窗口，尝试获取第一个顶层窗口
			TArray<TSharedRef<SWindow>> Windows = FSlateApplication::Get().GetInteractiveTopLevelWindows();
			if (Windows.Num() > 0)
			{
				MainWindow = Windows[0];
			}
		}
		
		if (MainWindow.IsValid())
		{
			// 如果窗口被最小化，先恢复
			if (MainWindow->GetNativeWindow().IsValid() && MainWindow->GetNativeWindow()->IsMinimized())
			{
				MainWindow->Restore();
			}
			// 将窗口置顶
			MainWindow->BringToFront();
			if (MainWindow->GetNativeWindow().IsValid())
			{
				MainWindow->GetNativeWindow()->SetWindowFocus();
			}
			UE_LOG(LogUALEditor, Log, TEXT("Editor window brought to front for screenshot"));
		}
	}
	
	// 4) 执行 HighResShot 控制台命令
	FString Command = FString::Printf(TEXT("HighResShot %dx%d"), Width, Height);
	UE_LOG(LogUALEditor, Log, TEXT("Executing: %s, files before: %d"), *Command, FilesBefore.Num());
	GEngine->Exec(GEditor->GetWorld(), *Command);
	
	// 5) 创建异步任务上下文并设置定时器（非阻塞）
	TSharedPtr<FScreenshotTaskContext> Context = MakeShared<FScreenshotTaskContext>();
	Context->RequestId = RequestId;
	Context->ScreenshotDir = ScreenshotDir;
	Context->FilesBeforeMap = FilesBeforeMap;
	Context->CommandExecuteTime = CommandExecuteTime;
	Context->Width = Width;
	Context->Height = Height;
	Context->RetryCount = 0;
	Context->MaxRetries = 15;
	
	// 使用 RequestId 作为任务 ID
	FString TaskId = RequestId;
	PendingScreenshotTasks.Add(TaskId, Context);
	
	// 设置定时器：每 2 秒检查一次，最多检查 15 次（共 30 秒）
	if (GEditor)
	{
		GEditor->GetTimerManager()->SetTimer(
			Context->TimerHandle,
			FTimerDelegate::CreateLambda([TaskId]() { CheckScreenshotFile(TaskId); }),
			2.0f,  // 每 2 秒检查一次
			true,  // 循环
			2.0f   // 首次延迟 2 秒（给截图时间生成）
		);
		UE_LOG(LogUALEditor, Log, TEXT("Screenshot async timer started for request: %s"), *RequestId);
	}
	else
	{
		UE_LOG(LogUALEditor, Error, TEXT("Failed to get TimerManager"));
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to start screenshot timer"));
		PendingScreenshotTasks.Remove(TaskId);
	}
	
	// 立即返回，不阻塞（响应将由定时器回调发送）
#else
	// 非编辑器模式不支持 HighResShot
	UAL_CommandUtils::SendError(RequestId, 501, TEXT("HighResShot only available in editor mode"));
#endif
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

	/**
	 * ========== 读取 GameMapsSettings 开发心得 (2026-01) ==========
	 * 
	 * 【问题背景】
	 * 需要读取 Project Settings → Maps & Modes 中配置的 GameDefaultMap 等字段。
	 * 这些配置存储在 Config/DefaultGame.ini 的 [/Script/EngineSettings.GameMapsSettings] section。
	 * 
	 * 【坑点1：GConfig 方式无效】
	 * 尝试使用 GConfig->GetString(Section, Key, Value, GGameIni) 时返回空。
	 * 原因：GGameIni 指向的是运行时合并后的 Game.ini，而非项目的 DefaultGame.ini。
	 * 
	 * 【坑点2：LoadLocalIniFile 加载错误文件】
	 * FConfigCacheIni::LoadLocalIniFile(ConfigFile, "Game", ...) 加载的是合并后的 Game.ini，
	 * 不包含 /Script/EngineSettings.GameMapsSettings section。
	 * 
	 * 【坑点3：FConfigFile::Read() 只加载部分内容】
	 * 直接用 FConfigFile::Read(DefaultGame.ini) 只返回 1 个 section (GeneralProjectSettings)，
	 * 不包含 GameMapsSettings。原因不明，可能与 UE 配置解析机制有关。
	 * 
	 * 【坑点4：Section 路径易混淆】
	 * /Script/EngineSettings.GameMapsSettings  ← DefaultGame.ini 中的格式
	 * /Script/Engine.GameMapsSettings          ← 不正确
	 * 两者容易混淆，但都不是根本解决方案。
	 * 
	 * 【正确方案：使用 UGameMapsSettings API】
	 * 引擎内部使用静态方法 UGameMapsSettings::GetGameDefaultMap()。
	 * 需要：
	 *   1. 在 Build.cs 添加 "EngineSettings" 模块依赖
	 *   2. #include "GameMapsSettings.h"
	 *   3. 调用 UGameMapsSettings::GetGameDefaultMap() 或 GetDefault<UGameMapsSettings>()
	 * 
	 * 此方式兼容 UE 5.0 - 5.7，是最可靠的方案。
	 * ==========================================================
	 */
	
	// GameMaps 设置 - 使用 UGameMapsSettings API（引擎内部使用的方式）
	FString GameDefaultMap = UGameMapsSettings::GetGameDefaultMap();
	if (!GameDefaultMap.IsEmpty())
	{
		UE_LOG(LogUALEditor, Log, TEXT("GameDefaultMap = %s"), *GameDefaultMap);
		Data->SetStringField(TEXT("defaultMap"), GameDefaultMap);
	}
	else
	{
		UE_LOG(LogUALEditor, Log, TEXT("GameDefaultMap is not set"));
	}

	// EditorStartupMap - 通过 GetDefault 获取
	const UGameMapsSettings* GameMapsSettings = GetDefault<UGameMapsSettings>();
	if (GameMapsSettings)
	{
		FString EditorStartupMap = GameMapsSettings->EditorStartupMap.GetLongPackageName();
		if (!EditorStartupMap.IsEmpty())
		{
			UE_LOG(LogUALEditor, Log, TEXT("EditorStartupMap = %s"), *EditorStartupMap);
			Data->SetStringField(TEXT("editorStartupMap"), EditorStartupMap);
		}

		// TransitionMap
		FString TransitionMap = GameMapsSettings->TransitionMap.GetLongPackageName();
		if (!TransitionMap.IsEmpty())
		{
			Data->SetStringField(TEXT("transitionMap"), TransitionMap);
		}
	}


	// GeneralProjectSettings - 使用 GConfig 读取（兼容所有版本）
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

	// 扩展：当前打开的关卡名
#if WITH_EDITOR
	if (GEditor && GEditor->GetWorld())
	{
		FString CurrentLevelName = GEditor->GetWorld()->GetMapName();
		Data->SetStringField(TEXT("currentLevelName"), CurrentLevelName);
		
		// 当前关卡完整路径
		FString CurrentLevelPath = GEditor->GetWorld()->GetOutermost()->GetName();
		Data->SetStringField(TEXT("currentLevelPath"), CurrentLevelPath);
	}
#endif

	// 扩展：渲染设置（Nanite/Lumen）
	FString NaniteEnabled;
	if (GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.Nanite.ProjectEnabled"), NaniteEnabled, GEngineIni))
	{
		Data->SetBoolField(TEXT("naniteEnabled"), NaniteEnabled.Equals(TEXT("True"), ESearchCase::IgnoreCase) || NaniteEnabled == TEXT("1"));
	}
	FString LumenGI;
	if (GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.DynamicGlobalIlluminationMethod"), LumenGI, GEngineIni))
	{
		// 1 = Lumen, 0 = None/SSGI
		Data->SetBoolField(TEXT("lumenGIEnabled"), LumenGI == TEXT("1"));
		Data->SetStringField(TEXT("dynamicGIMethod"), LumenGI);
	}
	FString LumenReflections;
	if (GConfig->GetString(TEXT("/Script/Engine.RendererSettings"), TEXT("r.ReflectionMethod"), LumenReflections, GEngineIni))
	{
		// 1 = Lumen, 0 = None, 2 = SSR
		Data->SetBoolField(TEXT("lumenReflectionsEnabled"), LumenReflections == TEXT("1"));
		Data->SetStringField(TEXT("reflectionMethod"), LumenReflections);
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
// 临时文件，用于添加新函数
void FUAL_EditorCommands::Handle_GetConfig(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString ConfigName;
	if (!Payload->TryGetStringField(TEXT("config_name"), ConfigName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: config_name"));
		return;
	}

	FString Section;
	if (!Payload->TryGetStringField(TEXT("section"), Section))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: section"));
		return;
	}

	FString Key;
	if (!Payload->TryGetStringField(TEXT("key"), Key))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: key"));
		return;
	}

	// 映射配置文件�?
	FString ConfigFileName;
	if (ConfigName.Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEngineIni;
	}
	else if (ConfigName.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GGameIni;
	}
	else if (ConfigName.Equals(TEXT("Editor"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEditorIni;
	}
	else if (ConfigName.Equals(TEXT("EditorPerProjectUserSettings"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEditorPerProjectIni;
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(TEXT("Unsupported config_name: %s. Supported: Engine, Game, Editor, EditorPerProjectUserSettings"), *ConfigName));
		return;
	}

	// 读取配置
	FString Value;
	if (!GConfig->GetString(*Section, *Key, Value, ConfigFileName))
	{
		// 配置项不存在，返回空值（不是错误�?
		Value = TEXT("");
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("config_name"), ConfigName);
	Result->SetStringField(TEXT("section"), Section);
	Result->SetStringField(TEXT("key"), Key);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetStringField(TEXT("file_path"), ConfigFileName);

	UE_LOG(LogUALEditor, Log, TEXT("project.get_config: %s [%s] %s = %s"), *ConfigName, *Section, *Key, *Value);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_EditorCommands::Handle_SetConfig(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString ConfigName;
	if (!Payload->TryGetStringField(TEXT("config_name"), ConfigName))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: config_name"));
		return;
	}

	FString Section;
	if (!Payload->TryGetStringField(TEXT("section"), Section))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: section"));
		return;
	}

	FString Key;
	if (!Payload->TryGetStringField(TEXT("key"), Key))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: key"));
		return;
	}

	FString Value;
	if (!Payload->TryGetStringField(TEXT("value"), Value))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: value"));
		return;
	}

	// 映射配置文件�?
	FString ConfigFileName;
	if (ConfigName.Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEngineIni;
	}
	else if (ConfigName.Equals(TEXT("Game"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GGameIni;
	}
	else if (ConfigName.Equals(TEXT("Editor"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEditorIni;
	}
	else if (ConfigName.Equals(TEXT("EditorPerProjectUserSettings"), ESearchCase::IgnoreCase))
	{
		ConfigFileName = GEditorPerProjectIni;
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(TEXT("Unsupported config_name: %s. Supported: Engine, Game, Editor, EditorPerProjectUserSettings"), *ConfigName));
		return;
	}

	// 设置配置
	GConfig->SetString(*Section, *Key, *Value, ConfigFileName);
	
	// 刷新到磁�?
	GConfig->Flush(false, ConfigFileName);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("config_name"), ConfigName);
	Result->SetStringField(TEXT("section"), Section);
	Result->SetStringField(TEXT("key"), Key);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetStringField(TEXT("file_path"), ConfigFileName);

	UE_LOG(LogUALEditor, Log, TEXT("project.set_config: %s [%s] %s = %s"), *ConfigName, *Section, *Key, *Value);

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_EditorCommands::Handle_AnalyzeUProject(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const FString ProjectFilePath = FPaths::GetProjectFilePath();
	FString ProjectFileContent;
	
	if (!FFileHelper::LoadFileToString(ProjectFileContent, *ProjectFilePath))
	{
		UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(TEXT("Failed to read .uproject file: %s"), *ProjectFilePath));
		return;
	}

	TSharedPtr<FJsonObject> ProjectJson;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ProjectFileContent);
	
	if (!FJsonSerializer::Deserialize(Reader, ProjectJson) || !ProjectJson.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to parse .uproject file as JSON"));
		return;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// 提取 EngineAssociation
	FString EngineAssociation;
	if (ProjectJson->TryGetStringField(TEXT("EngineAssociation"), EngineAssociation))
	{
		Result->SetStringField(TEXT("engine_association"), EngineAssociation);
	}

	// 提取 TargetPlatforms
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
			Result->SetArrayField(TEXT("target_platforms"), PlatformArray);
		}
	}

	// 提取 Modules
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
					// 保留完整的模块对象（包含 Name, Type, LoadingPhase 等字段）
					ModuleArray.Add(MakeShared<FJsonValueObject>(ModuleObj));
				}
			}
		}
		if (ModuleArray.Num() > 0)
		{
			Result->SetArrayField(TEXT("modules"), ModuleArray);
		}
	}

	// 提取 Plugins（复�?BuildProjectInfo 中的逻辑�?
	const TArray<TSharedPtr<FJsonValue>>* ProjectPlugins = nullptr;
	if (ProjectJson->TryGetArrayField(TEXT("Plugins"), ProjectPlugins) && ProjectPlugins)
	{
		IPluginManager& PluginManager = IPluginManager::Get();

		TArray<TSharedPtr<FJsonValue>> PluginArray;

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

			// 尝试补全插件元信�?
			const TSharedPtr<IPlugin> Plugin = PluginManager.FindPlugin(PluginName);
			if (Plugin.IsValid())
			{
				PluginObj->SetStringField(TEXT("version_name"), Plugin->GetDescriptor().VersionName);
				PluginObj->SetStringField(TEXT("category"), Plugin->GetDescriptor().Category);
				PluginObj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
				PluginObj->SetStringField(TEXT("friendly_name"), Plugin->GetDescriptor().FriendlyName);
				PluginObj->SetStringField(TEXT("description"), Plugin->GetDescriptor().Description);
			}
			else
			{
				PluginObj->SetStringField(TEXT("version_name"), TEXT(""));
				PluginObj->SetStringField(TEXT("category"), TEXT(""));
				PluginObj->SetStringField(TEXT("base_dir"), TEXT(""));
				PluginObj->SetStringField(TEXT("friendly_name"), TEXT(""));
				PluginObj->SetStringField(TEXT("description"), TEXT(""));
			}

			PluginArray.Add(MakeShared<FJsonValueObject>(PluginObj));
		}

		if (PluginArray.Num() > 0)
		{
			Result->SetArrayField(TEXT("plugins"), PluginArray);
		}
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Result);
}

void FUAL_EditorCommands::Handle_CaptureAppWindow(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
    // --- 前半部分逻辑保持不变 (获取 SlateApp, 查找 TargetWindow) ---
    if (!FSlateApplication::IsInitialized()) {
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Slate Application is not initialized"));
		return;
	}
    FSlateApplication& SlateApp = FSlateApplication::Get();
    TSharedPtr<SWindow> TargetWindow = SlateApp.GetActiveTopLevelWindow();

    // 容错：如果没有活跃窗口，找第一个交互窗口
    if (!TargetWindow.IsValid())
    {
        TArray<TSharedRef<SWindow>> Windows = SlateApp.GetInteractiveTopLevelWindows();
        if (Windows.Num() > 0) TargetWindow = Windows[0];
    }

    if (!TargetWindow.IsValid())
    {
        UAL_CommandUtils::SendError(RequestId, 404, TEXT("No valid window to capture"));
        return;
    }

    // 确保窗口是可见的
	// Check IsMinimized via NativeWindow if possible
	if (TargetWindow->GetNativeWindow().IsValid() && TargetWindow->GetNativeWindow()->IsMinimized())
	{
		TargetWindow->Restore();
	}
    TargetWindow->BringToFront();
	
	if (TargetWindow->GetNativeWindow().IsValid())
	{
		TargetWindow->GetNativeWindow()->SetWindowFocus();
	}
    
    // --- 使用 Windows GDI API 直接截取窗口，避免 HDR/Gamma 问题 ---
#if PLATFORM_WINDOWS
    // 获取原生窗口句柄
    HWND Hwnd = nullptr;
    if (TargetWindow->GetNativeWindow().IsValid())
    {
        Hwnd = static_cast<HWND>(TargetWindow->GetNativeWindow()->GetOSWindowHandle());
    }
    
    if (!Hwnd)
    {
        UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to get native window handle"));
        return;
    }

    // 获取窗口客户区大小
    RECT ClientRect;
    GetClientRect(Hwnd, &ClientRect);
    int32 Width = ClientRect.right - ClientRect.left;
    int32 Height = ClientRect.bottom - ClientRect.top;
    FIntPoint TargetSize(Width, Height);

    if (Width <= 0 || Height <= 0)
    {
        UAL_CommandUtils::SendError(RequestId, 500, TEXT("Invalid window size"));
        return;
    }

    // 创建兼容 DC 和位图
    HDC WindowDC = GetDC(Hwnd);
    HDC MemDC = CreateCompatibleDC(WindowDC);
    HBITMAP HBitmap = CreateCompatibleBitmap(WindowDC, Width, Height);
    HBITMAP OldBitmap = (HBITMAP)SelectObject(MemDC, HBitmap);

    // 使用 PrintWindow 捕获窗口内容（包括 DWM 合成的内容）
    // PW_RENDERFULLCONTENT (0x00000002) 可以捕获 DWM 合成内容
    PrintWindow(Hwnd, MemDC, 0x00000002);

    // 准备位图信息
    BITMAPINFO BitmapInfo;
    ZeroMemory(&BitmapInfo, sizeof(BITMAPINFO));
    BitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    BitmapInfo.bmiHeader.biWidth = Width;
    BitmapInfo.bmiHeader.biHeight = -Height; // 负值表示自上而下
    BitmapInfo.bmiHeader.biPlanes = 1;
    BitmapInfo.bmiHeader.biBitCount = 32;
    BitmapInfo.bmiHeader.biCompression = BI_RGB;

    // 读取像素数据
    TArray<FColor> Bitmap;
    Bitmap.SetNum(Width * Height);
    GetDIBits(MemDC, HBitmap, 0, Height, Bitmap.GetData(), &BitmapInfo, DIB_RGB_COLORS);

    // 清理 GDI 资源
    SelectObject(MemDC, OldBitmap);
    DeleteObject(HBitmap);
    DeleteDC(MemDC);
    ReleaseDC(Hwnd, WindowDC);

    // GDI 返回 BGRA，但 Alpha 通常为 0，需要修正
    for (FColor& Pixel : Bitmap)
    {
        Pixel.A = 255;
    }
#else
    UAL_CommandUtils::SendError(RequestId, 500, TEXT("Window capture is only supported on Windows"));
    return;
#endif

    // PNG 编码与保存

    // 7. 编码与保存 (复制你原本的代码即可)
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
    TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

    if (ImageWrapper.IsValid() && ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), TargetSize.X, TargetSize.Y, ERGBFormat::BGRA, 8))
    {
        TArray64<uint8> CompressedData = ImageWrapper->GetCompressed();
        
        // ... (文件名处理代码) ...
        FString DesiredName;
        Payload->TryGetStringField(TEXT("filepath"), DesiredName);
        FString CleanName = FPaths::GetCleanFilename(DesiredName);
        if (CleanName.IsEmpty()) CleanName = FDateTime::Now().ToString(TEXT("UAL_AppShot_%Y%m%d_%H%M%S.png"));
        
        FString OutputDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots/UAL")));
        IFileManager::Get().MakeDirectory(*OutputDir, true);
        FString OutputPath = DesiredName.IsEmpty() || FPaths::IsRelative(DesiredName) ? 
             FPaths::Combine(OutputDir, CleanName) : DesiredName;
             
        // 自动创建目录
		if (!DesiredName.IsEmpty() && !FPaths::IsRelative(DesiredName))
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
		}

        if (FFileHelper::SaveArrayToFile(CompressedData, *OutputPath))
        {
            // 构建成功返回 JSON
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetStringField(TEXT("path"), OutputPath);
			Data->SetStringField(TEXT("filename"), CleanName);
            Data->SetNumberField(TEXT("width"), TargetSize.X);
            Data->SetNumberField(TEXT("height"), TargetSize.Y);
            Data->SetBoolField(TEXT("saved"), true);
            UAL_CommandUtils::SendResponse(RequestId, 200, Data);
            return;
        }
    }

    UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to save image"));
}

/**
 * 获取当前焦点编辑器上下文
 * 返回正在编辑的资产信息（蓝图、材质、关卡等）
 */
void FUAL_EditorCommands::Handle_GetFocusContext(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> FocusedEditor = nullptr;
    TArray<TSharedPtr<FJsonValue>> OpenEditors;

    // 1. 通过 UAssetEditorSubsystem 获取所有打开的资产编辑器
    if (GEditor)
    {
        UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (AssetEditorSubsystem)
        {
            TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
            
            for (UObject* Asset : EditedAssets)
            {
                if (!Asset) continue;
                
                TSharedPtr<FJsonObject> EditorInfo = MakeShared<FJsonObject>();
                FString AssetPath = Asset->GetPathName();
                FString AssetName = Asset->GetName();
                FString AssetType = TEXT("other");
                
                // 识别资产类型
                if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
                {
                    AssetType = TEXT("blueprint");
                    
                    // 获取蓝图父类
                    if (Blueprint->ParentClass)
                    {
                        EditorInfo->SetStringField(TEXT("parentClass"), Blueprint->ParentClass->GetName());
                    }
                    
                    // 检查是否有未保存的修改
                    EditorInfo->SetBoolField(TEXT("isModified"), Blueprint->GetOutermost()->IsDirty());
                }
                else if (UMaterial* Material = Cast<UMaterial>(Asset))
                {
                    AssetType = TEXT("material");
                    EditorInfo->SetBoolField(TEXT("isModified"), Material->GetOutermost()->IsDirty());
                }
                else if (UMaterialInstance* MatInstance = Cast<UMaterialInstance>(Asset))
                {
                    AssetType = TEXT("material_instance");
                    EditorInfo->SetBoolField(TEXT("isModified"), MatInstance->GetOutermost()->IsDirty());
                    
                    // 获取父材质
                    if (MatInstance->Parent)
                    {
                        EditorInfo->SetStringField(TEXT("parentMaterial"), MatInstance->Parent->GetPathName());
                    }
                }
                else if (Asset->IsA(UTexture::StaticClass()))
                {
                    AssetType = TEXT("texture");
                }
                else if (Asset->IsA(UStaticMesh::StaticClass()) || Asset->IsA(USkeletalMesh::StaticClass()))
                {
                    AssetType = TEXT("mesh");
                }
                
                EditorInfo->SetStringField(TEXT("type"), AssetType);
                EditorInfo->SetStringField(TEXT("name"), AssetName);
                EditorInfo->SetStringField(TEXT("path"), AssetPath);
                
                // 第一个编辑的资产作为焦点编辑器（通常是最近打开的）
                if (!FocusedEditor.IsValid())
                {
                    FocusedEditor = EditorInfo;
                }
                
                OpenEditors.Add(MakeShared<FJsonValueObject>(EditorInfo));
            }
        }
    }

    // 2. 如果没有资产编辑器打开，返回当前关卡信息
    if (!FocusedEditor.IsValid() && GEditor && GEditor->GetWorld())
    {
        FocusedEditor = MakeShared<FJsonObject>();
        FocusedEditor->SetStringField(TEXT("type"), TEXT("level"));
        FocusedEditor->SetStringField(TEXT("name"), GEditor->GetWorld()->GetMapName());
        FocusedEditor->SetStringField(TEXT("path"), GEditor->GetWorld()->GetOutermost()->GetName());
        FocusedEditor->SetBoolField(TEXT("isModified"), GEditor->GetWorld()->GetOutermost()->IsDirty());
    }

    // 3. 构建返回结果
    if (FocusedEditor.IsValid())
    {
        Result->SetObjectField(TEXT("focusedEditor"), FocusedEditor);
    }
    
    if (OpenEditors.Num() > 0)
    {
        Result->SetArrayField(TEXT("openEditors"), OpenEditors);
    }
    
    Result->SetBoolField(TEXT("hasOpenEditors"), OpenEditors.Num() > 0);
    
    UE_LOG(LogUALEditor, Log, TEXT("editor.get_focus_context: %d open editors, focused: %s"),
        OpenEditors.Num(),
        FocusedEditor.IsValid() ? *FocusedEditor->GetStringField(TEXT("name")) : TEXT("none"));
    
    UAL_CommandUtils::SendResponse(RequestId, 200, Result);
#else
    UAL_CommandUtils::SendError(RequestId, 501, TEXT("editor.get_focus_context is only available in editor mode"));
#endif
}


