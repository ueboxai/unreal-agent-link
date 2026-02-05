// Copyright Epic Games, Inc. All Rights Reserved.
// UnrealAgentLink - Widget Commands

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * UMG Widget 命令处理器
 * 管理 Widget Blueprint 的创建、查询和修改
 * 
 * Phase 1 工具（只读）:
 * - widget.get_hierarchy : 获取 Widget 层级结构（含 Slot 类型识别）
 * 
 * Phase 2 工具（创建 + CanvasPanel 布局）:
 * - widget.create       : 创建 Widget Blueprint
 * - widget.add_control  : 添加控件到 CanvasPanel
 * - widget.set_canvas_slot : 设置 CanvasPanel 子控件的 Slot 属性
 * 
 * Phase 3 工具（其他布局容器）:
 * - widget.add_to_vertical   : 添加控件到 VerticalBox
 * - widget.add_to_horizontal : 添加控件到 HorizontalBox
 * - widget.set_vertical_slot : 设置 VerticalBox 子控件的 Slot 属性
 * 
 * Phase 4 工具（预览渲染）:
 * - widget.preview : 渲染预览截图
 */
class FUAL_WidgetCommands
{
public:
	/**
	 * 注册所有 Widget 相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// ========================================================================
	// Phase 1: 只读能力
	// ========================================================================
	
	/**
	 * widget.get_hierarchy - 获取 Widget 层级结构
	 * 返回 Widget 树的完整结构，包含每个控件的 Slot 类型和属性
	 * 
	 * @param Payload 请求参数:
	 *   - path: Widget Blueprint 路径，如 /Game/UI/WBP_MainMenu
	 * @param RequestId 请求 ID
	 * 
	 * @return JSON 结构:
	 *   - root: 根控件对象
	 *     - name: 控件名称
	 *     - class: 控件类型（Button, Text, CanvasPanel 等）
	 *     - slot_type: Slot 类型（CanvasPanelSlot, VerticalBoxSlot 等）
	 *     - slot_data: Slot 属性（根据类型不同包含不同字段）
	 *     - children: 子控件数组（如果是容器控件）
	 */
	static void Handle_GetHierarchy(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// ========================================================================
	// Phase 2: 创建 + CanvasPanel 布局
	// ========================================================================
	
	/**
	 * widget.create - 创建 Widget Blueprint
	 * 
	 * @param Payload 请求参数:
	 *   - name: Widget 名称（必填）
	 *   - folder: 保存目录（可选，默认 /Game/UI）
	 *   - root_type: 根控件类型（可选，默认 CanvasPanel）
	 * @param RequestId 请求 ID
	 */
	static void Handle_Create(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * widget.add_control - 添加控件到 CanvasPanel
	 * 
	 * @param Payload 请求参数:
	 *   - path: Widget Blueprint 路径（必填）
	 *   - control_type: 控件类型，如 Button, Text, Image（必填）
	 *   - name: 控件名称（可选）
	 *   - parent: 父控件名称（可选，默认根控件）
	 *   - anchors: 锚点预设，如 TopLeft, Center, Stretch（可选）
	 *   - position: { x, y } 位置（可选）
	 *   - size: { width, height } 尺寸（可选）
	 * @param RequestId 请求 ID
	 */
	static void Handle_AddControl(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * widget.set_canvas_slot - 设置 CanvasPanel 子控件的 Slot 属性
	 * 
	 * @param Payload 请求参数:
	 *   - path: Widget Blueprint 路径（必填）
	 *   - widget_name: 控件名称（必填）
	 *   - anchors: 锚点预设（可选）
	 *   - position: { x, y } 位置（可选）
	 *   - size: { width, height } 尺寸（可选）
	 *   - alignment: { x, y } 对齐（可选）
	 *   - z_order: 层级（可选）
	 * @param RequestId 请求 ID
	 */
	static void Handle_SetCanvasSlot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// ========================================================================
	// Phase 3: 通用子控件添加（自动检测父容器类型）
	// ========================================================================
	
	/**
	 * widget.add_child - 向任意容器添加子控件（通用命令）
	 * 自动检测父容器类型并使用正确的添加方式
	 * 
	 * 支持的父容器类型:
	 * - CanvasPanel: 使用 anchors/position/size
	 * - VerticalBox/HorizontalBox: 使用 size_rule/padding/alignment
	 * - Overlay: 使用 h_align/v_align/padding
	 * - Button/Border/SizeBox: 单内容容器，设置为 Content
	 * 
	 * @param Payload 请求参数:
	 *   - path: Widget Blueprint 路径（必填）
	 *   - parent_name: 父容器名称（必填，根容器用 "root"）
	 *   - control_type: 控件类型（必填）
	 *   - name: 控件名称（可选）
	 *   - 布局参数（根据父容器类型自动选用）:
	 *     - CanvasPanel: anchors, position, size
	 *     - VerticalBox/HorizontalBox: size_rule, padding, h_align, v_align
	 *     - Overlay: h_align, v_align, padding
	 *   - text: 当 control_type=TextBlock 时的文本内容（可选）
	 * @param RequestId 请求 ID
	 */
	static void Handle_AddChild(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// 保留旧命令用于向后兼容（内部调用 Handle_AddChild）
	static void Handle_AddToVertical(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	static void Handle_AddToHorizontal(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	static void Handle_SetVerticalSlot(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// ========================================================================
	// Phase 4: 预览渲染
	// ========================================================================
	
	/**
	 * widget.preview - 渲染预览截图
	 * 
	 * @param Payload 请求参数:
	 *   - path: Widget Blueprint 路径（必填）
	 *   - width: 渲染宽度（可选，默认 1920）
	 *   - height: 渲染高度（可选，默认 1080）
	 * @param RequestId 请求 ID
	 */
	static void Handle_Preview(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// ========================================================================
	// Phase 5: 事件绑定
	// ========================================================================
	
	/**
	 * widget.make_variable - 将控件设置为蓝图变量
	 * 这是事件绑定的前置条件，控件必须是变量才能在事件图中引用
	 * 
	 * @param Payload 请求参数:
	 *   - path: Widget Blueprint 路径（必填）
	 *   - widget_name: 控件名称（必填）
	 *   - variable_name: 变量名称（可选，默认使用控件名）
	 * @param RequestId 请求 ID
	 */
	static void Handle_MakeVariable(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	/**
	 * widget.set_property - 设置控件属性
	 * 支持设置常见属性如文本、颜色、可见性等
	 * 
	 * @param Payload 请求参数:
	 *   - path: Widget Blueprint 路径（必填）
	 *   - widget_name: 控件名称（必填）
	 *   - property_name: 属性名称（必填）
	 *   - value: 属性值（必填）
	 * @param RequestId 请求 ID
	 */
	static void Handle_SetProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

private:
	// ========================================================================
	// 辅助函数
	// ========================================================================
	
	/**
	 * 加载 Widget Blueprint
	 */
	static class UWidgetBlueprint* LoadWidgetBlueprint(const FString& Path, FString& OutError);
	
	/**
	 * 根据名称在 Widget 树中查找控件
	 */
	static class UWidget* FindWidgetByName(class UWidgetTree* WidgetTree, const FString& WidgetName);
	
	/**
	 * 查找 Widget 类
	 */
	static UClass* FindWidgetClass(const FString& ClassName);
	
	/**
	 * 构建 Widget 的 JSON 表示（递归）
	 */
	static TSharedPtr<FJsonObject> BuildWidgetJson(class UWidget* Widget);
	
	/**
	 * 构建 CanvasPanelSlot 的 JSON
	 */
	static TSharedPtr<FJsonObject> BuildCanvasSlotJson(class UCanvasPanelSlot* Slot);
	
	/**
	 * 构建 VerticalBoxSlot 的 JSON
	 */
	static TSharedPtr<FJsonObject> BuildVerticalSlotJson(class UVerticalBoxSlot* Slot);
	
	/**
	 * 构建 HorizontalBoxSlot 的 JSON
	 */
	static TSharedPtr<FJsonObject> BuildHorizontalSlotJson(class UHorizontalBoxSlot* Slot);
	
	/**
	 * 解析锚点预设字符串
	 */
	static struct FAnchors ParseAnchors(const FString& AnchorsStr);
	
	/**
	 * 解析 Vector2D 从 JSON 对象
	 */
	static FVector2D ParseVector2D(const TSharedPtr<FJsonObject>& Obj);
};
