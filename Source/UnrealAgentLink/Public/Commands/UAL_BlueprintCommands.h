#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 蓝图命令处理器
 * 包含: blueprint.create, blueprint.add_component, blueprint.set_property
 * 
 * 对应文档: 蓝图开发接口文档.md
 */
class FUAL_BlueprintCommands
{
public:
	/**
	 * 注册所有蓝图相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// Public Handlers called by Dispatcher
	// blueprint.create - 创建蓝图
	static void Handle_CreateBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// blueprint.add_component - 为已存在的蓝图添加组件
	static void Handle_AddComponentToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// blueprint.set_property - 设置蓝图属性（支持 CDO 和 SCS 组件）
	static void Handle_SetBlueprintProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
