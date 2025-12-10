#include "UAL_ActorCommands.h"
#include "UAL_CommandUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"
#include "Components/SceneComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALActor, Log, All);

void FUAL_ActorCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("actor.spawn"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SpawnActor(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.spawn_batch"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SpawnActorsBatch(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.destroy"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DestroyActor(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.destroy_batch"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DestroyActorsBatch(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.set_transform"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetTransformUnified(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.set_property"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetProperty(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.get_info"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetActorInfo(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.get"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetActor(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.inspect"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_InspectActor(Payload, RequestId);
	});
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========
// 原始行号参考:
//   SpawnSingleActor:        2514-2654
//   Handle_SpawnActor:       2656-2730
//   Handle_SpawnActorsBatch: 2732-2744
//   DestroySingleActor:      2746-2788
//   Handle_DestroyActor:     2790-2892
//   Handle_DestroyActorsBatch: 2894-2941
//   Handle_GetActorInfo:     2943-3005
//   Handle_GetActor:         3007-3089
//   Handle_InspectActor:     3091-3170
//   Handle_SetProperty:      3172-3330
//   Handle_SetTransformUnified: 3332-3529

TSharedPtr<FJsonObject> FUAL_ActorCommands::SpawnSingleActor(const TSharedPtr<FJsonObject>& Item)
{
	if (!Item.IsValid())
	{
		return nullptr;
	}

	FString PresetName, ClassPath, DesiredName, AssetId, MeshOverride;
	Item->TryGetStringField(TEXT("preset"), PresetName);
	Item->TryGetStringField(TEXT("class"), ClassPath);
	Item->TryGetStringField(TEXT("name"), DesiredName);
	Item->TryGetStringField(TEXT("asset_id"), AssetId);
	Item->TryGetStringField(TEXT("mesh"), MeshOverride);

	if (AssetId.IsEmpty() && PresetName.IsEmpty() && ClassPath.IsEmpty())
	{
		return nullptr; // Skip invalid
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		return nullptr;
	}

	UAL_CommandUtils::FUALResolvedSpawnRequest Resolved;
	FString ResolveError;

	if (!AssetId.IsEmpty())
	{
		if (!UAL_CommandUtils::ResolveSpawnFromAssetId(AssetId, Resolved, ResolveError))
		{
			UE_LOG(LogUALActor, Warning, TEXT("Spawn failed to resolve asset_id=%s error=%s"), *AssetId, *ResolveError);
			return nullptr;
		}
	}
	else if (!PresetName.IsEmpty())
	{
		UAL_CommandUtils::FUALSpawnPreset Preset;
		if (!UAL_CommandUtils::ResolvePreset(PresetName, Preset))
		{
			return nullptr;
		}
		Resolved.SpawnClass = Preset.Class;
		if (Preset.AssetPath)
		{
			Resolved.MeshPath = Preset.AssetPath;
		}
		Resolved.ResolvedType = Preset.Class ? Preset.Class->GetName() : TEXT("Preset");
		Resolved.SourceId = PresetName;
		Resolved.bFromAlias = true;
	}
	else if (!ClassPath.IsEmpty())
	{
		UObject* LoadedClassObj = StaticLoadObject(UClass::StaticClass(), nullptr, *ClassPath);
		if (LoadedClassObj)
		{
			Resolved.SpawnClass = Cast<UClass>(LoadedClassObj);
			if (Resolved.SpawnClass)
			{
				Resolved.ResolvedType = Resolved.SpawnClass->GetName();
			}
			Resolved.SourceId = ClassPath;
		}
	}

	if (Resolved.bFromAlias && PresetName.IsEmpty())
	{
		PresetName = Resolved.SourceId;
	}

	if (Resolved.SpawnClass == nullptr)
	{
		return nullptr;
	}

	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	FVector Scale = FVector(1, 1, 1);
	UAL_CommandUtils::ReadTransformFromItem(Item, Location, Rotation, Scale);

	FActorSpawnParameters Params;
	if (!DesiredName.IsEmpty())
	{
		Params.Name = FName(*DesiredName);
		Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	}

	const FTransform SpawnTransform(Rotation, Location);
	AActor* Actor = World->SpawnActor(Resolved.SpawnClass, &SpawnTransform, Params);
	if (!Actor)
	{
		return nullptr;
	}

	const TCHAR* MeshPath = nullptr;
	if (!MeshOverride.IsEmpty())
	{
		MeshPath = *MeshOverride;
	}
	else if (!Resolved.MeshPath.IsEmpty())
	{
		MeshPath = *Resolved.MeshPath;
	}

	if (!UAL_CommandUtils::SetStaticMeshIfNeeded(Actor, MeshPath))
	{
		Actor->Destroy();
		return nullptr;
	}

	Actor->SetActorScale3D(Scale);
#if WITH_EDITOR
	Actor->Modify();
	if (!DesiredName.IsEmpty())
	{
		Actor->SetActorLabel(DesiredName);
	}
#endif

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), UAL_CommandUtils::GetActorFriendlyName(Actor));
	Data->SetStringField(TEXT("path"), Actor->GetPathName());
	Data->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
	if (!AssetId.IsEmpty())
	{
		Data->SetStringField(TEXT("asset_id"), AssetId);
	}
	if (!Resolved.ResolvedType.IsEmpty())
	{
		Data->SetStringField(TEXT("type"), Resolved.ResolvedType);
	}
	if (!PresetName.IsEmpty())
	{
		Data->SetStringField(TEXT("preset"), PresetName);
	}
	return Data;
}

void FUAL_ActorCommands::Handle_SpawnActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 创建撤销事务，使生成操作可通过 Ctrl+Z 撤销
	FScopedTransaction Transaction(UAL_CommandUtils::LText(TEXT("生成Actor"), TEXT("Spawn Actor")));
#endif

	const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
	if (Payload->TryGetArrayField(TEXT("instances"), Instances) && Instances)
	{
		TArray<TSharedPtr<FJsonValue>> Created;
		int32 SuccessCount = 0;

		for (const TSharedPtr<FJsonValue>& Val : *Instances)
		{
			const TSharedPtr<FJsonObject> Item = Val->AsObject();
			if (TSharedPtr<FJsonObject> Res = SpawnSingleActor(Item))
			{
				Created.Add(MakeShared<FJsonValueObject>(Res));
				SuccessCount++;
			}
			else
			{
				Created.Add(MakeShared<FJsonValueNull>());
			}
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("created"), Created);
		Data->SetNumberField(TEXT("count"), SuccessCount);

		UAL_CommandUtils::SendResponse(RequestId, SuccessCount > 0 ? 200 : 500, Data);
		return;
	}

	// 兼容旧 batch 字段
	const TArray<TSharedPtr<FJsonValue>>* BatchCompat = nullptr;
	if (Payload->TryGetArrayField(TEXT("batch"), BatchCompat) && BatchCompat)
	{
		TSharedPtr<FJsonObject> CompatPayload = MakeShared<FJsonObject>();
		CompatPayload->SetArrayField(TEXT("instances"), *BatchCompat);
		Handle_SpawnActor(CompatPayload, RequestId);
		return;
	}

	TSharedPtr<FJsonObject> Data = SpawnSingleActor(Payload);
	if (Data.IsValid())
	{
#if WITH_EDITOR
		// 单体创建时尝试选中（批量时不选，避免闪烁）
		if (GEditor)
		{
			// 重新查找以执行Select
			FString Path;
			Data->TryGetStringField(TEXT("path"), Path);
			AActor* Actor = Cast<AActor>(StaticFindObject(AActor::StaticClass(), nullptr, *Path));
			if (Actor)
			{
				GEditor->SelectNone(false, true, false);
				GEditor->SelectActor(Actor, true, true);
				GEditor->NoteSelectionChange();
			}
		}
#endif
		UAL_CommandUtils::SendResponse(RequestId, 200, Data);
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Spawn failed"));
	}
}

