#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 解析 JSON 指令并在 GameThread 执行
 */
class FUAL_CommandHandler
{
public:
	FUAL_CommandHandler();

	// 必须在 GameThread 调用
	void ProcessMessage(const FString& JsonPayload);

private:
	using FHandlerFunc = TFunction<void(const TSharedPtr<FJsonObject>& /*Payload*/, const FString /*RequestId*/)> ;

	void RegisterCommands();

	// 指令实现
	void Handle_RunPython(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	void Handle_ExecConsole(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// 响应处理（用于弹出通知）
	void Handle_Response(const FString& Method, const TSharedPtr<FJsonObject>& Payload);

	// Response / Event 辅助
	void SendResponse(const FString& RequestId, int32 Code, const TSharedPtr<FJsonObject>& Data = nullptr);
	void SendError(const FString& RequestId, int32 Code, const FString& Message);

private:
	TMap<FString, FHandlerFunc> CommandMap;
};

