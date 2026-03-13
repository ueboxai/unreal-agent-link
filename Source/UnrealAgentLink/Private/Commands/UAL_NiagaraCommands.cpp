#include "UAL_NiagaraCommands.h"
#include "UAL_CommandUtils.h"

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraActor.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "UObject/SavePackage.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALNiagara, Log, All);

// ============================================================================
// RegisterCommands
// ============================================================================

void FUAL_NiagaraCommands::RegisterCommands(
	TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("niagara.create_system"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateSystem(Payload, RequestId);
	});

	CommandMap.Add(TEXT("niagara.describe_system"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DescribeSystem(Payload, RequestId);
	});

	CommandMap.Add(TEXT("niagara.add_emitter"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_AddEmitter(Payload, RequestId);
	});

	CommandMap.Add(TEXT("niagara.remove_emitter"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_RemoveEmitter(Payload, RequestId);
	});

	CommandMap.Add(TEXT("niagara.set_emitter_enabled"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetEmitterEnabled(Payload, RequestId);
	});

	CommandMap.Add(TEXT("niagara.set_param"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetParam(Payload, RequestId);
	});

	CommandMap.Add(TEXT("niagara.spawn"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_Spawn(Payload, RequestId);
	});
}

// ============================================================================
// niagara.create_system
// ============================================================================

void FUAL_NiagaraCommands::Handle_CreateSystem(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString SystemName = Payload->GetStringField(TEXT("system_name"));
	FString DestinationPath = Payload->HasField(TEXT("destination_path"))
		? Payload->GetStringField(TEXT("destination_path"))
		: TEXT("/Game/FX");

	if (SystemName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("system_name is required"));
		return;
	}

	// 确保以 NS_ 前缀命名
	if (!SystemName.StartsWith(TEXT("NS_")))
	{
		SystemName = TEXT("NS_") + SystemName;
	}

	// 构建包路径
	FString PackagePath = DestinationPath / SystemName;

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		UAL_CommandUtils::SendError(RequestId, 500, FString::Printf(TEXT("Failed to create package: %s"), *PackagePath));
		return;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Create Niagara System")));

	// 创建空 System（与 UNiagaraSystemFactoryNew::FactoryCreateNew L163-167 一致）
	UNiagaraSystem* NewSystem = NewObject<UNiagaraSystem>(
		Package, UNiagaraSystem::StaticClass(), FName(*SystemName),
		RF_Public | RF_Standalone | RF_Transactional
	);

	if (!NewSystem)
	{
		Transaction.Cancel();
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("NewObject<UNiagaraSystem> returned null"));
		return;
	}

	// 初始化系统（创建 SystemScriptSource / NodeGraph / EffectType）
	// 这是 NIAGARAEDITOR_API 导出的静态方法，进行必要的内部初始化
	UNiagaraSystemFactoryNew::InitializeSystem(NewSystem, true);

	// 编译
	NewSystem->RequestCompile(false);

	// 通知资产注册表
	FAssetRegistryModule::AssetCreated(NewSystem);
	NewSystem->MarkPackageDirty();

	// 返回
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("system_name"), SystemName);
	Data->SetStringField(TEXT("system_path"), NewSystem->GetPathName());
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);

	UE_LOG(LogUALNiagara, Log, TEXT("Created NiagaraSystem: %s"), *NewSystem->GetPathName());
}

// ============================================================================
// niagara.describe_system
// ============================================================================

