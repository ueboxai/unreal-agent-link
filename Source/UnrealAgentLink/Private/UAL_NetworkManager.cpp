#include "UAL_NetworkManager.h"

#include "Async/Async.h"
#include "WebSocketsModule.h"
#include "HAL/PlatformProcess.h"

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
}

void FUAL_NetworkManager::Shutdown()
{
	bWantsReconnect = false;
	StopReconnectTimer();
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

void FUAL_NetworkManager::HandleOnMessage(const FString& Data)
{
	MessageReceivedDelegate.Broadcast(Data);
}

void FUAL_NetworkManager::HandleOnConnected()
{
	UE_LOG(LogUALNetwork, Display, TEXT("Connected to %s"), *TargetUrl);
	bIsConnecting = false;
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

