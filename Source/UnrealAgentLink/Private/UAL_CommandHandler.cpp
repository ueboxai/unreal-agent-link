#include "UAL_CommandHandler.h"

#include "UAL_NetworkManager.h"

#include "Async/Async.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#include "IPythonScriptPlugin.h"
#include "Editor.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALCommand, Log, All);

namespace
{
	bool IsZh()
	{
		FString Name;
		if (const TSharedPtr<const FCulture> Culture = FInternationalization::Get().GetCurrentCulture())
		{
			Name = Culture->GetName();
		}
		return Name.StartsWith(TEXT("zh"));
	}

	FString LStr(const TCHAR* Zh, const TCHAR* En)
	{
		return IsZh() ? Zh : En;
	}

	FText LText(const TCHAR* Zh, const TCHAR* En)
	{
		return FText::FromString(LStr(Zh, En));
	}
}

FUAL_CommandHandler::FUAL_CommandHandler()
{
	RegisterCommands();
}

void FUAL_CommandHandler::ProcessMessage(const FString& JsonPayload)
{
	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this, Payload = JsonPayload]()
		{
			ProcessMessage(Payload);
		});
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogUALCommand, Warning, TEXT("Invalid JSON payload: %s"), *JsonPayload);
		return;
	}

	FString Type, Method, RequestId;
	Root->TryGetStringField(TEXT("type"), Type);
	Root->TryGetStringField(TEXT("method"), Method);
	Root->TryGetStringField(TEXT("id"), RequestId);

	const TSharedPtr<FJsonObject>* PayloadObj = nullptr;
	Root->TryGetObjectField(TEXT("payload"), PayloadObj);

	if (Type != TEXT("req"))
	{
		if (Type == TEXT("res"))
		{
			Handle_Response(Method, PayloadObj ? *PayloadObj : nullptr);
		}
		else
		{
			UE_LOG(LogUALCommand, Verbose, TEXT("Ignore non-request message: %s"), *Type);
		}
		return;
	}

	const FHandlerFunc* Handler = CommandMap.Find(Method);
	if (!Handler)
	{
		SendError(RequestId, 404, FString::Printf(TEXT("Unknown method: %s"), *Method));
		return;
	}

	(*Handler)(PayloadObj ? *PayloadObj : MakeShared<FJsonObject>(), RequestId);
}

void FUAL_CommandHandler::RegisterCommands()
{
	CommandMap.Add(TEXT("cmd.run_python"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_RunPython(Payload, RequestId);
	});

	CommandMap.Add(TEXT("cmd.exec_console"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ExecConsole(Payload, RequestId);
	});
}

void FUAL_CommandHandler::Handle_RunPython(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Script;
	if (!Payload->TryGetStringField(TEXT("script"), Script))
	{
		SendError(RequestId, 400, TEXT("Missing field: script"));
		return;
	}

	bool bExecuted = false;
#if defined(WITH_PYTHON) && WITH_PYTHON
	if (IPythonScriptPlugin::IsAvailable())
	{
		bExecuted = IPythonScriptPlugin::Get()->ExecPythonCommand(*Script);
	}
#else
	UE_LOG(LogUALCommand, Warning, TEXT("WITH_PYTHON is not enabled; skip exec"));
#endif

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), bExecuted);
	SendResponse(RequestId, bExecuted ? 200 : 500, Data);
}

void FUAL_CommandHandler::Handle_ExecConsole(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Command;
	if (!Payload->TryGetStringField(TEXT("command"), Command))
	{
		SendError(RequestId, 400, TEXT("Missing field: command"));
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
	SendResponse(RequestId, bResult ? 200 : 500, Data);
}

void FUAL_CommandHandler::Handle_Response(const FString& Method, const TSharedPtr<FJsonObject>& Payload)
{
	if (!Payload.IsValid())
	{
		return;
	}

	bool bOk = true;
	Payload->TryGetBoolField(TEXT("ok"), bOk);

	int32 Count = 0;
	if (!Payload->TryGetNumberField(TEXT("count"), Count))
	{
		Count = 0;
	}

	FString Error;
	Payload->TryGetStringField(TEXT("error"), Error);

	const bool bIsImportFolder = Method == TEXT("content.import_folder");
	const bool bIsImportAssets = Method == TEXT("content.import_assets");
	if (!bIsImportFolder && !bIsImportAssets)
	{
		return; // 非导入相关响应不提示
	}

	const FString Title = bIsImportFolder
		? LStr(TEXT("导入文件夹"), TEXT("Import Folder"))
		: LStr(TEXT("导入资产"), TEXT("Import Assets"));

	FString Body;
	if (bOk)
	{
		Body = FString::Printf(TEXT("%s: %d"),
			*LStr(TEXT("成功"), TEXT("Succeeded")),
			Count);
	}
	else
	{
		if (Error.IsEmpty())
		{
			Error = LStr(TEXT("导入失败"), TEXT("Import failed"));
		}
		Body = FString::Printf(TEXT("%s (%s)"),
			*LStr(TEXT("失败"), TEXT("Failed")),
			*Error);
	}

	FNotificationInfo Info(FText::FromString(Title));
	Info.SubText = FText::FromString(Body);
	Info.ExpireDuration = 4.0f;
	Info.FadeOutDuration = 0.5f;
	Info.bUseThrobber = false;
	Info.bFireAndForget = true;

	FSlateNotificationManager::Get().AddNotification(Info);
}

void FUAL_CommandHandler::SendResponse(const FString& RequestId, int32 Code, const TSharedPtr<FJsonObject>& Data)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ver"), TEXT("1.0"));
	Root->SetStringField(TEXT("type"), TEXT("res"));
	Root->SetStringField(TEXT("id"), RequestId);
	Root->SetNumberField(TEXT("code"), Code);

	if (Data.IsValid())
	{
		Root->SetObjectField(TEXT("data"), Data);
	}

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	FUAL_NetworkManager::Get().SendMessage(OutJson);
}

void FUAL_CommandHandler::SendError(const FString& RequestId, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("message"), Message);
	SendResponse(RequestId, Code, Data);
}

