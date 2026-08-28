#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Actor 相关命令处理器
 * 包含: actor.spawn, actor.destroy, actor.set_transform, actor.get_info, actor.inspect, actor.set_property
 * 
 * 对应文档: Actor接口文档.md
 */
class FUAL_ActorCommands
{
public:
	/**
	 * 注册所有 Actor 相关命令到 CommandMap
	 * @param CommandMap 命令映射表
	 */
	static void RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap);

	// Public Handlers called by Dispatcher
	// actor.spawn - 全能生成 v2.0
	static void Handle_SpawnActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// actor.spawn_batch - 批量生成（兼容）
	static void Handle_SpawnActorsBatch(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// actor.destroy - 统一 Selector 删除 v2.0
	static void Handle_DestroyActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// actor.destroy_batch - 批量删除（兼容）
	static void Handle_DestroyActorsBatch(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// actor.set_transform - 统一变换接口
	static void Handle_SetTransformUnified(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// actor.get_info - 场景感知 v2.0
	static void Handle_GetActorInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// actor.get - 旧版兼容
	static void Handle_GetActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// actor.inspect - 按需内省 v2.0
	static void Handle_InspectActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	
	// actor.set_property - 通用属性修改 v1.0
	static void Handle_SetProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

private:
	// ========== 内部辅助函数 ==========
	
	// 单体生成（不发送响应，只执行逻辑并返回结果 Json Object）
	static TSharedPtr<FJsonObject> SpawnSingleActor(const TSharedPtr<FJsonObject>& Item);
	
	// 单体删除
	static bool DestroySingleActor(const FString& Name, const FString& Path);
};