void FUAL_NiagaraCommands::Handle_DescribeSystem(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Path = Payload->GetStringField(TEXT("path"));
	if (Path.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("path is required"));
		return;
	}

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *Path);
	if (!System)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("NiagaraSystem not found: %s"), *Path));
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("name"), System->GetName());
	Data->SetStringField(TEXT("path"), System->GetPathName());
	Data->SetStringField(TEXT("class"), System->GetClass()->GetName());

	// Emitter 列表
	TArray<TSharedPtr<FJsonValue>> EmitterArray;
	const TArray<FNiagaraEmitterHandle>& EmitterHandles = System->GetEmitterHandles();
	for (int32 i = 0; i < EmitterHandles.Num(); i++)
	{
		const FNiagaraEmitterHandle& Handle = EmitterHandles[i];
		TSharedPtr<FJsonObject> EmitterObj = MakeShareable(new FJsonObject());
		EmitterObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EmitterObj->SetNumberField(TEXT("index"), i);
		EmitterObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		EmitterObj->SetStringField(TEXT("id"), Handle.GetId().ToString());

		// Emitter Instance 信息
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetInstance().GetEmitterData();
		if (EmitterData)
		{
			EmitterObj->SetStringField(TEXT("sim_target"),
				EmitterData->SimTarget == ENiagaraSimTarget::CPUSim ? TEXT("CPU") : TEXT("GPU"));

			// Renderer 数量
			EmitterObj->SetNumberField(TEXT("renderer_count"), EmitterData->GetRenderers().Num());
		}

		EmitterArray.Add(MakeShareable(new FJsonValueObject(EmitterObj)));
	}
	Data->SetArrayField(TEXT("emitters"), EmitterArray);

	// User Parameters（ExposedParameters）
	TArray<TSharedPtr<FJsonValue>> ParamArray;
	const FNiagaraUserRedirectionParameterStore& UserParams = System->GetExposedParameters();
	TArray<FNiagaraVariable> Variables;
	UserParams.GetParameters(Variables);

	for (const FNiagaraVariable& Var : Variables)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShareable(new FJsonObject());
		ParamObj->SetStringField(TEXT("name"), Var.GetName().ToString());
		ParamObj->SetStringField(TEXT("type"), Var.GetType().GetName());
		ParamArray.Add(MakeShareable(new FJsonValueObject(ParamObj)));
	}
	Data->SetArrayField(TEXT("user_parameters"), ParamArray);

	Data->SetNumberField(TEXT("emitter_count"), EmitterHandles.Num());
	Data->SetNumberField(TEXT("parameter_count"), Variables.Num());

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// niagara.add_emitter
// ============================================================================

void FUAL_NiagaraCommands::Handle_AddEmitter(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString SystemPath = Payload->GetStringField(TEXT("system_path"));
	FString EmitterPath = Payload->GetStringField(TEXT("emitter_path"));

	if (SystemPath.IsEmpty() || EmitterPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("system_path and emitter_path are required"));
		return;
	}

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!System)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));
		return;
	}

	// 尝试加载为 NiagaraEmitter（独立 Emitter 资产）
	UNiagaraEmitter* SourceEmitter = LoadObject<UNiagaraEmitter>(nullptr, *EmitterPath);

	// 如果不是独立 Emitter，可能是另一个 System，从中提取第一个 Emitter
	if (!SourceEmitter)
	{
		UNiagaraSystem* SourceSystem = LoadObject<UNiagaraSystem>(nullptr, *EmitterPath);
		if (SourceSystem && SourceSystem->GetEmitterHandles().Num() > 0)
		{
			SourceEmitter = SourceSystem->GetEmitterHandles()[0].GetInstance().Emitter;
		}
	}

	if (!SourceEmitter)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("Emitter not found: %s"), *EmitterPath));
		return;
	}

	// 添加 Emitter 到 System
	FScopedTransaction Transaction(FText::FromString(TEXT("Add Emitter to Niagara System")));
	System->Modify();

	FNiagaraEmitterHandle NewHandle = System->AddEmitterHandle(*SourceEmitter, SourceEmitter->GetFName(), SourceEmitter->GetExposedVersion().VersionGuid);

	// 重新编译系统
	System->RequestCompile(false);
	System->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("system_path"), System->GetPathName());
	Data->SetStringField(TEXT("emitter_name"), NewHandle.GetName().ToString());
	Data->SetNumberField(TEXT("emitter_index"), System->GetEmitterHandles().Num() - 1);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);

	UE_LOG(LogUALNiagara, Log, TEXT("Added emitter '%s' to system '%s'"),
		*NewHandle.GetName().ToString(), *System->GetPathName());
}

// ============================================================================
// niagara.remove_emitter
// ============================================================================

void FUAL_NiagaraCommands::Handle_RemoveEmitter(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString SystemPath = Payload->GetStringField(TEXT("system_path"));
	FString EmitterName = Payload->GetStringField(TEXT("emitter_name"));

	if (SystemPath.IsEmpty() || EmitterName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("system_path and emitter_name are required"));
		return;
	}

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!System)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));
		return;
	}

	// 找到要删除的 Emitter Handle
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	TSet<FGuid> IdsToRemove;

	for (const FNiagaraEmitterHandle& Handle : Handles)
	{
		if (Handle.GetName().ToString() == EmitterName)
		{
			IdsToRemove.Add(Handle.GetId());
			break;
		}
	}

	if (IdsToRemove.Num() == 0)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("Emitter '%s' not found in system '%s'"), *EmitterName, *SystemPath));
		return;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("Remove Emitter from Niagara System")));
	System->Modify();
	System->RemoveEmitterHandlesById(IdsToRemove);
	System->RequestCompile(false);
	System->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("system_path"), System->GetPathName());
	Data->SetStringField(TEXT("removed_emitter"), EmitterName);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);

	UE_LOG(LogUALNiagara, Log, TEXT("Removed emitter '%s' from system '%s'"),
		*EmitterName, *System->GetPathName());
}

