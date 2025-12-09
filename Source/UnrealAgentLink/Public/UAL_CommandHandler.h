#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 解析 JSON 指令并在 GameThread 执行
 */
class FUAL_CommandHandler
{
public:
	FUAL_CommandHandler();

	// 必须在 GameThread 调用
	void ProcessMessage(const FString& JsonPayload);

	// 构建项目信息（GameThread 调用）
	TSharedPtr<FJsonObject> BuildProjectInfo() const;

private:
	using FHandlerFunc = TFunction<void(const TSharedPtr<FJsonObject>& /*Payload*/, const FString /*RequestId*/)> ;

	void RegisterCommands();

	// 指令实现
	void Handle_RunPython(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	void Handle_ExecConsole(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	void Handle_GetProjectInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	void Handle_SpawnActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	void Handle_SpawnActorsBatch(const TSharedPtr<FJsonObject>& Payload, const FString RequestId); // 批量
	void Handle_DestroyActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	void Handle_DestroyActorsBatch(const TSharedPtr<FJsonObject>& Payload, const FString RequestId); // 批量
	void Handle_SetTransformUnified(const TSharedPtr<FJsonObject>& Payload, const FString RequestId); // 兼容单体与批量
	void Handle_SetActorTransform(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);   // 单体
	void Handle_GetActorInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);       // 新版：全能感知
	void Handle_GetActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	void Handle_InspectActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	void Handle_SetProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);        // 通用属性修改
	void Handle_FindActors(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);
	void Handle_TransformActors(const TSharedPtr<FJsonObject>& Payload, const FString RequestId); // actor.transformr

	// Blueprint
	void Handle_CreateBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId);

	// 辅助函数：单体生成（不发送响应，只执行逻辑并返回结果 Json Object）
	TSharedPtr<FJsonObject> SpawnSingleActor(const TSharedPtr<FJsonObject>& Item);
	// 辅助函数：单体删除
	bool DestroySingleActor(const FString& Name, const FString& Path);

	// 响应处理（用于弹出通知）
	void Handle_Response(const FString& Method, const TSharedPtr<FJsonObject>& Payload);

	// Response / Event 辅助
	void SendResponse(const FString& RequestId, int32 Code, const TSharedPtr<FJsonObject>& Data = nullptr);
	void SendError(const FString& RequestId, int32 Code, const FString& Message);

private:
	TMap<FString, FHandlerFunc> CommandMap;
};
