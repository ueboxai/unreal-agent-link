#pragma once

#include "CoreMinimal.h"
#include "IWebSocket.h"
#include "Runtime/Launch/Resources/Version.h"

// 线程间消息通知
DECLARE_MULTICAST_DELEGATE_OneParam(FUALOnMessageReceived, const FString&);
DECLARE_MULTICAST_DELEGATE(FUALOnConnected);

#include "Containers/Ticker.h"
using FTickerHandleType = FTSTicker::FDelegateHandle;
using FTickerDelegateType = FTickerDelegate;
#define UAL_CORE_TICKER FTSTicker::GetCoreTicker()

/**
 * 维护 WebSocket 连接、心跳、重连
 */
class FUAL_NetworkManager
{
public:
	static FUAL_NetworkManager& Get();

	// 初始化并连接到指定服务器
	void Init(const FString& ServerUrl);

	// 关闭连接并释放资源
	void Shutdown();

	// 发送消息（线程安全）
	void SendMessage(const FString& JsonData);

	// 接收消息回调（在 Socket 线程触发，外部需切到 GameThread）
	FUALOnMessageReceived& OnMessageReceived() { return MessageReceivedDelegate; }

	// 连接成功回调（在 Socket 线程触发，外部需切到 GameThread）
	FUALOnConnected& OnConnected() { return ConnectedDelegate; }

	// 当前是否已连接
	bool IsConnected() const;

private:
	FUAL_NetworkManager() = default;

	void Connect();
	void BindSocketEvents();
	void CleanupSocket();

	void StartReconnectTimer();
	void StopReconnectTimer();
	bool TickReconnect(float DeltaTime);

	void StartHeartbeatTimer();
	void StopHeartbeatTimer();
	bool TickHeartbeat(float DeltaTime);

	void HandleOnMessage(const FString& Data);
	void HandleOnConnected();
	void HandleOnClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void HandleOnConnectionError(const FString& Error);

private:
	FCriticalSection SendMutex;
	TSharedPtr<IWebSocket> Socket;
	FString TargetUrl;

	FTickerHandleType ReconnectTickerHandle;
	FTickerHandleType HeartbeatTickerHandle;
	bool bIsConnecting = false;
	bool bWantsReconnect = false;

	FUALOnMessageReceived MessageReceivedDelegate;
	FUALOnConnected ConnectedDelegate;
};

