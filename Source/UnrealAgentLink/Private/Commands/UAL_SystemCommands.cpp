#include "UAL_SystemCommands.h"
#include "UAL_CommandUtils.h"

#include "IPythonScriptPlugin.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/App.h"
#include "EngineStats.h"

// 性能统计宏定义
#if defined(STATS) && STATS
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;
// UE 5.1+ 正式支持扩展统计变量（ENGINE_API导出）
// UE 5.0 中这些变量可能存在于引擎内部但未导出，无法直接访问
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#define UAL_WITH_EXTENDED_AVG_STATS 0
extern ENGINE_API float GAverageGameTime;
extern ENGINE_API float GAverageDrawTime;
extern ENGINE_API float GAverageRHITTime;
extern ENGINE_API float GAverageGPUTime;
#else
// UE 5.0: 这些变量在引擎中可能存在但未通过ENGINE_API导出
// 直接声明会导致链接错误，因此无法在5.0中直接访问
// 用户可以通过 stat unit 等控制台命令查看这些信息
#define UAL_WITH_EXTENDED_AVG_STATS 0
#endif
#else
#define UAL_WITH_EXTENDED_AVG_STATS 0
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALSystem, Log, All);

void FUAL_SystemCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("cmd.run_python"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_RunPython(Payload, RequestId);
	});

	CommandMap.Add(TEXT("cmd.exec_console"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ExecConsole(Payload, RequestId);
	});

	CommandMap.Add(TEXT("system.run_console_command"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ExecConsole(Payload, RequestId);
	});

	CommandMap.Add(TEXT("system.get_performance_stats"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetPerformanceStats(Payload, RequestId);
	});

	CommandMap.Add(TEXT("system.manage_plugin"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ManagePlugin(Payload, RequestId);
	});

	CommandMap.Add(TEXT("system.get_project_info"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetProjectInfo(Payload, RequestId);
	});
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========
// 原始行号参考:
//   Handle_RunPython:           1631-1653
//   Handle_ExecConsole:         1655-1678
//   Handle_GetPerformanceStats: 1680-1719

void FUAL_SystemCommands::Handle_RunPython(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Script;
	if (!Payload->TryGetStringField(TEXT("script"), Script))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: script"));
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	bool bExecuted = false;

#if defined(WITH_PYTHON) && WITH_PYTHON
	if (IPythonScriptPlugin::IsAvailable())
	{
		// 使用扩展版本获取详细输出
		FPythonCommandEx PythonCommand;
		PythonCommand.Command = Script;
		PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Public; // 共享环境，便于后续脚本访问变量
		
		bExecuted = IPythonScriptPlugin::Get()->ExecPythonCommandEx(PythonCommand);
		
		// 返回脚本结果（成功时为表达式结果，失败时为错误追踪）
		if (!PythonCommand.CommandResult.IsEmpty())
		{
			Data->SetStringField(TEXT("result"), PythonCommand.CommandResult);
		}
		
		// 收集日志输出（print()、unreal.log() 等）
		if (PythonCommand.LogOutput.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> LogArray;
			for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
			{
				TSharedPtr<FJsonObject> LogEntry = MakeShared<FJsonObject>();
				LogEntry->SetStringField(TEXT("type"), LexToString(Entry.Type));
				LogEntry->SetStringField(TEXT("message"), Entry.Output);
				LogArray.Add(MakeShared<FJsonValueObject>(LogEntry));
			}
			Data->SetArrayField(TEXT("logs"), LogArray);
		}
	}
#else
	UE_LOG(LogUALSystem, Warning, TEXT("WITH_PYTHON is not enabled; skip exec"));
#endif

	Data->SetBoolField(TEXT("ok"), bExecuted);
	UAL_CommandUtils::SendResponse(RequestId, bExecuted ? 200 : 500, Data);
}

void FUAL_SystemCommands::Handle_ExecConsole(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Command;
	if (!Payload->TryGetStringField(TEXT("command"), Command))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: command"));
		return;
	}

	bool bResult = false;
	if (GEngine)
	{
#if WITH_EDITOR
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
#else
		UWorld* World = GWorld;
#endif
		bResult = GEngine->Exec(World, *Command);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("result"), bResult ? TEXT("OK") : TEXT("Failed"));
	UAL_CommandUtils::SendResponse(RequestId, bResult ? 200 : 500, Data);
}

