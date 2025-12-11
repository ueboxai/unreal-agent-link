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
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
#define UAL_WITH_EXTENDED_AVG_STATS 1
extern ENGINE_API float GAverageGameTime;
extern ENGINE_API float GAverageDrawTime;
extern ENGINE_API float GAverageRHITTime;
extern ENGINE_API float GAverageGPUTime;
#else
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

	bool bExecuted = false;
#if defined(WITH_PYTHON) && WITH_PYTHON
	if (IPythonScriptPlugin::IsAvailable())
	{
		bExecuted = IPythonScriptPlugin::Get()->ExecPythonCommand(*Script);
	}
#else
	UE_LOG(LogUALSystem, Warning, TEXT("WITH_PYTHON is not enabled; skip exec"));
#endif

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
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
	GameThreadMs = GAverageGameTime;
	RenderThreadMs = GAverageDrawTime;
	RHIMs = GAverageRHITTime;
	GPUMs = GAverageGPUTime;
#else
	GameThreadMs = FrameMs;
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
