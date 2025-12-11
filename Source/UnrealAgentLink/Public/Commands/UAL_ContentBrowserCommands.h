#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 内容浏览器命令处理器
 * 管理 UE 编辑器内的文件与文件夹结构
 * 
 * 包含 4 个原子工具（CRUD 操作）:
 * - content.search  : 搜索资产路径、类名
 * - content.import  : 导入外部文件 (FBX/PNG/WAV 等)
 * - content.move    : 移动/重命名资产
 * - content.delete  : 删除资产/文件夹
 * 
 * 对应文档: 内容管理文档.md
 */
class FUAL_ContentBrowserCommands
{
public:
	/**
	 * 注册所有内容浏览器相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// ========================================================================
	// Public Handlers (由 Dispatcher 调用)
	// ========================================================================
	
	/**
	 * content.search - 搜索资产
	 * 在 Content Browser 中查找匹配的资产路径
	 * 
	 * @param Payload 请求参数 (query, filter_class, limit)
	 * @param RequestId 请求 ID
	 */
	static void Handle_SearchAssets(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * content.import - 导入外部文件
	 * 将磁盘上的文件导入到 UE 项目中
	 * 
	 * @param Payload 请求参数 (files, destination_path, overwrite)
	 * @param RequestId 请求 ID
	 */
	static void Handle_ImportAssets(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * content.move - 移动/重命名资产
	 * 移动资产或通过修改目标路径实现重命名
	 * 
	 * @param Payload 请求参数 (source_path, destination_path)
	 * @param RequestId 请求 ID
	 */
	static void Handle_MoveAsset(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * content.delete - 删除资产
	 * 彻底删除资产或文件夹
	 * 
	 * @param Payload 请求参数 (paths)
	 * @param RequestId 请求 ID
	 */
	static void Handle_DeleteAssets(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
