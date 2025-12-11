#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 系统命令处理器
 * 包含: system.run_console_command, system.get_performance_stats, cmd.run_python, cmd.exec_console
 * 
 * 对应文档: 系统工具接口文档.md
 */
class FUAL_SystemCommands
{
public:
	/**
	 * 注册所有系统相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// Public Handlers called by Dispatcher
	// cmd.run_python - 执行 Python 脚本
	static void Handle_RunPython(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// cmd.exec_console / system.run_console_command - 执行控制台指令
	static void Handle_ExecConsole(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// system.get_performance_stats - 获取性能统计
	static void Handle_GetPerformanceStats(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// system.manage_plugin - 查询或修改插件状态
	static void Handle_ManagePlugin(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
