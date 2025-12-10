#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 关卡工具命令处理器
 * 包含: level.query_assets
 * 
 * 对应文档: 关卡工具接口文档.md
 */
class FUAL_LevelCommands
{
public:
	/**
	 * 注册所有关卡相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// Public Handlers called by Dispatcher
	// level.query_assets - 多维资产查询
	static void Handle_QueryAssets(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
