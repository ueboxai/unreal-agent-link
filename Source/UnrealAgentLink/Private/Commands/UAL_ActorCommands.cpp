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

			// 特殊处理 ActorLabel：它是 Editor-Only 属性，需要调用专用函数 SetActorLabel
			// 该函数会自动处理名称冲突（自动添加后缀），而不是简单的内存读写
#if WITH_EDITOR
			if (PropName.Equals(TEXT("ActorLabel"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("Label"), ESearchCase::IgnoreCase))
			{
				FString NewLabel;
				if (DesiredValue.IsValid() && DesiredValue->TryGetString(NewLabel) && !NewLabel.IsEmpty())
				{
					const FString OldLabel = Actor->GetActorLabel();
					Actor->SetActorLabel(NewLabel);
					const FString FinalLabel = Actor->GetActorLabel();
					
					Updated->SetStringField(TEXT("ActorLabel"), FinalLabel);
					
					// 如果最终标签与请求的不同，说明发生了名称冲突自动加后缀
					if (!FinalLabel.Equals(NewLabel))
					{
						TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
						Warning->SetStringField(TEXT("property"), TEXT("ActorLabel"));
						Warning->SetStringField(TEXT("warning"), TEXT("Name conflict resolved with suffix"));
						Warning->SetStringField(TEXT("requested"), NewLabel);
						Warning->SetStringField(TEXT("actual"), FinalLabel);
						Errors.Add(MakeShared<FJsonValueObject>(Warning));
					}
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("ActorLabel must be a non-empty string"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}
#endif

			// ========== 特殊属性拦截白名单 ==========
			// 以下属性在用户眼中是"属性"，但在 C++ 底层是"函数调用"或需要触发状态重建
			// 直接通过反射修改内存值不会生效，或不会触发渲染/物理更新

			// 1. FolderPath (世界大纲文件夹)
			// 直接改变量不会刷新大纲视图，必须调用 SetFolderPath
#if WITH_EDITOR
			if (PropName.Equals(TEXT("FolderPath"), ESearchCase::IgnoreCase))
			{
				FString NewPath;
				if (DesiredValue.IsValid() && DesiredValue->TryGetString(NewPath))
				{
					Actor->SetFolderPath(FName(*NewPath));
					Updated->SetStringField(TEXT("FolderPath"), Actor->GetFolderPath().ToString());
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("FolderPath must be a string"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}
#endif

			// 2. SimulatePhysics (物理模拟)
			// 这是 RootComponent (UPrimitiveComponent) 的属性，必须调用 SetSimulatePhysics 触发物理状态重建
			if (PropName.Equals(TEXT("SimulatePhysics"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("bSimulatePhysics"), ESearchCase::IgnoreCase))
			{
				bool bSimulate = false;
				if (DesiredValue.IsValid() && DesiredValue->TryGetBool(bSimulate))
				{
					UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
					if (PrimComp)
					{
						PrimComp->SetSimulatePhysics(bSimulate);
						Updated->SetBoolField(TEXT("SimulatePhysics"), PrimComp->IsSimulatingPhysics());
					}
					else
					{
						TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
						Err->SetStringField(TEXT("property"), PropName);
						Err->SetStringField(TEXT("error"), TEXT("Actor has no UPrimitiveComponent as RootComponent"));
						Errors.Add(MakeShared<FJsonValueObject>(Err));
					}
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("SimulatePhysics must be a boolean"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}

			// 3. Mobility (移动性)
			// 修改移动性涉及光照失效、导航网格失效等，必须调用 SetMobility
			if (PropName.Equals(TEXT("Mobility"), ESearchCase::IgnoreCase))
			{
				USceneComponent* RootComp = Actor->GetRootComponent();
				if (RootComp)
				{
					FString MobilityStr;
					EComponentMobility::Type NewMobility = RootComp->Mobility;
					bool bValidInput = false;

					if (DesiredValue.IsValid() && DesiredValue->TryGetString(MobilityStr))
					{
						if (MobilityStr.Equals(TEXT("Static"), ESearchCase::IgnoreCase))
						{
							NewMobility = EComponentMobility::Static;
							bValidInput = true;
						}
						else if (MobilityStr.Equals(TEXT("Stationary"), ESearchCase::IgnoreCase))
						{
							NewMobility = EComponentMobility::Stationary;
							bValidInput = true;
						}
						else if (MobilityStr.Equals(TEXT("Movable"), ESearchCase::IgnoreCase))
						{
							NewMobility = EComponentMobility::Movable;
							bValidInput = true;
						}
					}
					else
					{
						// 尝试数字输入
						int32 MobilityInt = 0;
						if (DesiredValue.IsValid() && DesiredValue->TryGetNumber(MobilityInt))
						{
							if (MobilityInt >= 0 && MobilityInt <= 2)
							{
								NewMobility = static_cast<EComponentMobility::Type>(MobilityInt);
								bValidInput = true;
							}
						}
					}

					if (bValidInput)
					{
						RootComp->SetMobility(NewMobility);
						
						// 返回字符串形式的移动性
						FString ResultMobility;
						switch (RootComp->Mobility)
						{
						case EComponentMobility::Static: ResultMobility = TEXT("Static"); break;
						case EComponentMobility::Stationary: ResultMobility = TEXT("Stationary"); break;
						case EComponentMobility::Movable: ResultMobility = TEXT("Movable"); break;
						default: ResultMobility = TEXT("Unknown"); break;
						}
						Updated->SetStringField(TEXT("Mobility"), ResultMobility);
					}
					else
					{
						TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
						Err->SetStringField(TEXT("property"), PropName);
						Err->SetStringField(TEXT("error"), TEXT("Mobility must be 'Static', 'Stationary', or 'Movable'"));
						Errors.Add(MakeShared<FJsonValueObject>(Err));
					}
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("Actor has no RootComponent"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}

			// 4. Hidden / bHidden (运行时显隐)
			// 调用 SetActorHiddenInGame，处理网络同步和子组件递归显隐
			// 注意：这是运行时隐藏，在编辑器视图中可能仍然可见
			if (PropName.Equals(TEXT("Hidden"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("bHidden"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("HiddenInGame"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("bHiddenInGame"), ESearchCase::IgnoreCase))
			{
				bool bHidden = false;
				if (DesiredValue.IsValid() && DesiredValue->TryGetBool(bHidden))
				{
					Actor->SetActorHiddenInGame(bHidden);
					Updated->SetBoolField(TEXT("bHidden"), Actor->IsHidden());
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("bHidden must be a boolean"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}

			// 5. HiddenInEditor / bHiddenEd (编辑器模式显隐)
			// 调用 SetIsTemporarilyHiddenInEditor，在编辑器视图中立即隐藏/显示
#if WITH_EDITOR
			if (PropName.Equals(TEXT("HiddenInEditor"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("bHiddenInEditor"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("bHiddenEd"), ESearchCase::IgnoreCase))
			{
				bool bHidden = false;
				if (DesiredValue.IsValid() && DesiredValue->TryGetBool(bHidden))
				{
					Actor->SetIsTemporarilyHiddenInEditor(bHidden);
					Updated->SetBoolField(TEXT("bHiddenInEditor"), Actor->IsTemporarilyHiddenInEditor());
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("bHiddenInEditor must be a boolean"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}
#endif

			// 6. Tags (Actor 标签数组)
			// Actor::Tags 是 TArray<FName>，需要特殊处理
			// 支持：1) 数组覆盖 ["tag1", "tag2"]
			//       2) 单字符串添加 "tag1"
			//       3) 对象操作 { "add": ["tag1"], "remove": ["tag2"] }
			if (PropName.Equals(TEXT("Tags"), ESearchCase::IgnoreCase))
			{
				bool bSuccess = false;
				
				// 尝试解析为数组（覆盖模式）
				const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
				if (DesiredValue.IsValid() && DesiredValue->TryGetArray(TagsArray) && TagsArray)
				{
					Actor->Tags.Empty();
					for (const TSharedPtr<FJsonValue>& TagVal : *TagsArray)
					{
						FString TagStr;
						if (TagVal.IsValid() && TagVal->TryGetString(TagStr) && !TagStr.IsEmpty())
						{
							Actor->Tags.AddUnique(FName(*TagStr));
						}
					}
					bSuccess = true;
				}
				// 尝试解析为对象（增删模式）
				else if (const TSharedPtr<FJsonObject>* TagsObj = nullptr; 
						 DesiredValue.IsValid() && DesiredValue->TryGetObject(TagsObj) && TagsObj && TagsObj->IsValid())
				{
					// 处理 add
					const TArray<TSharedPtr<FJsonValue>>* AddArray = nullptr;
					if ((*TagsObj)->TryGetArrayField(TEXT("add"), AddArray) && AddArray)
					{
						for (const TSharedPtr<FJsonValue>& TagVal : *AddArray)
						{
							FString TagStr;
							if (TagVal.IsValid() && TagVal->TryGetString(TagStr) && !TagStr.IsEmpty())
							{
								Actor->Tags.AddUnique(FName(*TagStr));
							}
						}
					}
					// 处理 remove
					const TArray<TSharedPtr<FJsonValue>>* RemoveArray = nullptr;
					if ((*TagsObj)->TryGetArrayField(TEXT("remove"), RemoveArray) && RemoveArray)
					{
						for (const TSharedPtr<FJsonValue>& TagVal : *RemoveArray)
						{
							FString TagStr;
							if (TagVal.IsValid() && TagVal->TryGetString(TagStr) && !TagStr.IsEmpty())
							{
								Actor->Tags.Remove(FName(*TagStr));
							}
						}
					}
					bSuccess = true;
				}
				// 尝试解析为单字符串（添加单个标签）
				else
				{
					FString SingleTag;
					if (DesiredValue.IsValid() && DesiredValue->TryGetString(SingleTag) && !SingleTag.IsEmpty())
					{
						Actor->Tags.AddUnique(FName(*SingleTag));
						bSuccess = true;
					}
				}

				if (bSuccess)
				{
					// 返回当前所有标签
					TArray<TSharedPtr<FJsonValue>> TagsJson;
					for (const FName& Tag : Actor->Tags)
					{
						TagsJson.Add(MakeShared<FJsonValueString>(Tag.ToString()));
					}
					Updated->SetArrayField(TEXT("Tags"), TagsJson);
				}
				else
				{
					TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
					Err->SetStringField(TEXT("property"), PropName);
					Err->SetStringField(TEXT("error"), TEXT("Tags must be a string, array of strings, or object with 'add'/'remove' arrays"));
					Errors.Add(MakeShared<FJsonValueObject>(Err));
				}
				continue;
			}

			// ========== 通用属性处理 ==========
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
