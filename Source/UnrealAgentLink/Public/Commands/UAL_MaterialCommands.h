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
	
	// ========================================================================
	// Phase 1: 材质图表编辑命令
	// ========================================================================
	
	/**
	 * material.get_graph - 获取材质图表结构
	 * 
	 * 使用场景：
	 * 1. 查看材质中所有表达式节点
	 * 2. 获取节点的引脚和连接关系
	 * 
	 * 请求参数：
	 * - path: 材质资产路径（必填）
	 * - include_values: 是否包含节点当前值（可选，默认 true）
	 * 
	 * 响应数据：
	 * - nodes: 节点列表（node_id, class, pins, position 等）
	 * - connections: 连接列表（from -> to）
	 * - material_pins: 材质主节点可用引脚
	 */
	static void Handle_GetMaterialGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.add_node - 添加材质表达式节点
	 * 
	 * 使用场景：
	 * 1. 添加 TextureSample、Constant、Math 等材质节点
	 * 2. 创建参数节点（ScalarParameter、VectorParameter）
	 * 
	 * 请求参数：
	 * - material_path: 材质资产路径（必填）
	 * - node_type: 节点类型（必填）
	 * - node_name: 参数名称（参数节点时使用）
	 * - position: 节点位置（可选）
	 * - initial_value: 初始值（可选）
	 * - texture_path: 贴图路径（TextureSample 时使用）
	 * 
	 * 响应数据：
	 * - node_id: 节点唯一标识
	 * - pins: 引脚列表
	 */
	static void Handle_AddMaterialNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.connect_pins - 连接材质节点引脚
	 * 
	 * 使用场景：
	 * 1. 连接节点输出到材质主节点
	 * 2. 连接节点之间的引脚
	 * 
	 * 请求参数：
	 * - material_path: 材质资产路径（必填）
	 * - source_node: 源节点 ID（必填）
	 * - source_pin: 源引脚名称（必填）
	 * - target_node: 目标节点 ID（必填）
	 * - target_pin: 目标引脚名称（必填）
	 * 
	 * 响应数据：
	 * - connection: 连接信息
	 */
	static void Handle_ConnectMaterialPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.compile - 编译材质
	 * 
	 * 使用场景：
	 * 1. 验证材质图表的正确性
	 * 2. 生成 Shader 并获取编译错误
	 * 
	 * 请求参数：
	 * - path: 材质资产路径（必填）
	 * - force_recompile: 是否强制重新编译（可选）
	 * 
	 * 响应数据：
	 * - compiled: 是否编译成功
	 * - errors: 错误列表
	 * - warnings: 警告列表
	 */
	static void Handle_CompileMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.set_node_value - 设置材质节点值
	 * 
	 * 使用场景：
	 * 1. 修改 Constant 节点的数值
	 * 2. 设置参数节点的默认值
	 * 3. 更换 TextureSample 节点的贴图
	 * 
	 * 请求参数：
	 * - material_path: 材质资产路径（必填）
	 * - node_id: 节点 ID（必填）
	 * - value: 要设置的值（必填）
	 * - property_name: 属性名称（可选）
	 * 
	 * 响应数据：
	 * - node_id: 节点 ID
	 * - old_value: 修改前的值
	 * - new_value: 修改后的值
	 */
	static void Handle_SetMaterialNodeValue(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.delete_node - 删除材质节点
	 * 
	 * 使用场景：
	 * 1. 删除不需要的材质节点
	 * 2. 清理材质图表
	 * 
	 * 请求参数：
	 * - material_path: 材质资产路径（必填）
	 * - node_id: 要删除的节点 ID（必填）
	 * - disconnect_first: 是否先断开连接（可选，默认 true）
	 * 
	 * 响应数据：
	 * - node_id: 被删除的节点 ID
	 * - disconnected_count: 断开的连接数量
	 */
	static void Handle_DeleteMaterialNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// ========================================================================
	// Phase 2: 材质管理命令（智能容错）
	// ========================================================================
	
	/**
	 * material.duplicate - 复制材质
	 * 
	 * 智能容错特性：
	 * - 路径自动补全（省略 /Game/ 时自动添加）
	 * - 模糊路径匹配（返回相似资产建议）
	 * - 名称冲突自动处理
	 * 
	 * 请求参数：
	 * - source_path: 源材质路径（支持简写）
	 * - new_name: 新材质名称（可选）
	 * - destination_path: 目标路径（可选）
	 * 
	 * 响应数据：
	 * - new_path: 新材质的完整路径
	 * - suggestions: 如失败，返回修复建议
	 * - similar_assets: 相似资产列表（路径错误时）
	 */
	static void Handle_DuplicateMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.set_property - 设置材质属性
	 * 
	 * 智能容错特性：
	 * - 属性值多格式支持（枚举名/中文/数字索引）
	 * - 大小写不敏感
	 * - 失败时返回有效值列表
	 * 
	 * 请求参数：
	 * - path: 材质资产路径
	 * - properties: 属性键值对
	 *   - blend_mode: Opaque/Masked/Translucent/Additive（或 不透明/半透明）
	 *   - shading_model: DefaultLit/Unlit/Subsurface（或 默认/无光照）
	 *   - two_sided: 是否双面
	 * 
	 * 响应数据：
	 * - updated_properties: 成功更新的属性
	 * - failed_properties: 失败的属性及修复建议
	 * - current_state: 当前材质状态
	 */
	static void Handle_SetMaterialProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.create_instance - 创建材质实例
	 * 
	 * 智能容错特性：
	 * - 父材质路径自动补全
	 * - 实例名称自动生成
	 * - 返回可用参数列表
	 * 
	 * 请求参数：
	 * - parent_path: 父材质路径
	 * - instance_name: 实例名称（可选）
	 * - destination_path: 保存路径（可选）
	 * - initial_params: 初始参数值（可选）
	 * 
	 * 响应数据：
	 * - instance_path: 新实例路径
	 * - available_params: 可用参数列表
	 * - suggestions: 如失败，返回修复建议
	 */
	static void Handle_CreateMaterialInstance(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

private:
	// ========================================================================
	// 智能容错辅助函数
	// ========================================================================
	
	/**
	 * 标准化资产路径（智能补全）
	 * - 自动添加 /Game/ 前缀（如果缺失）
	 * - 移除多余的斜杠
	 * - 处理文件扩展名
	 */
	static FString NormalizePath(const FString& InputPath, const FString& DefaultPrefix = TEXT("/Game/Materials"));
	
	/**
	 * 查找相似资产（用于路径错误时的建议）
	 * @return 相似资产路径列表（最多5个）
	 */
	static TArray<FString> FindSimilarAssets(const FString& PartialPath, const FString& AssetClass = TEXT("MaterialInterface"));
	
	/**
	 * 解析 BlendMode（支持多种格式）
	 * @param Value 输入值（字符串或数字）
	 * @param OutMode 输出的 BlendMode
	 * @return 是否解析成功
	 */
	static bool ParseBlendMode(const FString& Value, EBlendMode& OutMode);
	
	/**
	 * 解析 ShadingModel（支持多种格式）
	 * @param Value 输入值
	 * @param OutModel 输出的 ShadingModel  
	 * @return 是否解析成功
	 */
	static bool ParseShadingModel(const FString& Value, EMaterialShadingModel& OutModel);
	
	/**
	 * 获取 BlendMode 的有效值列表（用于错误提示）
	 */
	static TArray<FString> GetValidBlendModes();
	
	/**
	 * 获取 ShadingModel 的有效值列表
	 */
	static TArray<FString> GetValidShadingModels();
	
	/**
	 * 将初始值应用到材质节点
	 * @param Expression 材质表达式节点
	 * @param InitialValueObj 初始值对象
	 */
	static void ApplyInitialValueToNode(UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& InitialValueObj);
	
	// ========================================================================
	// Phase 3: 材质查询和预览命令
	// ========================================================================
	
	/**
	 * material.list - 列出材质资产
	 * 
	 * 请求参数：
	 * - search_path: 搜索路径（可选，默认 /Game）
	 * - name_filter: 名称过滤器（可选，支持 * 通配符）
	 * - material_type: 类型过滤（all/material/instance）
	 * - max_results: 最大返回数量（可选，默认50）
	 * 
	 * 响应数据：
	 * - materials: 材质列表
	 * - total_count: 总数量
	 */
	static void Handle_ListMaterials(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * material.preview - 预览材质信息
	 * 
	 * 请求参数：
	 * - path: 材质路径
	 * - preview_type: 预览类型（info/thumbnail）
	 * - include_graph_summary: 是否包含图表摘要
	 * 
	 * 响应数据：
	 * - material_name, material_type, blend_mode, shading_model
	 * - node_count, texture_count, parameter_count
	 */
	static void Handle_PreviewMaterial(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
};
