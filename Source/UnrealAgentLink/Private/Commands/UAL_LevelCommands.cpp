#include "UAL_LevelCommands.h"
#include "UAL_CommandUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "PhysicsEngine/BodySetup.h"
#include "AssetRegistry/AssetRegistryModule.h"

#if WITH_EDITOR
#include "Selection.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUALLevel, Log, All);

void FUAL_LevelCommands::RegisterCommands(TMap<FString, TFunction<void(const TSharedPtr<FJsonObject>&, const FString)>>& CommandMap)
{
	CommandMap.Add(TEXT("level.query_assets"), [](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_QueryAssets(Payload, RequestId);
	});
}

// ========== 从 UAL_CommandHandler.cpp 迁移以下函数 ==========
// 原始行号参考:
//   Handle_QueryAssets: 1721-1826 (含后续过滤逻辑直到约 1981 行)

void FUAL_LevelCommands::Handle_QueryAssets(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	struct FQueryItem
	{
		FString Name;
		FString Path;
		FString Type;
		int32 Triangles = -1;
		int64 DiskSize = 0;
		bool bNanite = false;
		bool bMissingCollision = false;
		bool bCastsShadow = false;
	};

	// 1) 解析参数
	FString ScopeType = TEXT("Level");
	FString ScopePath;
	if (const TSharedPtr<FJsonObject>* ScopeObj = nullptr; Payload->TryGetObjectField(TEXT("scope"), ScopeObj) && ScopeObj && ScopeObj->IsValid())
	{
		(*ScopeObj)->TryGetStringField(TEXT("type"), ScopeType);
		(*ScopeObj)->TryGetStringField(TEXT("path"), ScopePath);
	}

	const TSharedPtr<FJsonObject>* Conditions = nullptr;
	Payload->TryGetObjectField(TEXT("conditions"), Conditions);

	auto ReadNumber = [](const TSharedPtr<FJsonObject>* Obj, const TCHAR* Key, double DefaultValue) -> double
	{
		if (!Obj || !Obj->IsValid())
		{
			return DefaultValue;
		}
		double Val = DefaultValue;
		(*Obj)->TryGetNumberField(Key, Val);
		return Val;
	};

	auto ReadBool = [](const TSharedPtr<FJsonObject>* Obj, const TCHAR* Key, bool DefaultValue) -> bool
	{
		if (!Obj || !Obj->IsValid())
		{
			return DefaultValue;
		}
		bool Val = DefaultValue;
		(*Obj)->TryGetBoolField(Key, Val);
		return Val;
	};

	const double MinTriangles = ReadNumber(Conditions, TEXT("min_triangles"), -1.0);
	const double MinTextureSize = ReadNumber(Conditions, TEXT("min_texture_size"), -1.0);
	const double MaxTextureSize = ReadNumber(Conditions, TEXT("max_texture_size"), -1.0);
	const double ShaderComplexityIdx = ReadNumber(Conditions, TEXT("shader_complexity_index"), -1.0);
	const bool bMissingCollisionOnly = ReadBool(Conditions, TEXT("missing_collision"), false);
	const bool bNaniteEnabled = ReadBool(Conditions, TEXT("nanite_enabled"), true); // 默认允许 Nanite，false 表示筛未开启
	const bool bShadowCasting = ReadBool(Conditions, TEXT("shadow_casting"), false);
	FString ClassFilter;
	if (Conditions && Conditions->IsValid())
	{
		(*Conditions)->TryGetStringField(TEXT("class_filter"), ClassFilter);
	}

	FString SortBy;
	Payload->TryGetStringField(TEXT("sort_by"), SortBy);
	int32 Limit = 20;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	if (Limit <= 0)
	{
		Limit = 20;
	}

	// 2) 收集目标 Actor
	TArray<AActor*> Candidates;
	UWorld* World = UAL_CommandUtils::GetTargetWorld();
	if (!World)
	{
		UAL_CommandUtils::SendError(RequestId, 500, TEXT("No world available"));
		return;
	}

	if (ScopeType.Equals(TEXT("Level"), ESearchCase::IgnoreCase))
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			Candidates.Add(*It);
		}
	}
#if WITH_EDITOR
	else if (ScopeType.Equals(TEXT("Selection"), ESearchCase::IgnoreCase))
	{
		if (GEditor)
		{
			for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
			{
				if (AActor* Selected = Cast<AActor>(*It))
				{
					Candidates.Add(Selected);
				}
			}
		}
	}