// ============================================================================
// niagara.set_emitter_enabled
// ============================================================================

void FUAL_NiagaraCommands::Handle_SetEmitterEnabled(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString SystemPath = Payload->GetStringField(TEXT("system_path"));
	FString EmitterName = Payload->GetStringField(TEXT("emitter_name"));

	if (SystemPath.IsEmpty() || EmitterName.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("system_path and emitter_name are required"));
		return;
	}

	if (!Payload->HasField(TEXT("enabled")))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("enabled is required"));
		return;
	}

	bool bEnabled = Payload->GetBoolField(TEXT("enabled"));

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!System)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));
		return;
	}

	// 查找 Emitter Handle 并修改
	bool bFound = false;
	// 获取可修改的引用（UE 5.3 提供 non-const 重载）
	TArray<FNiagaraEmitterHandle>& MutableHandles = System->GetEmitterHandles();
	for (FNiagaraEmitterHandle& Handle : MutableHandles)
	{
		if (Handle.GetName().ToString() == EmitterName)
		{
			FScopedTransaction Transaction(FText::FromString(TEXT("Set Emitter Enabled")));
			System->Modify();
			Handle.SetIsEnabled(bEnabled, *System, false);
			System->RequestCompile(false);
			System->MarkPackageDirty();
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		UAL_CommandUtils::SendError(RequestId, 404,
			FString::Printf(TEXT("Emitter '%s' not found in system '%s'"), *EmitterName, *SystemPath));
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("system_path"), System->GetPathName());
	Data->SetStringField(TEXT("emitter_name"), EmitterName);
	Data->SetBoolField(TEXT("enabled"), bEnabled);
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

// ============================================================================
// niagara.set_param  — 运行时 User Parameter Override
// ============================================================================

bool FUAL_NiagaraCommands::FindNiagaraComponentByTarget(
	const FString& Target, UNiagaraComponent*& OutComponent, FString& OutError)
{
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		OutError = TEXT("No world available");
		return false;
	}

	// 策略 1: 通过 Actor Label 查找
	AActor* FoundActor = UAL_CommandUtils::FindActorByLabel(World, Target);

	// 策略 2: 通过 Actor 路径查找
	if (!FoundActor)
	{
		FoundActor = FindObject<AActor>(nullptr, *Target);
	}

	// 策略 3: 按名称搜索所有 NiagaraActor
	if (!FoundActor)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor->GetActorNameOrLabel() == Target || Actor->GetFName().ToString() == Target)
			{
				FoundActor = Actor;
				break;
			}
		}
	}

	if (!FoundActor)
	{
		OutError = FString::Printf(TEXT("Actor not found: %s"), *Target);
		return false;
	}

	// 查找 NiagaraComponent
	OutComponent = FoundActor->FindComponentByClass<UNiagaraComponent>();
	if (!OutComponent)
	{
		OutError = FString::Printf(TEXT("Actor '%s' has no NiagaraComponent"), *Target);
		return false;
	}

	return true;
}

