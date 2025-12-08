#include "UAL_LogInterceptor.h"

#include "UAL_NetworkManager.h"

#include "Async/Async.h"
#include "Logging/LogVerbosity.h"
#include "Misc/OutputDeviceHelper.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/IConsoleManager.h"

// 兼容 5.0 与 5.1+ 的日志等级字符串获取
static FString UALVerbosityToString(ELogVerbosity::Type Verbosity)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	return FLogVerbosity::ToString(Verbosity);
#else
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return FOutputDeviceHelper::VerbosityToString(Verbosity);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
}

void FUAL_LogInterceptor::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category)
{
	// 控制台变量开关，默认关闭日志转发，需手动开启：ual.ForwardLogs 1
	static TAutoConsoleVariable<int32> CVarForwardLogs(
		TEXT("ual.ForwardLogs"),
		0,
		TEXT("Forward UE logs to Unreal Agent (0=off, 1=on)"),
		ECVF_Default);

	if (!bIsCaptureEnabled || CVarForwardLogs.GetValueOnAnyThread() == 0)
	{
		return;
	}

	// 避免将网络层自身的日志再次发出去，造成递归与卡死
	static const FName NetworkLogCategory(TEXT("LogUALNetwork"));
	if (Category == NetworkLogCategory)
	{
		return;
	}

	// 未连接时直接丢弃日志，避免无网时不停重入 SendMessage
	if (!FUAL_NetworkManager::Get().IsConnected())
	{
		return;
	}

	const FString Message = FString(V);
	const FString CategoryName = Category.ToString();
	const FString VerbosityName = UALVerbosityToString(Verbosity);

	AsyncTask(ENamedThreads::GameThread, [Message, CategoryName, VerbosityName]()
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("text"), Message);
		Payload->SetStringField(TEXT("category"), CategoryName);
		Payload->SetStringField(TEXT("level"), VerbosityName);

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("ver"), TEXT("1.0"));
		Root->SetStringField(TEXT("type"), TEXT("evt"));
		Root->SetStringField(TEXT("method"), TEXT("log.entry"));
		Root->SetObjectField(TEXT("payload"), Payload);

		FString OutJson;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

		FUAL_NetworkManager::Get().SendMessage(OutJson);
	});
}