#endif
	else
	{
		UAL_CommandUtils::SendError(RequestId, 400, TEXT("Scope type not supported yet"));
		return;
	}

	// 3) 过滤与统计
	TArray<FQueryItem> Results;
	for (AActor* Actor : Candidates)
	{
		if (!Actor)
		{
			continue;
		}

		if (!ClassFilter.IsEmpty())
		{
			if (!Actor->GetClass()->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		UStaticMeshComponent* SMC = Actor->FindComponentByClass<UStaticMeshComponent>();
		if (!SMC)
		{
			continue;
		}

		UStaticMesh* Mesh = SMC->GetStaticMesh();
		if (!Mesh)
		{
			continue;
		}

		FQueryItem Item;
		Item.Name = Actor->GetActorLabel();
		Item.Path = Mesh->GetPathName();
		Item.Type = TEXT("StaticMesh");

		// Triangles
		if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
		{
			if (RenderData->LODResources.Num() > 0)
			{
				Item.Triangles = RenderData->LODResources[0].GetNumTriangles();
			}
		}

		// Disk Size
		Item.DiskSize = Mesh->GetResourceSizeBytes(EResourceSizeMode::Exclusive);

		// Nanite
#if ENGINE_MAJOR_VERSION >= 5
		Item.bNanite = Mesh->HasValidNaniteData();
#endif

		// Collision
		if (const UBodySetup* BodySetup = Mesh->GetBodySetup())
		{
			const ECollisionTraceFlag TraceFlag = BodySetup->GetCollisionTraceFlag();
			const bool bHasSimple = BodySetup->AggGeom.GetElementCount() > 0;
			const bool bUsesComplex = TraceFlag == CTF_UseComplexAsSimple;
			Item.bMissingCollision = !bHasSimple && !bUsesComplex;
		}
		else
		{
			Item.bMissingCollision = true;
		}

		// Shadow
		Item.bCastsShadow = SMC->CastShadow;

		// 条件过滤
		if (MinTriangles >= 0 && Item.Triangles >= 0 && Item.Triangles < MinTriangles)
		{
			continue;
		}
		if (bMissingCollisionOnly && !Item.bMissingCollision)
		{
			continue;
		}
		if (!bNaniteEnabled && Item.bNanite)
		{
			continue;
		}
		if (bShadowCasting && !Item.bCastsShadow)
		{
			continue;
		}

		Results.Add(MoveTemp(Item));
	}

	// 4) 排序
	Results.StableSort([&](const FQueryItem& A, const FQueryItem& B)
	{
		auto GetKey = [&](const FQueryItem& X) -> int64
		{
			if (SortBy.Equals(TEXT("TriangleCount"), ESearchCase::IgnoreCase))
			{
				return X.Triangles;
			}
			if (SortBy.Equals(TEXT("DiskSize"), ESearchCase::IgnoreCase))
			{
				return X.DiskSize;
			}
			return X.Triangles;
		};
		return GetKey(A) > GetKey(B);
	});

	if (Limit > 0 && Results.Num() > Limit)
	{
		Results.SetNum(Limit);
	}

	// 5) 构建响应
	TArray<TSharedPtr<FJsonValue>> AssetsJson;
	for (const FQueryItem& Item : Results)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Item.Name);
		Obj->SetStringField(TEXT("path"), Item.Path);
		Obj->SetStringField(TEXT("type"), Item.Type);

		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		if (Item.Triangles >= 0) Stats->SetNumberField(TEXT("triangles"), Item.Triangles);
		Stats->SetNumberField(TEXT("disk_size"), Item.DiskSize);
		Stats->SetBoolField(TEXT("nanite"), Item.bNanite);
		Stats->SetBoolField(TEXT("missing_collision"), Item.bMissingCollision);
		Stats->SetBoolField(TEXT("shadow_casting"), Item.bCastsShadow);
		Obj->SetObjectField(TEXT("stats"), Stats);

		TArray<FString> Tips;
		if (MinTriangles >= 0 && Item.Triangles > MinTriangles && !Item.bNanite)
		{
			Tips.Add(FString::Printf(TEXT("High poly (%d). Consider enabling Nanite or reducing LOD."), Item.Triangles));
		}
		if (Item.bMissingCollision)
		{
			Tips.Add(TEXT("Missing collision. Add simple collision or enable complex-as-simple."));
		}
		if (!Item.bCastsShadow && bShadowCasting)
		{
			Tips.Add(TEXT("Shadow casting disabled."));
		}
		if (Tips.Num() > 0)
		{
			Obj->SetStringField(TEXT("suggestion"), FString::Join(Tips, TEXT(" ")));
		}

		AssetsJson.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Results.Num());
	Data->SetArrayField(TEXT("assets"), AssetsJson);

	UAL_CommandUtils::SendResponse(RequestId, 200, Data);
}
