#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 内容浏览器命令处理器
 * 管理 UE 编辑器内的文件与文件夹结构
 * 
 * 包含 5 个原子工具（CRUD + Describe）:
 * - content.search   : 搜索资产路径、类名
 * - content.import   : 导入外部文件 (FBX/PNG/WAV 等)
 * - content.move     : 移动/重命名资产
 * - content.delete   : 删除资产/文件夹
 * - content.describe : 获取资产详情（含依赖和被引用关系）
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
	
	/**
	 * content.describe - 获取资产详情
	 * 返回资产的完整信息，包括依赖项和被引用项
	 * 
	 * @param Payload 请求参数 (path, include_dependencies, include_referencers)
	 * @param RequestId 请求 ID
	 */
	static void Handle_DescribeAsset(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * content.normalized_import - 规范化导入 uasset/umap 资产
	 * 将外部工程的资产导入到规范化的目录结构中
	 * 自动处理依赖闭包、包名重映射和引用修复
	 * 
	 * @param Payload 请求参数:
	 *   - files: 要导入的文件路径列表
	 *   - target_root: 可选，目标根目录（默认 /Game/Imported）
	 *   - use_pascal_case: 可选，是否使用 PascalCase（默认 true）
	 *   - auto_rename_on_conflict: 可选，冲突时是否自动重命名（默认 true）
	 * @param RequestId 请求 ID
	 */
	static void Handle_NormalizedImport(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
