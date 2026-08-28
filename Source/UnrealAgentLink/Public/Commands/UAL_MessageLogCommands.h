#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * MessageLog 命令处理器
 * 提供读取 UE 消息日志（Message Log）窗口内容的能力
 * 
 * 支持的命令：
 * - messagelog.list: 获取所有已注册的日志类别
 * - messagelog.get: 读取指定类别的消息
 * - messagelog.subscribe: 订阅类别变化（实时推送）
 * - messagelog.unsubscribe: 取消订阅
 */
class FUAL_MessageLogCommands
{
public:
	using FHandlerFunc = TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>;

	/**
	 * 注册所有 MessageLog 相关命令到 CommandMap
	 */
	static void RegisterCommands(TMap<FString, FHandlerFunc>& CommandMap);

	/**
	 * 获取所有已注册的日志类别
	 */
	static void Handle_List(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * 读取指定类别的消息日志内容
	 */
	static void Handle_Get(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * 订阅指定类别的消息变化
	 */
	static void Handle_Subscribe(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * 取消订阅指定类别
	 */
	static void Handle_Unsubscribe(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

private:
	/**
	 * 将单条 FTokenizedMessage 序列化为 JSON
	 */
	static TSharedPtr<FJsonObject> SerializeMessage(const TSharedRef<class FTokenizedMessage>& Message);

	/**
	 * 获取严重级别的字符串表示
	 */
	static FString SeverityToString(int32 Severity);
};
