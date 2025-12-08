#include "UAL_LogInterceptor.h"

#include "UAL_NetworkManager.h"

#include "Async/Async.h"
#include "Logging/LogVerbosity.h"
#include "Misc/OutputDeviceHelper.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

void FUAL_LogInterceptor::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category)
{
	if (!bIsCaptureEnabled)
	{
		return;
	}

	const FString Message = FString(V);
	const FString CategoryName = Category.ToString();
	FString VerbosityName;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	VerbosityName = FLogVerbosity::ToString(Verbosity);
#else
	VerbosityName = FOutputDeviceHelper::VerbosityToString(Verbosity);
#endif

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

