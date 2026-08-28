#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;

/**
 * 蓝图命令处理器
 * 包含: blueprint.describe, blueprint.create, blueprint.add_component, blueprint.set_property, blueprint.compile
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

	// ============================================================================
	// 命令处理函数
	// ============================================================================
	
	// blueprint.describe - 获取蓝图完整结构信息（组件、变量等）
	static void Handle_DescribeBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// blueprint.create - 创建蓝图
	static void Handle_CreateBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// blueprint.add_component - 为已存在的蓝图添加组件
	static void Handle_AddComponentToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// blueprint.set_property - 设置蓝图属性（支持 CDO 和 SCS 组件）
	static void Handle_SetBlueprintProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.add_variable - 添加蓝图成员变量
	static void Handle_AddVariableToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.get_graph - 获取蓝图图表（节点、引脚等）
	static void Handle_GetBlueprintGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	static void Handle_ListBlueprintGraphs(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.add_node - 在图表中添加节点
	static void Handle_AddNodeToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.add_timeline - 在图表中添加 Timeline 节点（Timeline 不是普通变量/节点，需要创建 TimelineTemplate）
	static void Handle_AddTimelineToBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.connect_pins - 连接两个节点的引脚
	static void Handle_ConnectBlueprintPins(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.create_function - 创建蓝图函数图表（可选定义输入输出参数）
	static void Handle_CreateFunctionGraph(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.compile - 编译蓝图并可选保存
	static void Handle_CompileBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.set_pin_value - 设置节点 Pin 的默认值
	static void Handle_SetPinValue(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// blueprint.delete_node - 删除图表中的节点
	static void Handle_DeleteNode(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * blueprint.create_graph - 声明式蓝图图表创建（原子操作）
	 * 
	 * 一次性创建完整的蓝图图表，包括所有节点和连线。
	 * 这是一个原子操作：要么全部成功，要么全部失败。
	 * 
	 * 参数格式：
	 * {
	 *   "blueprint_path": "/Game/Blueprints/BP_Test",
	 *   "graph_name": "EventGraph",  // 可选，默认 EventGraph
	 *   "clear_existing": false,      // 可选，是否清除现有节点
	 *   "nodes": [
	 *     { "id": "node1", "type": "Event", "name": "BeginPlay" },
	 *     { "id": "node2", "type": "Function", "name": "KismetSystemLibrary.PrintString" }
	 *   ],
	 *   "connections": [
	 *     ["node1.Then", "node2.execute"]
	 *   ],
	 *   "pin_values": {  // 可选，设置 Pin 默认值
	 *     "node2.InString": "Hello World"
	 *   },
	 *   "auto_layout": true,  // 可选，自动布局
	 *   "compile": true       // 可选，完成后编译
	 * }
	 */
	static void Handle_CreateGraphDeclarative(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// ============================================================================
	// 辅助函数
	// ============================================================================
	
	/**
	 * 构建蓝图结构 JSON 对象
	 * 包含：基本信息、所有组件（SCS + 继承）、变量列表、编译状态
	 * 被 describe、create、add_component 复用
	 * 
	 * @param Blueprint 蓝图对象
	 * @param bIncludeVariables 是否包含变量列表（默认 true）
	 * @param bIncludeComponentDetails 是否包含组件详细属性（默认 false，仅返回基本信息）
	 * @return JSON 对象
	 */
	static TSharedPtr<FJsonObject> BuildBlueprintStructureJson(
		UBlueprint* Blueprint, 
		bool bIncludeVariables = true, 
		bool bIncludeComponentDetails = false
	);

private:
	/**
	 * 收集蓝图的所有组件信息（包括 SCS 添加的和继承的）
	 * @param Blueprint 蓝图对象
	 * @return 组件信息数组
	 */
	static TArray<TSharedPtr<FJsonValue>> CollectComponentsInfo(UBlueprint* Blueprint);
	
	/**
	 * 收集蓝图的变量列表
	 * @param Blueprint 蓝图对象
	 * @return 变量信息数组
	 */
	static TArray<TSharedPtr<FJsonValue>> CollectVariablesInfo(UBlueprint* Blueprint);
};