void FUAL_ActorCommands::Handle_SpawnActorsBatch(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TArray<TSharedPtr<FJsonValue>>* Batch = nullptr;
	if (!Payload->TryGetArrayField(TEXT("batch"), Batch) || !Batch)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing batch array"));
		return;
	}

	TSharedPtr<FJsonObject> ForwardPayload = MakeShared<FJsonObject>();
	ForwardPayload->SetArrayField(TEXT("instances"), *Batch);
	Handle_SpawnActor(ForwardPayload, RequestId);
}

bool FUAL_ActorCommands::DestroySingleActor(const FString& Name, const FString& Path)
{
	if (Name.IsEmpty() && Path.IsEmpty())
	{
		return false;
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		return false;
	}

	AActor* TargetActor = nullptr;
	if (!Path.IsEmpty())
	{
		TargetActor = FindObject<AActor>(nullptr, *Path);
		if (!TargetActor)
		{
			TargetActor = Cast<AActor>(StaticFindObject(AActor::StaticClass(), nullptr, *Path));
		}
	}

	if (!TargetActor && !Name.IsEmpty())
	{
		TargetActor = UAL_CommandUtils::FindActorByLabel(World, Name);
	}

	if (!TargetActor)
	{
		return false;
	}

#if WITH_EDITOR
	// 使用 EditorDestroyActor 以支持撤销操作
	if (World)
	{
		return World->EditorDestroyActor(TargetActor, true);
	}
#endif
	return TargetActor->Destroy();
}

