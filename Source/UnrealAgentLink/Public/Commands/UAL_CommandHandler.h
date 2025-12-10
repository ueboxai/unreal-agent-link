#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 解析 JSON 指令并在 GameThread 执行
 * Acts as the central dispatcher for all commands.
 */
class FUAL_CommandHandler
{
public:
	FUAL_CommandHandler();

	// 必须在 GameThread 调用
	void ProcessMessage(const FString& JsonPayload);

	// 构建项目信息（代理到 FUAL_EditorCommands::BuildProjectInfo）
	TSharedPtr<FJsonObject> BuildProjectInfo() const;

private:
	using FHandlerFunc = TFunction<void(const TSharedPtr<FJsonObject>& /*Payload*/, const FString /*RequestId*/)> ;

	void RegisterCommands();

	// Response / Event 辅助 (This might be better in CommandUtils, but ProcessMessage uses it indirectly? 
	// No, ProcessMessage calls handlers, handlers call SendResponse. 
	// Provide a way to dispatch? Or just let handlers use CommandUtils::SendResponse directly?)
	// Handlers use CommandUtils::SendResponse.
	// But Handle_Response is used for notifications presumably.

	// 响应处理（用于弹出通知）
	void Handle_Response(const FString& Method, const TSharedPtr<FJsonObject>& Payload);

private:
	TMap<FString, FHandlerFunc> CommandMap;
};
