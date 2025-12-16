#include "UAL_NetworkManager.h"

#include "Async/Async.h"
#include "WebSocketsModule.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALNetwork, Log, All);

FUAL_NetworkManager& FUAL_NetworkManager::Get()
{
	static FUAL_NetworkManager Instance;
	return Instance;
}

void FUAL_NetworkManager::Init(const FString& ServerUrl)
{
	TargetUrl = ServerUrl;
	bWantsReconnect = true;

	Connect();
	StartReconnectTimer();
	StartHeartbeatTimer();
}

void FUAL_NetworkManager::Shutdown()
{
	bWantsReconnect = false;
	StopReconnectTimer();
	StopHeartbeatTimer();
	CleanupSocket();
}

bool FUAL_NetworkManager::IsConnected() const
{
	return Socket.IsValid() && Socket->IsConnected();
}

void FUAL_NetworkManager::SendMessage(const FString& JsonData)
{
	FScopeLock Lock(&SendMutex);
	if (IsConnected())
	{
		UE_LOG(LogUALNetwork, Verbose, TEXT("SendMessage: %s"), *JsonData);
		Socket->Send(JsonData);
	}
	else
	{
		UE_LOG(LogUALNetwork, Warning, TEXT("SendMessage skipped: socket not connected"));
	}
}

void FUAL_NetworkManager::Connect()
{
	if (bIsConnecting || TargetUrl.IsEmpty())
	{
		return;
	}

	if (!FModuleManager::Get().IsModuleLoaded("WebSockets"))
	{
		FModuleManager::LoadModuleChecked<FWebSocketsModule>("WebSockets");
	}

	bIsConnecting = true;
	UE_LOG(LogUALNetwork, Log, TEXT("Connecting to %s"), *TargetUrl);

	Socket = FWebSocketsModule::Get().CreateWebSocket(TargetUrl);
	BindSocketEvents();
	Socket->Connect();
}

void FUAL_NetworkManager::BindSocketEvents()
{
	if (!Socket.IsValid())
	{
		return;
	}

	Socket->OnConnected().AddRaw(this, &FUAL_NetworkManager::HandleOnConnected);
	Socket->OnConnectionError().AddRaw(this, &FUAL_NetworkManager::HandleOnConnectionError);
	Socket->OnClosed().AddRaw(this, &FUAL_NetworkManager::HandleOnClosed);
	Socket->OnMessage().AddRaw(this, &FUAL_NetworkManager::HandleOnMessage);
}

void FUAL_NetworkManager::CleanupSocket()
{
	if (Socket.IsValid())
	{
		Socket->OnConnected().RemoveAll(this);
		Socket->OnConnectionError().RemoveAll(this);
		Socket->OnClosed().RemoveAll(this);
		Socket->OnMessage().RemoveAll(this);
		Socket->Close();
		Socket.Reset();
	}
	bIsConnecting = false;
}

void FUAL_NetworkManager::StartReconnectTimer()
{
	if (ReconnectTickerHandle.IsValid())
	{
		return;
	}

	ReconnectTickerHandle = UAL_CORE_TICKER.AddTicker(FTickerDelegateType::CreateRaw(this, &FUAL_NetworkManager::TickReconnect), 5.0f);
}

void FUAL_NetworkManager::StopReconnectTimer()
{
	if (ReconnectTickerHandle.IsValid())
	{
		UAL_CORE_TICKER.RemoveTicker(ReconnectTickerHandle);
		ReconnectTickerHandle.Reset();
	}
}

bool FUAL_NetworkManager::TickReconnect(float DeltaTime)
{
	if (!bWantsReconnect)
	{
		return false; // 停止 ticker
	}

	if (!IsConnected() && !bIsConnecting)
	{
		UE_LOG(LogUALNetwork, Verbose, TEXT("Reconnect ticker triggering connect"));
		Connect();
	}
	return true; // 继续运行
}

void FUAL_NetworkManager::StartHeartbeatTimer()
{
	if (HeartbeatTickerHandle.IsValid())
	{
		return;
	}

	// 每10秒发送一次心跳（前端配置是15秒检查间隔，45秒超时，所以10秒发送一次足够）
	HeartbeatTickerHandle = UAL_CORE_TICKER.AddTicker(FTickerDelegateType::CreateRaw(this, &FUAL_NetworkManager::TickHeartbeat), 10.0f);
}

void FUAL_NetworkManager::StopHeartbeatTimer()
{
	if (HeartbeatTickerHandle.IsValid())
	{
		UAL_CORE_TICKER.RemoveTicker(HeartbeatTickerHandle);
		HeartbeatTickerHandle.Reset();
	}
}

bool FUAL_NetworkManager::TickHeartbeat(float DeltaTime)
{
	if (!bWantsReconnect || !IsConnected())
	{
		return false; // 停止 ticker
	}

	// 发送心跳消息
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ver"), TEXT("1.0"));
	Root->SetStringField(TEXT("type"), TEXT("evt"));
	Root->SetStringField(TEXT("method"), TEXT("system.heartbeat"));
	
	// payload可以为空，或者包含一些基本信息
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("payload"), Payload);

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	SendMessage(OutJson);

	return true; // 继续运行
}

void FUAL_NetworkManager::HandleOnMessage(const FString& Data)
{
	MessageReceivedDelegate.Broadcast(Data);
}

void FUAL_NetworkManager::HandleOnConnected()
{
	UE_LOG(LogUALNetwork, Display, TEXT("Connected to %s"), *TargetUrl);
	bIsConnecting = false;
	ConnectedDelegate.Broadcast();
}

void FUAL_NetworkManager::HandleOnClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogUALNetwork, Warning, TEXT("Socket closed (%d): %s Clean=%d"), StatusCode, *Reason, bWasClean);
	bIsConnecting = false;
	if (bWantsReconnect)
	{
		CleanupSocket();
	}
}

void FUAL_NetworkManager::HandleOnConnectionError(const FString& Error)
{
	UE_LOG(LogUALNetwork, Error, TEXT("Connection error: %s"), *Error);
	bIsConnecting = false;
	if (bWantsReconnect)
	{
		CleanupSocket();
	}
}