void FUAL_ActorCommands::Handle_DestroyActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 创建撤销事务，使删除操作可通过 Ctrl+Z 撤销
	FScopedTransaction Transaction(UAL_CommandUtils::LText(TEXT("删除Actor"), TEXT("Delete Actor")));
#endif

	// 新版：targets 选择器
	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("targets"), TargetsObj) && TargetsObj && TargetsObj->IsValid())
	{
		UWorld* World = UAL_CommandUtils::GetTargetWorld();
		if (!World)
		{
			UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
			return;
		}

		TSet<AActor*> TargetSet;
		FString TargetError;
		if (!UAL_CommandUtils::ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError))
		{
			UAL_CommandUtils::SendError(RequestId, 404, TargetError);
			return;
		}

		int32 SuccessCount = 0;
		TArray<TSharedPtr<FJsonValue>> Deleted;
		for (AActor* Actor : TargetSet)
		{
			if (!Actor)
			{
				continue;
			}
			const FString FriendlyName = UAL_CommandUtils::GetActorFriendlyName(Actor);
			const FString ActorPath = Actor->GetPathName();
			const FString ActorClass = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();

			bool bDestroyed = false;
#if WITH_EDITOR
			// 使用 EditorDestroyActor 以支持撤销操作
			bDestroyed = World->EditorDestroyActor(Actor, true);
#else
			bDestroyed = Actor->Destroy();
#endif
			if (bDestroyed)
			{
				SuccessCount++;
				TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("name"), FriendlyName);
				Obj->SetStringField(TEXT("path"), ActorPath);
				if (!ActorClass.IsEmpty())
				{
					Obj->SetStringField(TEXT("class"), ActorClass);
				}
				Deleted.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("count"), SuccessCount);
		Data->SetNumberField(TEXT("target_count"), TargetSet.Num());
		Data->SetArrayField(TEXT("deleted_actors"), Deleted);

		const int32 Code = SuccessCount > 0 ? 200 : 404;
		UAL_CommandUtils::SendResponse(RequestId, Code, Data);
		return;
	}

	// 兼容旧：name/path
	FString Label;
	FString Path;
	Payload->TryGetStringField(TEXT("name"), Label);
	Payload->TryGetStringField(TEXT("path"), Path);

	const bool bDestroyed = DestroySingleActor(Label, Path);
	if (!bDestroyed)
	{
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("Actor not found or failed to destroy"));
		return;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), bDestroyed);
	Data->SetStringField(TEXT("name"), Label);
	if (!Path.IsEmpty())
	{
		Data->SetStringField(TEXT("path"), Path);
	}
	Data->SetNumberField(TEXT("count"), 1);

	TArray<TSharedPtr<FJsonValue>> Deleted;
	TSharedPtr<FJsonObject> DeletedObj = MakeShared<FJsonObject>();
	DeletedObj->SetStringField(TEXT("name"), Label);
	if (!Path.IsEmpty())
	{
		DeletedObj->SetStringField(TEXT("path"), Path);
	}
	Deleted.Add(MakeShared<FJsonValueObject>(DeletedObj));
	Data->SetArrayField(TEXT("deleted_actors"), Deleted);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_ActorCommands::Handle_DestroyActorsBatch(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TArray<TSharedPtr<FJsonValue>>* Batch = nullptr;
	if (!Payload->TryGetArrayField(TEXT("batch"), Batch) || !Batch)
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing batch array"));
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Names;
	TArray<TSharedPtr<FJsonValue>> Paths;

	for (const TSharedPtr<FJsonValue>& Val : *Batch)
	{
		const TSharedPtr<FJsonObject> Item = Val->AsObject();
		if (!Item.IsValid())
		{
			continue;
		}

		FString Label, Path;
		Item->TryGetStringField(TEXT("name"), Label);
		Item->TryGetStringField(TEXT("path"), Path);

		if (!Label.IsEmpty())
		{
			Names.Add(MakeShared<FJsonValueString>(Label));
		}
		if (!Path.IsEmpty())
		{
			Paths.Add(MakeShared<FJsonValueString>(Path));
		}
	}

	TSharedPtr<FJsonObject> Targets = MakeShared<FJsonObject>();
	if (Names.Num() > 0)
	{
		Targets->SetArrayField(TEXT("names"), Names);
	}
	if (Paths.Num() > 0)
	{
		Targets->SetArrayField(TEXT("paths"), Paths);
	}

	TSharedPtr<FJsonObject> ForwardPayload = MakeShared<FJsonObject>();
	ForwardPayload->SetObjectField(TEXT("targets"), Targets);
	Handle_DestroyActor(ForwardPayload, RequestId);
}