void FUAL_NiagaraCommands::Handle_SetParam(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Target = Payload->GetStringField(TEXT("target"));
	FString ParamName = Payload->GetStringField(TEXT("param_name"));
	FString ParamType = Payload->GetStringField(TEXT("param_type"));

	if (Target.IsEmpty() || ParamName.IsEmpty() || ParamType.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("target, param_name, and param_type are required"));
		return;
	}

	if (!Payload->HasField(TEXT("value")))
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("value is required"));
		return;
	}

	// 找到目标 NiagaraComponent
	UNiagaraComponent* NiagaraComp = nullptr;
	FString FindError;
	if (!FindNiagaraComponentByTarget(Target, NiagaraComp, FindError))
	{
		UAL_CommandUtils::SendError(RequestId, 404, FindError);
		return;
	}

	// 根据参数类型设置值
	FName ParamFName(*ParamName);
	ParamType = ParamType.ToLower();

	if (ParamType == TEXT("float"))
	{
		float Value = static_cast<float>(Payload->GetNumberField(TEXT("value")));
		NiagaraComp->SetVariableFloat(ParamFName, Value);
	}
	else if (ParamType == TEXT("int"))
	{
		int32 Value = static_cast<int32>(Payload->GetNumberField(TEXT("value")));
		NiagaraComp->SetVariableInt(ParamFName, Value);
	}
	else if (ParamType == TEXT("bool"))
	{
		bool Value = Payload->GetBoolField(TEXT("value"));
		NiagaraComp->SetVariableBool(ParamFName, Value);
	}
	else if (ParamType == TEXT("vector") || ParamType == TEXT("vec3"))
	{
		const TSharedPtr<FJsonObject>* VecObjPtr = nullptr;
		if (Payload->TryGetObjectField(TEXT("value"), VecObjPtr) && VecObjPtr && VecObjPtr->IsValid())
		{
			FVector Value = UAL_CommandUtils::ReadVectorDirect(*VecObjPtr);
			NiagaraComp->SetVariableVec3(ParamFName, Value);
		}
		else
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("value must be {x, y, z} for vector type"));
			return;
		}
	}
	else if (ParamType == TEXT("color") || ParamType == TEXT("linearcolor"))
	{
		const TSharedPtr<FJsonObject>* ColorObjPtr = nullptr;
		if (Payload->TryGetObjectField(TEXT("value"), ColorObjPtr) && ColorObjPtr && ColorObjPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& ColorObj = *ColorObjPtr;
			float R = ColorObj->HasField(TEXT("r")) ? static_cast<float>(ColorObj->GetNumberField(TEXT("r"))) : 0.f;
			float G = ColorObj->HasField(TEXT("g")) ? static_cast<float>(ColorObj->GetNumberField(TEXT("g"))) : 0.f;
			float B = ColorObj->HasField(TEXT("b")) ? static_cast<float>(ColorObj->GetNumberField(TEXT("b"))) : 0.f;
			float A = ColorObj->HasField(TEXT("a")) ? static_cast<float>(ColorObj->GetNumberField(TEXT("a"))) : 1.f;
			NiagaraComp->SetVariableLinearColor(ParamFName, FLinearColor(R, G, B, A));
		}
		else
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("value must be {r, g, b, a} for color type"));
			return;
		}
	}
	else if (ParamType == TEXT("vec2"))
	{
		const TSharedPtr<FJsonObject>* VecObjPtr = nullptr;
		if (Payload->TryGetObjectField(TEXT("value"), VecObjPtr) && VecObjPtr && VecObjPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& VecObj = *VecObjPtr;
			float X = VecObj->HasField(TEXT("x")) ? static_cast<float>(VecObj->GetNumberField(TEXT("x"))) : 0.f;
			float Y = VecObj->HasField(TEXT("y")) ? static_cast<float>(VecObj->GetNumberField(TEXT("y"))) : 0.f;
			NiagaraComp->SetVariableVec2(ParamFName, FVector2D(X, Y));
		}
		else
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("value must be {x, y} for vec2 type"));
			return;
		}
	}
	else if (ParamType == TEXT("vec4") || ParamType == TEXT("vector4"))
	{
		const TSharedPtr<FJsonObject>* VecObjPtr = nullptr;
		if (Payload->TryGetObjectField(TEXT("value"), VecObjPtr) && VecObjPtr && VecObjPtr->IsValid())
		{
			const TSharedPtr<FJsonObject>& VecObj = *VecObjPtr;
			float X = VecObj->HasField(TEXT("x")) ? static_cast<float>(VecObj->GetNumberField(TEXT("x"))) : 0.f;
			float Y = VecObj->HasField(TEXT("y")) ? static_cast<float>(VecObj->GetNumberField(TEXT("y"))) : 0.f;
			float Z = VecObj->HasField(TEXT("z")) ? static_cast<float>(VecObj->GetNumberField(TEXT("z"))) : 0.f;
			float W = VecObj->HasField(TEXT("w")) ? static_cast<float>(VecObj->GetNumberField(TEXT("w"))) : 0.f;
			NiagaraComp->SetVariableVec4(ParamFName, FVector4(X, Y, Z, W));
		}
		else
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("value must be {x, y, z, w} for vec4 type"));
			return;
		}
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 400,
			FString::Printf(TEXT("Unsupported param_type: %s. Supported: float, int, bool, vector, vec2, vec4, color"), *ParamType));
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("target"), Target);
	Data->SetStringField(TEXT("param_name"), ParamName);
	Data->SetStringField(TEXT("param_type"), ParamType);
	Data->SetField(TEXT("value"), Payload->TryGetField(TEXT("value")));
	UAL_CommandUtils::SendResponse(RequestId, 200, Data);

	UE_LOG(LogUALNiagara, Log, TEXT("Set param '%s' (%s) on target '%s'"), *ParamName, *ParamType, *Target);
}

