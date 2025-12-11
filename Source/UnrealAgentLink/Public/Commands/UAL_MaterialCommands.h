#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 材质命令处理器
 * 实现 PBR 材质自动化流程：从贴图创建材质、应用材质到 Actor
 * 
 * 包含 4 个原子工具：
 * - material.create   : 从贴图创建 Material Instance
 * - material.apply    : 将材质应用到场景中的 Actor
 * - material.describe : 查询材质的参数和贴图槽信息
 * - material.set_param: 设置材质参数（颜色、标量、向量等）
 * 
 * 核心功能：
 * 1. 智能识别贴图类型（Albedo/Normal/Roughness 等）
 * 2. 自动分组同一资产的多张贴图
 * 3. 创建 PBR Material Instance 并连接贴图
 * 4. 应用材质到 StaticMeshComponent 的材质槽
 */
class FUAL_MaterialCommands
{
public:
	/**
	 * 注册所有材质相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// ========================================================================
	// Public Handlers (由 Dispatcher 调用)
	// ========================================================================
	
	/**
	 * material.create - 从贴图创建 PBR 材质
	 * 
	 * 使用场景：
	 * 1. 用户导入了多张 PBR 贴图（如 T_Wood_D.png, T_Wood_N.png）
	 * 2. Agent 调用此命令自动创建材质并连接贴图
	 * 
	 * 请求参数：
	 * - texture_paths: 贴图资产路径列表（必填）
	 *   例: ["/Game/Textures/T_Wood_D", "/Game/Textures/T_Wood_N"]
	 * - material_name: 输出材质名称（可选，自动生成）
	 * - destination_path: 输出材质路径（可选，默认与贴图同目录）
	 * - parent_material: 父材质路径（可选，默认使用 M_MasterPBR）
	 * 
	 * 响应数据：
	 * - material_path: 创建的材质路径
	 * - material_name: 材质名称
	 * - texture_bindings: 贴图槽绑定信息
	 *   例: { "BaseColor": "/Game/Textures/T_Wood_D", "Normal": "/Game/Textures/T_Wood_N" }
	 * 
	 * @param Payload 请求参数
	 * @param RequestId 请求 ID
	 */
	static void Handle_CreateMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.apply - 将材质应用到 Actor
	 * 
	 * 使用场景：
	 * 1. 用户创建了材质，想应用到场景中的某个物体
	 * 2. Agent 调用此命令将材质设置到指定 Actor 的材质槽
	 * 
	 * 请求参数：
	 * - targets: Actor 选择器（统一格式：names/paths/filter）
	 * - material_path: 材质资产路径（必填）
	 * - slot_index: 材质槽索引（可选，默认 0）
	 * - slot_name: 材质槽名称（可选，与 slot_index 二选一）
	 * 
	 * 响应数据：
	 * - applied_count: 成功应用的 Actor 数量
	 * - actors: 受影响的 Actor 信息列表
	 * 
	 * @param Payload 请求参数
	 * @param RequestId 请求 ID
	 */
	static void Handle_ApplyMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.describe - 获取材质详细信息
	 * 
	 * 使用场景：
	 * 1. Agent 需要了解材质有哪些参数可以调整
	 * 2. 检查材质当前使用了哪些贴图
	 * 
	 * 请求参数：
	 * - path: 材质资产路径（必填）
	 * - include_parent_params: 是否包含父材质参数（可选，默认 true）
	 * 
	 * 响应数据：
	 * - name: 材质名称
	 * - path: 材质路径
	 * - class: 材质类型（Material/MaterialInstance）
	 * - parent_material: 父材质路径（如果是 Instance）
	 * - scalar_params: 标量参数列表
	 * - vector_params: 向量参数列表 
	 * - texture_params: 贴图参数列表
	 * 
	 * @param Payload 请求参数
	 * @param RequestId 请求 ID
	 */
	static void Handle_DescribeMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.set_param - 设置材质参数
	 * 
	 * 使用场景：
	 * 1. 调整材质的颜色、金属度、粗糙度等参数
	 * 2. 更换材质使用的贴图
	 * 
	 * 请求参数：
	 * - path: 材质资产路径（必填，必须是 MaterialInstanceConstant）
	 * - params: 参数键值对
	 *   - 标量: { "Roughness": 0.5, "Metallic": 1.0 }
	 *   - 向量/颜色: { "BaseColor": { "r": 1, "g": 0, "b": 0 } }
	 *   - 贴图: { "NormalMap": "/Game/Textures/T_Normal" }
	 * 
	 * 响应数据：
	 * - updated_params: 成功更新的参数列表
	 * - errors: 失败的参数及原因
	 * 
	 * @param Payload 请求参数
	 * @param RequestId 请求 ID
	 */
	static void Handle_SetMaterialParam(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
