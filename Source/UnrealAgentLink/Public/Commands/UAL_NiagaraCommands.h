#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Niagara 粒子系统命令处理器
 * 实现 Niagara 系统的创建、查看、Emitter 管理、参数设置和场景放置
 *
 * 包含 7 个命令：
 * - niagara.create_system      : 创建 NiagaraSystem 资产
 * - niagara.describe_system    : 查看 System 结构（Emitter、参数、Renderer）
 * - niagara.add_emitter        : 向 System 添加 Emitter
 * - niagara.remove_emitter     : 从 System 移除 Emitter
 * - niagara.set_emitter_enabled: 启用/禁用 Emitter
 * - niagara.set_param          : 设置运行时 User Parameter Override
 * - niagara.spawn              : 在场景中放置 NiagaraSystem Actor
 */
class FUAL_NiagaraCommands
{
public:
	/**
	 * 注册所有 Niagara 相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// ========================================================================
	// 命令处理函数
	// ========================================================================

	/**
	 * niagara.create_system - 创建 NiagaraSystem 资产
	 *
	 * 请求参数：
	 * - system_name: 资产名称（必填）
	 * - destination_path: 保存路径（可选，默认 /Game/FX）
	 *
	 * 响应数据：
	 * - system_name: 创建的系统名称
	 * - system_path: 创建的系统路径
	 */
	static void Handle_CreateSystem(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * niagara.describe_system - 查看 NiagaraSystem 结构
	 *
	 * 请求参数：
	 * - path: NiagaraSystem 资产路径（必填）
	 *
	 * 响应数据：
	 * - name, path, class
	 * - emitters: Emitter 列表（name, enabled, sim_target, renderer_count）
	 * - user_parameters: User 参数列表（name, type）
	 */
	static void Handle_DescribeSystem(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * niagara.add_emitter - 向 System 添加 Emitter
	 *
	 * 请求参数：
	 * - system_path: 目标 NiagaraSystem 路径（必填）
	 * - emitter_path: 源 Emitter 或模板 System 路径（必填）
	 *
	 * 响应数据：
	 * - system_path, emitter_name, emitter_index
	 */
	static void Handle_AddEmitter(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * niagara.remove_emitter - 从 System 移除 Emitter
	 *
	 * 请求参数：
	 * - system_path: 目标 NiagaraSystem 路径（必填）
	 * - emitter_name: Emitter 名称（必填）
	 *
	 * 响应数据：
	 * - system_path, removed_emitter
	 */
	static void Handle_RemoveEmitter(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * niagara.set_emitter_enabled - 启用/禁用 Emitter
	 *
	 * 请求参数：
	 * - system_path: 目标 NiagaraSystem 路径（必填）
	 * - emitter_name: Emitter 名称（必填）
	 * - enabled: 是否启用（必填）
	 *
	 * 响应数据：
	 * - system_path, emitter_name, enabled
	 */
	static void Handle_SetEmitterEnabled(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * niagara.set_param - 设置运行时 User Parameter Override
	 *
	 * 请求参数：
	 * - target: Actor Label / Actor Path / Component Path（必填）
	 * - param_name: 参数名（必填）
	 * - param_type: 参数类型：float, int, bool, vector, color（必填）
	 * - value: 参数值（必填）
	 *
	 * 响应数据：
	 * - target, param_name, param_type, value
	 */
	static void Handle_SetParam(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	/**
	 * niagara.spawn - 在场景中放置 NiagaraSystem Actor
	 *
	 * 请求参数：
	 * - system_path: NiagaraSystem 资产路径（必填）
	 * - location: 位置 {x, y, z}（可选，默认原点）
	 * - rotation: 旋转 {pitch, yaw, roll}（可选）
	 * - scale: 缩放 {x, y, z}（可选，默认 1,1,1）
	 * - auto_activate: 是否自动激活（可选，默认 true）
	 * - label: 自定义 Actor 标签（可选）
	 *
	 * 响应数据（协议字段 — 供 set_param 引用）：
	 * - system_path: NiagaraSystem 资产路径
	 * - actor_path: 场景中 Actor 的路径
	 * - actor_label: Actor 的显示标签
	 * - component_path: NiagaraComponent 的完整路径
	 */
	static void Handle_Spawn(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

private:
	/**
	 * 在场景中查找拥有 NiagaraComponent 的 Actor（按标签、路径或组件路径）
	 * @param Target 搜索目标（actor_label / actor_path / component_path）
	 * @param OutComponent 找到的 NiagaraComponent
	 * @param OutError 错误信息
	 * @return 是否成功找到
	 */
	static bool FindNiagaraComponentByTarget(const FString& Target, class UNiagaraComponent*& OutComponent, FString& OutError);
};