// ============================================================================
// niagara.spawn — 场景放置（协议字段：actor_path, component_path, actor_label）
// ============================================================================

void FUAL_NiagaraCommands::Handle_Spawn(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString SystemPath = Payload->GetStringField(TEXT("system_path"));
	if (SystemPath.IsEmpty())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("system_path is required"));
		return;
	}

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!System)
	{
		UAL_CommandUtils::SendError(RequestId, 404, FString::Printf(TEXT("NiagaraSystem not found: %s"), *SystemPath));
		return;
	}

	// 读取变换参数
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector::OneVector;

	if (Payload->HasField(TEXT("location")))
	{
		const TSharedPtr<FJsonObject>* LocObjPtr = nullptr;
		if (Payload->TryGetObjectField(TEXT("location"), LocObjPtr) && LocObjPtr && LocObjPtr->IsValid())
		{
			Location = UAL_CommandUtils::ReadVectorDirect(*LocObjPtr);
		}
	}

	if (Payload->HasField(TEXT("rotation")))
	{
		const TSharedPtr<FJsonObject>* RotObjPtr = nullptr;
		if (Payload->TryGetObjectField(TEXT("rotation"), RotObjPtr) && RotObjPtr && RotObjPtr->IsValid())
		{
			Rotation = UAL_CommandUtils::ReadRotatorDirect(*RotObjPtr);
		}
	}

	if (Payload->HasField(TEXT("scale")))
	{
		const TSharedPtr<FJsonObject>* ScaleObjPtr = nullptr;
		if (Payload->TryGetObjectField(TEXT("scale"), ScaleObjPtr) && ScaleObjPtr && ScaleObjPtr->IsValid())
		{
			Scale = UAL_CommandUtils::ReadVectorDirect(*ScaleObjPtr, FVector::OneVector);
		}
	}

	bool bAutoActivate = Payload->HasField(TEXT("auto_activate"))
		? Payload->GetBoolField(TEXT("auto_activate"))
		: true;

	FString CustomLabel = Payload->HasField(TEXT("label"))
		? Payload->GetStringField(TEXT("label"))
		: TEXT("");

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No world available"));
		return;
	}

	// 创建 ANiagaraActor
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	ANiagaraActor* NiagaraActor = World->SpawnActor<ANiagaraActor>(
		ANiagaraActor::StaticClass(),
		Location,
		Rotation,
		SpawnParams
	);

	if (!NiagaraActor)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to spawn NiagaraActor"));
		return;
	}

	// 设置缩放
	NiagaraActor->SetActorScale3D(Scale);

	// 设置 NiagaraSystem
	UNiagaraComponent* NiagaraComp = NiagaraActor->GetNiagaraComponent();
	if (NiagaraComp)
	{
		NiagaraComp->SetAsset(System);
		NiagaraComp->SetAutoActivate(bAutoActivate);
		if (bAutoActivate)
		{
			NiagaraComp->Activate(true);
		}
	}

	// 设置自定义标签
	if (!CustomLabel.IsEmpty())
	{
		NiagaraActor->SetActorLabel(CustomLabel);
	}
	else
	{
		NiagaraActor->SetActorLabel(System->GetName());
	}

	// 构建协议字段
	TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject());
	Data->SetStringField(TEXT("system_path"), System->GetPathName());
	Data->SetStringField(TEXT("actor_path"), NiagaraActor->GetPathName());
	Data->SetStringField(TEXT("actor_label"), NiagaraActor->GetActorLabel());

	if (NiagaraComp)
	{
		Data->SetStringField(TEXT("component_path"), NiagaraComp->GetPathName());
	}

	// 位置信息
	Data->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(Location));
	Data->SetObjectField(TEXT("rotation"), UAL_CommandUtils::MakeRotatorJson(Rotation));
	Data->SetObjectField(TEXT("scale"), UAL_CommandUtils::MakeVectorJson(Scale));

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);

	UE_LOG(LogUALNiagara, Log, TEXT("Spawned NiagaraActor '%s' with system '%s' at (%s)"),
		*NiagaraActor->GetActorLabel(), *System->GetPathName(), *Location.ToString());
}