void FUAL_ActorCommands::Handle_GetActorInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	bool bReturnTransform = true;
	Payload->TryGetBoolField(TEXT("return_transform"), bReturnTransform);

	bool bReturnBounds = false;
	Payload->TryGetBoolField(TEXT("return_bounds"), bReturnBounds);

	int32 Limit = 50;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	if (Limit <= 0)
	{
		Limit = 50;
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!UAL_CommandUtils::ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError))
	{
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	const int32 TotalFound = TargetSet.Num();

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = UAL_CommandUtils::GetActorFriendlyName(A);
		const FString NameB = UAL_CommandUtils::GetActorFriendlyName(B);
		return NameA < NameB;
	});

	TArray<TSharedPtr<FJsonValue>> ActorsJson;
	for (int32 Index = 0; Index < TargetArray.Num() && Index < Limit; ++Index)
	{
		if (TSharedPtr<FJsonObject> Info = UAL_CommandUtils::BuildActorInfoWithOptions(TargetArray[Index], bReturnTransform, bReturnBounds))
		{
			ActorsJson.Add(MakeShared<FJsonValueObject>(Info));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), ActorsJson.Num());
	Data->SetNumberField(TEXT("total_found"), TotalFound);
	Data->SetArrayField(TEXT("actors"), ActorsJson);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_ActorCommands::Handle_GetActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	// 兼容旧参数，同时复用统一 targets 解析逻辑
	const TSharedPtr<FJsonObject>* TargetsObjPtr = nullptr;
	TSharedPtr<FJsonObject> Targets;
	if (Payload->TryGetObjectField(TEXT("targets"), TargetsObjPtr) && TargetsObjPtr && TargetsObjPtr->IsValid())
	{
		Targets = *TargetsObjPtr;
	}
	else
	{
		FString Label;
		Payload->TryGetStringField(TEXT("name"), Label);

		FString Path;
		Payload->TryGetStringField(TEXT("path"), Path);

		if (Label.IsEmpty() && Path.IsEmpty())
		{
			UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing field: name or path"));
			return;
		}

		Targets = MakeShared<FJsonObject>();
		if (!Label.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Names;
			Names.Add(MakeShared<FJsonValueString>(Label));
			Targets->SetArrayField(TEXT("names"), Names);
		}
		if (!Path.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Paths;
			Paths.Add(MakeShared<FJsonValueString>(Path));
			Targets->SetArrayField(TEXT("paths"), Paths);
		}
	}

	if (!Targets.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!UAL_CommandUtils::ResolveTargetsToActors(Targets, World, TargetSet, TargetError))
	{
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = UAL_CommandUtils::GetActorFriendlyName(A);
		const FString NameB = UAL_CommandUtils::GetActorFriendlyName(B);
		return NameA < NameB;
	});

	AActor* TargetActor = TargetArray.Num() > 0 ? TargetArray[0] : nullptr;
	if (!TargetActor)
	{
		UAL_CommandUtils::SendError(RequestId, 404, TEXT("Actor not found"));
		return;
	}

	if (TSharedPtr<FJsonObject> Info = UAL_CommandUtils::BuildActorInfo(TargetActor))
	{
		UAL_CommandUtils::SendResponse(RequestId, 200, Info);
	}
	else
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("Failed to build actor info"));
	}
}