void FUAL_SystemCommands::Handle_GetPerformanceStats(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	float Fps = 0.0f;
	float FrameMs = 0.0f;
	float GameThreadMs = 0.0f;
	float RenderThreadMs = 0.0f;
	float RHIMs = 0.0f;
	float GPUMs = 0.0f;

#if defined(STATS) && STATS
	Fps = GAverageFPS;
	FrameMs = GAverageMS;
#if UAL_WITH_EXTENDED_AVG_STATS
	// UE 5.1+: 使用正式导出的扩展统计变量
	GameThreadMs = GAverageGameTime;
	RenderThreadMs = GAverageDrawTime;
	RHIMs = GAverageRHITTime;
	GPUMs = GAverageGPUTime;
#else
	// UE 5.0: 扩展统计变量未通过ENGINE_API导出，无法直接访问
	// 这些变量在引擎内部可能存在，但由于未导出，插件无法访问
	// 因此 RenderThreadMs、RHIMs、GPUMs 将保持为 0
	// 
	// 说明：
	// - 这是UE 5.0的限制，不是插件的问题
	// - 用户可以通过控制台命令查看这些信息：
	//   * stat unit - 显示所有线程和GPU时间
	//   * stat scenerendering - 显示场景渲染统计
	//   * stat rhi - 显示RHI线程统计
	//   * stat game - 显示游戏线程统计
	// - 在UE 5.1+中，这些值可以正常获取
	GameThreadMs = FrameMs; // 使用FrameMs作为GameThreadMs的近似值
	RenderThreadMs = 0.0f;   // UE 5.0中无法获取，保持为0
	RHIMs = 0.0f;           // UE 5.0中无法获取，保持为0
	GPUMs = 0.0f;           // UE 5.0中无法获取，保持为0
#endif
#else
	const float DeltaSeconds = FApp::GetDeltaTime();
	if (DeltaSeconds > SMALL_NUMBER)
	{
		Fps = 1.0f / DeltaSeconds;
		FrameMs = DeltaSeconds * 1000.0f;
		GameThreadMs = FrameMs;
	}
#endif

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("fps"), Fps);
	Data->SetNumberField(TEXT("frame_ms"), FrameMs);
	Data->SetNumberField(TEXT("game_thread_ms"), GameThreadMs);
	Data->SetNumberField(TEXT("render_thread_ms"), RenderThreadMs);
	Data->SetNumberField(TEXT("rhi_thread_ms"), RHIMs);
	Data->SetNumberField(TEXT("gpu_ms"), GPUMs);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_SystemCommands::Handle_ManagePlugin(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString PluginName;
	if (!Payload->TryGetStringField(TEXT("plugin_name"), PluginName) || PluginName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: plugin_name"));
		return;
	}

	FString ActionString;
	const bool bHasAction = Payload->TryGetStringField(TEXT("action"), ActionString);
	const FString NormalizedAction = bHasAction ? ActionString.ToLower() : TEXT("query");

	IPluginManager& PluginManager = IPluginManager::Get();
	const TSharedPtr<IPlugin> Plugin = PluginManager.FindPlugin(PluginName);
	if (!Plugin.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Plugin '%s' not found"), *PluginName));
		return;
	}

	bool bSuccess = true;
	bool bRequiresRestart = false;
	FString Message;

	const bool bCurrentlyEnabled = Plugin->IsEnabled();

	auto SetPluginEnabled = [&PluginName](bool bEnable) -> bool
	{
		// 统一使用 ProjectManager 设置，它会自动处理 .uproject 文件更新
		FText FailReason;
		return IProjectManager::Get().SetPluginEnabled(PluginName, bEnable, FailReason);
	};

	if (NormalizedAction == TEXT("enable"))
	{
		if (!bCurrentlyEnabled)
		{
			if (SetPluginEnabled(true))
			{
				bRequiresRestart = true;
				Message = TEXT("Plugin enabled. Restart required.");
			}
			else
			{
				bSuccess = false;
				Message = TEXT("Failed to enable plugin.");
			}
		}
	}
	else if (NormalizedAction == TEXT("disable"))
	{
		if (bCurrentlyEnabled)
		{
			if (SetPluginEnabled(false))
			{
				bRequiresRestart = true;
				Message = TEXT("Plugin disabled. Restart required.");
			}
			else
			{
				bSuccess = false;
				Message = TEXT("Failed to disable plugin.");
			}
		}
	}
	else if (NormalizedAction == TEXT("query"))
	{
		// no-op
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 400, FString::Printf(TEXT("Unsupported action: %s"), *ActionString));
		return;
	}

	const bool bIsEnabled = Plugin->IsEnabled();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("plugin_name"), Plugin->GetName());
	Result->SetBoolField(TEXT("is_enabled"), bIsEnabled);
	Result->SetBoolField(TEXT("requires_restart"), bRequiresRestart);
	Result->SetStringField(TEXT("friendly_name"), Plugin->GetDescriptor().FriendlyName);
	if (!Message.IsEmpty())
	{
		Result->SetStringField(TEXT("message"), Message);
	}

	UAL_CommandUtils::SendResponse(RequestId, bSuccess ? 200 : 500, Result);
}

/**
 * system.get_project_info - 获取项目信息
 * 返回项目路径、Content目录等信息,用于外部工具快速定位项目资源
 */
void FUAL_SystemCommands::Handle_GetProjectInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const FString ProjectFilePath = FPaths::GetProjectFilePath();
	const FString ProjectDir = FPaths::ProjectDir();
	const FString ContentDir = FPaths::ProjectContentDir();
	const FString SavedDir = FPaths::ProjectSavedDir();
	const FString IntermediateDir = FPaths::ProjectIntermediateDir();
	const FString PluginsDir = FPaths::ProjectPluginsDir();
	
	// 获取项目名称
	FString ProjectName = FApp::GetProjectName();
	
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetStringField(TEXT("project_name"), ProjectName);
	Response->SetStringField(TEXT("project_path"), ProjectFilePath);
	Response->SetStringField(TEXT("project_dir"), ProjectDir);
	Response->SetStringField(TEXT("content_dir"), ContentDir);
	Response->SetStringField(TEXT("saved_dir"), SavedDir);
	Response->SetStringField(TEXT("intermediate_dir"), IntermediateDir);
	Response->SetStringField(TEXT("plugins_dir"), PluginsDir);
	
	// 添加引擎版本信息
	Response->SetStringField(TEXT("engine_version"), FApp::GetBuildVersion());
	Response->SetNumberField(TEXT("engine_major"), ENGINE_MAJOR_VERSION);
	Response->SetNumberField(TEXT("engine_minor"), ENGINE_MINOR_VERSION);
	
	UE_LOG(LogUALSystem, Log, TEXT("system.get_project_info: %s"), *ProjectName);
	
	UAL_CommandUtils::SendResponse(RequestId, 200, Response);
}
