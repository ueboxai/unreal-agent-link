#include "UAL_CommandHandler.h"
#include "UAL_NetworkManager.h"
#include "UAL_CommandUtils.h"
#include "UAL_ActorCommands.h"
#include "UAL_SystemCommands.h"
#include "UAL_LevelCommands.h"
#include "UAL_EditorCommands.h"
#include "UAL_BlueprintCommands.h"
#include "UAL_ContentBrowserCommands.h"

#include "Async/Async.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Misc/MessageDialog.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALCommand, Log, All);

FUAL_CommandHandler::FUAL_CommandHandler()
{
	RegisterCommands();
}

TSharedPtr<FJsonObject> FUAL_CommandHandler::BuildProjectInfo() const
{
	return FUAL_EditorCommands::BuildProjectInfo();
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

	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	if (Type == TEXT("req"))
	{
		// JSON-RPC 风格：params；兼容旧字段 payload
		if (!Root->TryGetObjectField(TEXT("params"), ParamsObj))
		{
			Root->TryGetObjectField(TEXT("payload"), ParamsObj);
		}
	}
	else if (Type == TEXT("res"))
	{
		// JSON-RPC 风格：result；兼容旧字段 data
		if (!Root->TryGetObjectField(TEXT("result"), ParamsObj))
		{
			Root->TryGetObjectField(TEXT("data"), ParamsObj);
		}
	}

	UE_LOG(LogUALCommand, Display, TEXT("Recv message type=%s method=%s id=%s"), *Type, *Method, *RequestId);

	if (Type != TEXT("req"))
	{
		if (Type == TEXT("res"))
		{
			Handle_Response(Method, ParamsObj ? *ParamsObj : nullptr);
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
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Unknown method: %s"), *Method));
		return;
	}

	(*Handler)(ParamsObj ? *ParamsObj : MakeShared<FJsonObject>(), RequestId);
}

void FUAL_CommandHandler::RegisterCommands()
{
	// 统一调用各模块的 RegisterCommands 函数
	// 新增命令只需在对应模块的 RegisterCommands 中添加即可
	
	FUAL_SystemCommands::RegisterCommands(CommandMap);
	FUAL_LevelCommands::RegisterCommands(CommandMap);
	FUAL_EditorCommands::RegisterCommands(CommandMap);
	FUAL_ActorCommands::RegisterCommands(CommandMap);
	FUAL_BlueprintCommands::RegisterCommands(CommandMap);
	FUAL_ContentBrowserCommands::RegisterCommands(CommandMap);
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

	FString ImportedPath;
	Payload->TryGetStringField(TEXT("importedPath"), ImportedPath);

	FString Error;
	Payload->TryGetStringField(TEXT("error"), Error);

	const bool bIsImportFolder = Method == TEXT("content.import_folder");
	const bool bIsImportAssets = Method == TEXT("content.import_assets");
	if (!bIsImportFolder && !bIsImportAssets)
	{
		return; // 非导入相关响应不提示
	}

	const FString Title = bIsImportFolder
		? UAL_CommandUtils::LStr(TEXT("导入文件夹到虚幻助手资产库"), TEXT("Import Folder to Unreal Agent Asset Library"))
		: UAL_CommandUtils::LStr(TEXT("导入资产到虚幻助手资产库"), TEXT("Import Assets to Unreal Agent Asset Library"));

	FString Body;
	if (bOk)
	{
		if (!ImportedPath.IsEmpty())
		{
			Body = FString::Printf(TEXT("%s: %s"),
				*UAL_CommandUtils::LStr(TEXT("成功"), TEXT("Succeeded")),
				*ImportedPath);
		}
		else
		{
			Body = FString::Printf(TEXT("%s: %d"),
				*UAL_CommandUtils::LStr(TEXT("成功"), TEXT("Succeeded")),
				Count);
		}
	}
	else
	{
		if (Error.IsEmpty())
		{
			Error = UAL_CommandUtils::LStr(TEXT("导入失败"), TEXT("Import failed"));
		}
		Body = FString::Printf(TEXT("%s (%s)"),
			*UAL_CommandUtils::LStr(TEXT("失败"), TEXT("Failed")),
			*Error);
	}

	FNotificationInfo Info(FText::FromString(Title));
	Info.SubText = FText::FromString(Body);
	Info.ExpireDuration = 4.0f;
	Info.FadeOutDuration = 0.5f;
	Info.bUseThrobber = false;
	Info.bFireAndForget = true;

#if WITH_EDITOR
	TSharedPtr<SNotificationItem> Notification;
	if (FSlateApplication::IsInitialized())
	{
		Notification = FSlateNotificationManager::Get().AddNotification(Info);
		if (Notification.IsValid())
		{
			Notification->SetCompletionState(bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	if (!Notification.IsValid())
	{
		const FText DialogText = FText::Format(
			FText::FromString(TEXT("{0}\n{1}")),
			FText::FromString(Title),
			FText::FromString(Body));
		FMessageDialog::Open(EAppMsgType::Ok, DialogText);
	}
#else
	FSlateNotificationManager::Get().AddNotification(Info);
#endif
}