void FUAL_ActorCommands::Handle_InspectActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TSharedPtr<FJsonObject>* TargetsObjPtr = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObjPtr) || !TargetsObjPtr || !TargetsObjPtr->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}
	const TSharedPtr<FJsonObject> Targets = *TargetsObjPtr;

	// properties: 若为空/缺省则使用默认白名单
	TArray<FString> WantedProps;
	const TArray<TSharedPtr<FJsonValue>>* PropsArr = nullptr;
	if (Payload->TryGetArrayField(TEXT("properties"), PropsArr) && PropsArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *PropsArr)
		{
			FString PropName;
			if (V.IsValid() && V->TryGetString(PropName) && !PropName.IsEmpty())
			{
				WantedProps.Add(PropName);
			}
		}
	}
	if (WantedProps.Num() == 0)
	{
		WantedProps = UAL_CommandUtils::GetDefaultInspectProps();
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!UAL_CommandUtils::ResolveTargetsToActors(Targets, World, TargetSet, TargetError))
	{
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = UAL_CommandUtils::GetActorFriendlyName(A);
		const FString NameB = UAL_CommandUtils::GetActorFriendlyName(B);
		return NameA < NameB;
	});

	TArray<TSharedPtr<FJsonValue>> Results;
	for (AActor* Actor : TargetArray)
	{
		if (!Actor)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Obj = UAL_CommandUtils::BuildActorInfo(Actor);
		if (!Obj.IsValid())
		{
			continue;
		}

		if (TSharedPtr<FJsonObject> Props = UAL_CommandUtils::BuildSelectedProps(Actor, WantedProps))
		{
			Obj->SetObjectField(TEXT("props"), Props);
		}

		Results.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Results.Num());
	Data->SetArrayField(TEXT("actors"), Results);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_ActorCommands::Handle_SetProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 创建撤销事务，使属性修改操作可通过 Ctrl+Z 撤销
	FScopedTransaction Transaction(UAL_CommandUtils::LText(TEXT("修改Actor属性"), TEXT("Modify Actor Property")));
