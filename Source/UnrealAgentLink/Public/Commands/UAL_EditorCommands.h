#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 编辑器命令处理器
 * 包含: editor.screenshot, take_screenshot, project.info, editor.get_project_info(兼容别名)
 * 
 * 对应文档: 编辑器工具接口文档.md
 */
class FUAL_EditorCommands
{
public:
	/**
	 * 注册所有编辑器相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// Public Handlers called by Dispatcher
	// editor.screenshot / take_screenshot - 抓取当前视口截图
	static void Handle_TakeScreenshot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// project.info - 获取项目信息
	static void Handle_GetProjectInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// project.get_config - 读取配置文件项
	static void Handle_GetConfig(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// project.set_config - 设置配置文件项
	static void Handle_SetConfig(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// project.analyze_uproject - 分析 .uproject 文件，返回模块和插件信息
	static void Handle_AnalyzeUProject(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// 构建项目信息（公开给外部使用）
public:
	static TSharedPtr<FJsonObject> BuildProjectInfo();
};