#endif

	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !PropsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: properties"));
		return;
	}

	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!UAL_CommandUtils::ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError))
	{
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = UAL_CommandUtils::GetActorFriendlyName(A);
		const FString NameB = UAL_CommandUtils::GetActorFriendlyName(B);
		return NameA < NameB;
	});

	int32 SuccessActors = 0;
	TArray<TSharedPtr<FJsonValue>> ActorResults;

	for (AActor* Actor : TargetArray)
	{
		if (!Actor)
		{
			continue;
		}

		TSharedPtr<FJsonObject> ActorObj = UAL_CommandUtils::BuildActorInfo(Actor);
		if (!ActorObj.IsValid())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Updated = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Errors;

		TArray<FString> CandidateNames;
		UAL_CommandUtils::CollectPropertyNames(Actor, CandidateNames);
		if (USceneComponent* RootComp = Actor->GetRootComponent())
		{
			UAL_CommandUtils::CollectPropertyNames(RootComp, CandidateNames);
		}
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			UAL_CommandUtils::CollectPropertyNames(Comp, CandidateNames);
		}

		for (const auto& Pair : (*PropsObj)->Values)
		{
			const FString PropName = Pair.Key;
			const TSharedPtr<FJsonValue>& DesiredValue = Pair.Value;

			UObject* TargetObj = nullptr;
			FProperty* Prop = UAL_CommandUtils::FindWritablePropertyOnActorHierarchy(Actor, PropName, TargetObj);

			if (!Prop)
			{
				TArray<FString> Suggestions;
				UAL_CommandUtils::SuggestProperties(PropName, CandidateNames, Suggestions);

				TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
				Err->SetStringField(TEXT("property"), PropName);
				Err->SetStringField(TEXT("error"), TEXT("Property not found"));
				if (Suggestions.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> SuggestVals;
					for (const FString& S : Suggestions)
					{
						SuggestVals.Add(MakeShared<FJsonValueString>(S));
					}
					Err->SetArrayField(TEXT("suggestions"), SuggestVals);
				}
				Errors.Add(MakeShared<FJsonValueObject>(Err));
				continue;
			}

			FString TypeError;
			if (UAL_CommandUtils::SetSimpleProperty(Prop, TargetObj, DesiredValue, TypeError))
			{
				// 通知引擎属性已更改，触发渲染刷新
#if WITH_EDITOR
				FPropertyChangedEvent ChangedEvent(Prop, EPropertyChangeType::ValueSet);
				TargetObj->PostEditChangeProperty(ChangedEvent);
				
				// 如果目标对象是组件，还需要标记组件渲染状态为脏
				if (UActorComponent* Comp = Cast<UActorComponent>(TargetObj))
				{
					Comp->MarkRenderStateDirty();
				}
#endif
				if (TSharedPtr<FJsonValue> JsonValue = UAL_CommandUtils::PropertyToJsonValueCompat(Prop, Prop->ContainerPtrToValuePtr<void>(TargetObj)))
				{
					Updated->SetField(PropName, JsonValue);
				}
				else
				{
					Updated->SetField(PropName, DesiredValue);
				}
				continue;
			}

			TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
			Err->SetStringField(TEXT("property"), PropName);
			Err->SetStringField(TEXT("error"), TypeError.IsEmpty() ? TEXT("Failed to set property") : TypeError);
			if (TSharedPtr<FJsonValue> Current = UAL_CommandUtils::PropertyToJsonValueCompat(Prop, Prop->ContainerPtrToValuePtr<void>(TargetObj)))
			{
				Err->SetStringField(TEXT("expected_type"), Prop->GetClass()->GetName());
				Err->SetStringField(TEXT("current_value"), UAL_CommandUtils::JsonValueToString(Current));
			}
			Errors.Add(MakeShared<FJsonValueObject>(Err));
		}

		if (Updated->Values.Num() > 0)
		{
			Actor->Modify();
			SuccessActors++;
			ActorObj->SetObjectField(TEXT("updated"), Updated);
		}
		if (Errors.Num() > 0)
		{
			ActorObj->SetArrayField(TEXT("errors"), Errors);
		}

		ActorResults.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), SuccessActors);
	Data->SetArrayField(TEXT("actors"), ActorResults);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}

void FUAL_ActorCommands::Handle_SetTransformUnified(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	const TSharedPtr<FJsonObject>* OpObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("operation"), OpObj) || !OpObj || !OpObj->IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing object: operation"));
		return;
	}

	const TSharedPtr<FJsonObject>& Targets = *TargetsObj;
	const TSharedPtr<FJsonObject>& Operation = *OpObj;

	// space
	FString SpaceStr;
	Operation->TryGetStringField(TEXT("space"), SpaceStr);
	const bool bLocalSpace = SpaceStr.Equals(TEXT("Local"), ESearchCase::IgnoreCase);

	bool bSnapToFloor = false;
	Operation->TryGetBoolField(TEXT("snap_to_floor"), bSnapToFloor);

	// set / add / multiply
	TSharedPtr<FJsonObject> SetObj, AddObj, MulObj;
	UAL_CommandUtils::TryGetObjectFieldFlexible(Operation, TEXT("set"), SetObj);
	UAL_CommandUtils::TryGetObjectFieldFlexible(Operation, TEXT("add"), AddObj);
	UAL_CommandUtils::TryGetObjectFieldFlexible(Operation, TEXT("multiply"), MulObj);

	if (!SetObj.IsValid() && !AddObj.IsValid() && !MulObj.IsValid())
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Missing operation fields: set/add/multiply"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!UAL_CommandUtils::ResolveTargetsToActors(Targets, World, TargetSet, TargetError))
	{
		UAL_CommandUtils::SendError(RequestId, 404, TargetError);
		return;
	}

#if WITH_EDITOR
	FScopedTransaction Transaction(UAL_CommandUtils::LText(TEXT("批量修改Actor变换"), TEXT("Batch Modify Actor Transform")));
#endif

	int32 AffectedCount = 0;
	TArray<TSharedPtr<FJsonValue>> Affected;
	const int32 MaxReport = 100;

	for (AActor* Actor : TargetSet)
	{
		if (!Actor)
		{
			continue;
		}

		FVector NewLocation = Actor->GetActorLocation();
		FRotator NewRotation = Actor->GetActorRotation();
		FVector NewScale = Actor->GetActorScale3D();

		if (SetObj.IsValid())
		{
			TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(SetObj, TEXT("location"), LocObj))
			{
				NewLocation = UAL_CommandUtils::ReadVectorDirect(LocObj, NewLocation);
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(SetObj, TEXT("rotation"), RotObj))
			{
				NewRotation = UAL_CommandUtils::ReadRotatorDirect(RotObj, NewRotation);
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(SetObj, TEXT("scale"), ScaleObj))
			{
				NewScale = UAL_CommandUtils::ReadVectorDirect(ScaleObj, NewScale);
			}
		}

		if (AddObj.IsValid())
		{
			TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(AddObj, TEXT("location"), LocObj))
			{
				FVector Delta = UAL_CommandUtils::ReadVectorDirect(LocObj, FVector::ZeroVector);
				if (bLocalSpace)
				{
					// 使用已累积的旋转结果，避免同一请求中先设置旋转再按旧旋转位移导致偏差
					Delta = NewRotation.RotateVector(Delta);
				}
				NewLocation += Delta;
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(AddObj, TEXT("rotation"), RotObj))
			{
				FRotator Delta = UAL_CommandUtils::ReadRotatorDirect(RotObj, FRotator::ZeroRotator);
				if (bLocalSpace)
				{
					FQuat Curr = NewRotation.Quaternion();
					FQuat Dq = Delta.Quaternion();
					NewRotation = (Curr * Dq).Rotator();
				}
				else
				{
					NewRotation += Delta;
				}
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(AddObj, TEXT("scale"), ScaleObj))
			{
				NewScale += UAL_CommandUtils::ReadVectorDirect(ScaleObj, FVector::ZeroVector);
			}
		}

		if (MulObj.IsValid())
		{
			TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(MulObj, TEXT("location"), LocObj))
			{
				const FVector Mul = UAL_CommandUtils::ReadVectorDirect(LocObj, FVector(1, 1, 1));
				NewLocation.X *= Mul.X;
				NewLocation.Y *= Mul.Y;
				NewLocation.Z *= Mul.Z;
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(MulObj, TEXT("rotation"), RotObj))
			{
				const FRotator MulRot = UAL_CommandUtils::ReadRotatorDirect(RotObj, FRotator(1, 1, 1));
				NewRotation.Roll *= MulRot.Roll;
				NewRotation.Pitch *= MulRot.Pitch;
				NewRotation.Yaw *= MulRot.Yaw;
			}
			if (UAL_CommandUtils::TryGetObjectFieldFlexible(MulObj, TEXT("scale"), ScaleObj))
			{
				const FVector Mul = UAL_CommandUtils::ReadVectorDirect(ScaleObj, FVector(1, 1, 1));
				NewScale.X *= Mul.X;
				NewScale.Y *= Mul.Y;
				NewScale.Z *= Mul.Z;
			}
		}

		Actor->Modify();
		Actor->SetActorLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
		Actor->SetActorScale3D(NewScale);

		if (bSnapToFloor)
		{
#if WITH_EDITOR
			if (GEditor)
			{
				const bool bWasSelected = Actor->IsSelected();
				if (!bWasSelected)
				{
					GEditor->SelectActor(Actor, true, false);
				}
				GEditor->Exec(World, TEXT("SNAPTOFLOOR"));
				NewLocation = Actor->GetActorLocation();
				if (!bWasSelected)
				{
					GEditor->SelectActor(Actor, false, false);
				}
			}
#endif
		}

		AffectedCount++;
		if (Affected.Num() < MaxReport)
		{
			TSharedPtr<FJsonObject> Obj = UAL_CommandUtils::BuildActorInfo(Actor);
			if (Obj.IsValid())
			{
				Obj->SetObjectField(TEXT("location"), UAL_CommandUtils::MakeVectorJson(NewLocation));
				Obj->SetObjectField(TEXT("rotation"), UAL_CommandUtils::MakeRotatorJson(NewRotation));
				Obj->SetObjectField(TEXT("scale"), UAL_CommandUtils::MakeVectorJson(NewScale));
				Affected.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), AffectedCount);
	if (Affected.Num() > 0)
	{
		Data->SetArrayField(TEXT("actors"), Affected);
		Data->SetNumberField(TEXT("reported"), Affected.Num());
		Data->SetNumberField(TEXT("report_limit"), MaxReport);
	}

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}
