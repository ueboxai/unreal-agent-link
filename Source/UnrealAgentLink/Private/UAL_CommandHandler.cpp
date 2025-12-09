#include "UAL_CommandHandler.h"

#include "UAL_NetworkManager.h"

#include "Async/Async.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Misc/MessageDialog.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Runtime/Launch/Resources/Version.h"

#include "IPythonScriptPlugin.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/RectLight.h"
#include "Camera/CameraActor.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Components/SceneComponent.h"
#include "UObject/SoftObjectPath.h"
#include "JsonObjectConverter.h"
#include "UObject/UnrealType.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "Engine/LevelScriptActor.h"
#include "ScopedTransaction.h"
#include "Algo/Sort.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALCommand, Log, All);

namespace
{
	struct FUALSpawnPreset
	{
		FName Key;
		TSubclassOf<AActor> Class;
		const TCHAR* AssetPath = nullptr; // optional mesh for StaticMeshActor
	};

	bool IsZh()
	{
		FString Name;
		if (const TSharedPtr<const FCulture> Culture = FInternationalization::Get().GetCurrentCulture())
		{
			Name = Culture->GetName();
		}
		return Name.StartsWith(TEXT("zh"));
	}

	FString LStr(const TCHAR* Zh, const TCHAR* En)
	{
		return IsZh() ? Zh : En;
	}

	FText LText(const TCHAR* Zh, const TCHAR* En)
	{
		return FText::FromString(LStr(Zh, En));
	}

	UWorld* GetTargetWorld()
	{
#if WITH_EDITOR
		if (GEditor)
		{
			if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
			{
				return EditorWorld;
			}
		}
#endif
		return GWorld;
	}

	FVector ReadVector(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector& DefaultValue = FVector::ZeroVector)
	{
		const TSharedPtr<FJsonObject>* Sub = nullptr;
		if (!Obj->TryGetObjectField(Field, Sub) || !Sub || !Sub->IsValid())
		{
			return DefaultValue;
		}

		double X = DefaultValue.X, Y = DefaultValue.Y, Z = DefaultValue.Z;
		(*Sub)->TryGetNumberField(TEXT("x"), X);
		(*Sub)->TryGetNumberField(TEXT("y"), Y);
		(*Sub)->TryGetNumberField(TEXT("z"), Z);
		return FVector(X, Y, Z);
	}

	FRotator ReadRotator(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FRotator& DefaultValue = FRotator::ZeroRotator)
	{
		const TSharedPtr<FJsonObject>* Sub = nullptr;
		if (!Obj->TryGetObjectField(Field, Sub) || !Sub || !Sub->IsValid())
		{
			return DefaultValue;
		}

		double Pitch = DefaultValue.Pitch, Yaw = DefaultValue.Yaw, Roll = DefaultValue.Roll;
		(*Sub)->TryGetNumberField(TEXT("pitch"), Pitch);
		(*Sub)->TryGetNumberField(TEXT("yaw"), Yaw);
		(*Sub)->TryGetNumberField(TEXT("roll"), Roll);
		return FRotator(Pitch, Yaw, Roll);
	}

	FVector ReadVectorDirect(const TSharedPtr<FJsonObject>& Obj, const FVector& DefaultValue = FVector::ZeroVector)
	{
		if (!Obj.IsValid())
		{
			return DefaultValue;
		}
		double X = DefaultValue.X, Y = DefaultValue.Y, Z = DefaultValue.Z;
		Obj->TryGetNumberField(TEXT("x"), X);
		Obj->TryGetNumberField(TEXT("y"), Y);
		Obj->TryGetNumberField(TEXT("z"), Z);
		return FVector(X, Y, Z);
	}

	FRotator ReadRotatorDirect(const TSharedPtr<FJsonObject>& Obj, const FRotator& DefaultValue = FRotator::ZeroRotator)
	{
		if (!Obj.IsValid())
		{
			return DefaultValue;
		}
		double Pitch = DefaultValue.Pitch, Yaw = DefaultValue.Yaw, Roll = DefaultValue.Roll;
		Obj->TryGetNumberField(TEXT("pitch"), Pitch);
		Obj->TryGetNumberField(TEXT("yaw"), Yaw);
		Obj->TryGetNumberField(TEXT("roll"), Roll);
		return FRotator(Pitch, Yaw, Roll);
	}

	bool TryGetObjectFieldFlexible(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field, TSharedPtr<FJsonObject>& OutObj)
	{
		const TSharedPtr<FJsonObject>* Sub = nullptr;
		if (Parent->TryGetObjectField(Field, Sub) && Sub && Sub->IsValid())
		{
			OutObj = *Sub;
			return true;
		}

		FString AsString;
		if (Parent->TryGetStringField(Field, AsString) && !AsString.IsEmpty())
		{
			TSharedPtr<FJsonObject> Parsed;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(AsString);
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
			{
				OutObj = Parsed;
				return true;
			}
		}
		return false;
	}

	bool ResolvePreset(const FString& Name, FUALSpawnPreset& OutPreset)
	{
		static const TArray<FUALSpawnPreset> Presets = {
			{ TEXT("cube"), AStaticMeshActor::StaticClass(), TEXT("/Engine/BasicShapes/Cube.Cube") },
			{ TEXT("sphere"), AStaticMeshActor::StaticClass(), TEXT("/Engine/BasicShapes/Sphere.Sphere") },
			{ TEXT("cylinder"), AStaticMeshActor::StaticClass(), TEXT("/Engine/BasicShapes/Cylinder.Cylinder") },
			{ TEXT("cone"), AStaticMeshActor::StaticClass(), TEXT("/Engine/BasicShapes/Cone.Cone") },
			{ TEXT("plane"), AStaticMeshActor::StaticClass(), TEXT("/Engine/BasicShapes/Plane.Plane") },
			{ TEXT("point_light"), APointLight::StaticClass(), nullptr },
			{ TEXT("spot_light"), ASpotLight::StaticClass(), nullptr },
			{ TEXT("directional_light"), ADirectionalLight::StaticClass(), nullptr },
			{ TEXT("rect_light"), ARectLight::StaticClass(), nullptr },
			{ TEXT("camera"), ACameraActor::StaticClass(), nullptr },
		};

		for (const FUALSpawnPreset& Preset : Presets)
		{
			if (Preset.Key == FName(*Name))
			{
				OutPreset = Preset;
				return true;
			}
		}
		return false;
	}

	bool SetStaticMeshIfNeeded(AActor* Actor, const TCHAR* MeshPath)
	{
		if (!MeshPath || !Actor)
		{
			return true;
		}

		AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor);
		if (!MeshActor)
		{
			return true;
		}

		FSoftObjectPath MeshAssetPath(MeshPath);
		if (!MeshAssetPath.IsValid())
		{
			return false;
		}

		UObject* LoadedObj = MeshAssetPath.TryLoad();
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(LoadedObj);
		if (!StaticMesh)
		{
			return false;
		}

		MeshActor->GetStaticMeshComponent()->SetStaticMesh(StaticMesh);
		return true;
	}

	// 前置声明，供解析函数使用
	UClass* ResolveClassFromIdentifier(const FString& Identifier, UClass* ExpectedBase, FString& OutError);

	struct FUALResolvedSpawnRequest
	{
		TSubclassOf<AActor> SpawnClass = nullptr;
		FString MeshPath;
		FString ResolvedType;
		FString SourceId;
		bool bFromAlias = false;
	};

	bool ResolveSpawnFromAssetId(const FString& AssetId, FUALResolvedSpawnRequest& OutResolved, FString& OutError)
	{
		OutResolved = FUALResolvedSpawnRequest();
		OutError.Reset();

		if (AssetId.IsEmpty())
		{
			OutError = TEXT("asset_id is empty");
			return false;
		}

		// Level 1：别名（沿用 preset 表）
		FUALSpawnPreset Preset;
		if (ResolvePreset(AssetId, Preset))
		{
			OutResolved.SpawnClass = Preset.Class;
			if (Preset.AssetPath)
			{
				OutResolved.MeshPath = Preset.AssetPath;
			}
			OutResolved.ResolvedType = Preset.Class ? Preset.Class->GetName() : TEXT("Preset");
			OutResolved.SourceId = AssetId;
			OutResolved.bFromAlias = true;
			return true;
		}

		// Level 2/3：资源路径（静态网格 / 蓝图 / 原生类）
		if (AssetId.StartsWith(TEXT("/")))
		{
			FSoftObjectPath SoftObjPath(AssetId);
			FSoftClassPath SoftClassPath(AssetId);

			UObject* LoadedObj = SoftObjPath.IsValid() ? SoftObjPath.TryLoad() : nullptr;

			// _C 等 Class 路径优先走 TryLoadClass（兼容 5.0-5.7）
			if (UClass* LoadedClass = SoftClassPath.IsValid() ? SoftClassPath.TryLoadClass<AActor>() : nullptr)
			{
				OutResolved.SpawnClass = LoadedClass;
				OutResolved.ResolvedType = LoadedClass->GetName();
				OutResolved.SourceId = AssetId;
				return true;
			}

			if (LoadedObj)
			{
				// 蓝图资产
				if (UBlueprint* BP = Cast<UBlueprint>(LoadedObj))
				{
					if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(AActor::StaticClass()))
					{
						OutResolved.SpawnClass = BP->GeneratedClass;
						OutResolved.ResolvedType = BP->GeneratedClass->GetName();
						OutResolved.SourceId = AssetId;
						return true;
					}
				}

				// 直接类资产
				if (UClass* AsClass = Cast<UClass>(LoadedObj))
				{
					if (AsClass->IsChildOf(AActor::StaticClass()))
					{
						OutResolved.SpawnClass = AsClass;
						OutResolved.ResolvedType = AsClass->GetName();
						OutResolved.SourceId = AssetId;
						return true;
					}
				}

				// 静态网格 -> AStaticMeshActor + 绑定 Mesh
				if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(LoadedObj))
				{
					OutResolved.SpawnClass = AStaticMeshActor::StaticClass();
					OutResolved.MeshPath = AssetId;
					OutResolved.ResolvedType = TEXT("StaticMeshActor");
					OutResolved.SourceId = AssetId;
					return true;
				}
			}

			OutError = FString::Printf(TEXT("Unsupported asset type or failed to load: %s"), *AssetId);
			return false;
		}

		// Fallback：将 asset_id 视作类名
		FString ClassError;
		if (UClass* ResolvedClass = ResolveClassFromIdentifier(AssetId, AActor::StaticClass(), ClassError))
		{
			OutResolved.SpawnClass = ResolvedClass;
			OutResolved.ResolvedType = ResolvedClass->GetName();
			OutResolved.SourceId = AssetId;
			return true;
		}

		OutError = ClassError.IsEmpty() ? FString::Printf(TEXT("Failed to resolve asset_id: %s"), *AssetId) : ClassError;
		return false;
	}

	void ReadTransformFromItem(const TSharedPtr<FJsonObject>& Item, FVector& OutLocation, FRotator& OutRotation, FVector& OutScale)
	{
		// 顶层兼容旧字段
		OutLocation = ReadVector(Item, TEXT("location"), OutLocation);
		OutRotation = ReadRotator(Item, TEXT("rotation"), OutRotation);
		OutScale = ReadVector(Item, TEXT("scale"), OutScale);

		// 新字段：transform { location/rotation/scale }
		const TSharedPtr<FJsonObject>* TransformObj = nullptr;
		if (Item->TryGetObjectField(TEXT("transform"), TransformObj) && TransformObj && TransformObj->IsValid())
		{
			TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
			if (TryGetObjectFieldFlexible(*TransformObj, TEXT("location"), LocObj))
			{
				OutLocation = ReadVectorDirect(LocObj, OutLocation);
			}
			if (TryGetObjectFieldFlexible(*TransformObj, TEXT("rotation"), RotObj))
			{
				OutRotation = ReadRotatorDirect(RotObj, OutRotation);
			}
			if (TryGetObjectFieldFlexible(*TransformObj, TEXT("scale"), ScaleObj))
			{
				OutScale = ReadVectorDirect(ScaleObj, OutScale);
			}
		}
	}

	AActor* FindActorByLabel(UWorld* World, const FString& Label)
	{
		if (!World || Label.IsEmpty())
		{
			return nullptr;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
#if WITH_EDITOR
			if (Actor && Actor->GetActorLabel() == Label)
			{
				return Actor;
			}
#else
			if (Actor && Actor->GetName() == Label)
			{
				return Actor;
			}
#endif
		}
		return nullptr;
	}

	FString GetActorFriendlyName(AActor* Actor)
	{
		if (!Actor)
		{
			return FString();
		}
#if WITH_EDITOR
		return Actor->GetActorLabel();
#else
		return Actor->GetName();
#endif
	}

	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Vec)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Vec.X);
		Obj->SetNumberField(TEXT("y"), Vec.Y);
		Obj->SetNumberField(TEXT("z"), Vec.Z);
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Rot)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Rot.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Rot.Yaw);
		Obj->SetNumberField(TEXT("roll"), Rot.Roll);
		return Obj;
	}

	// 兼容 UE5.0-5.7 的属性转 Json 接口差异
	TSharedPtr<FJsonValue> PropertyToJsonValueCompat(FProperty* Prop, const void* ValuePtr)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
		TSharedPtr<FJsonValue> JsonValue;
		if (FJsonObjectConverter::PropertyToJsonValue(Prop, ValuePtr, 0, 0, JsonValue) && JsonValue.IsValid())
		{
			return JsonValue;
		}
		return nullptr;
#else
		return FJsonObjectConverter::UPropertyToJsonValue(Prop, ValuePtr, 0, 0, nullptr, nullptr);
#endif
	}

	TSharedPtr<FJsonObject> BuildActorInfo(AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), GetActorFriendlyName(Actor));
		Obj->SetStringField(TEXT("path"), Actor->GetPathName());
		Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		return Obj;
	}

	TSharedPtr<FJsonObject> BuildActorInfoWithOptions(AActor* Actor, bool bIncludeTransform, bool bIncludeBounds)
	{
		TSharedPtr<FJsonObject> Obj = BuildActorInfo(Actor);
		if (!Obj.IsValid())
		{
			return nullptr;
		}

		if (bIncludeTransform)
		{
			TSharedPtr<FJsonObject> TransformObj = MakeShared<FJsonObject>();
			TransformObj->SetObjectField(TEXT("location"), MakeVectorJson(Actor->GetActorLocation()));
			TransformObj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor->GetActorRotation()));
			TransformObj->SetObjectField(TEXT("scale"), MakeVectorJson(Actor->GetActorScale3D()));
			Obj->SetObjectField(TEXT("transform"), TransformObj);
		}

		if (bIncludeBounds)
		{
			const FBox Bounds = Actor->GetComponentsBoundingBox(true);
			const FVector Size = Bounds.IsValid ? Bounds.GetSize() : FVector::ZeroVector;
			Obj->SetObjectField(TEXT("bounds"), MakeVectorJson(Size));
		}

		return Obj;
	}

	bool ShouldIncludeActor(const AActor* Actor, const FString& NameKeyword, bool bNameExact, const FString& ClassKeyword, bool bClassExact)
	{
		if (!Actor)
		{
			return false;
		}

		if (!NameKeyword.IsEmpty())
		{
			const FString Name = GetActorFriendlyName(const_cast<AActor*>(Actor));
			const bool bMatchName = bNameExact
				? Name.Equals(NameKeyword, ESearchCase::IgnoreCase)
				: Name.Contains(NameKeyword, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			if (!bMatchName)
			{
				return false;
			}
		}

		if (!ClassKeyword.IsEmpty())
		{
			const FString ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
			const bool bMatchClass = bClassExact
				? ClassName.Equals(ClassKeyword, ESearchCase::IgnoreCase)
				: ClassName.Contains(ClassKeyword, ESearchCase::IgnoreCase, ESearchDir::FromStart);
			if (!bMatchClass)
			{
				return false;
			}
		}

		return true;
	}

	bool ShouldIncludeActorAdvanced(
		const AActor* Actor,
		const FString& NameContains,
		const FString& NameNotContains,
		const FString& ClassContains,
		const FString& ClassNotContains,
		const FString& ClassExact,
		const TArray<FString>& ExcludeClasses)
	{
		if (!Actor)
		{
			return false;
		}

		const FString Name = GetActorFriendlyName(const_cast<AActor*>(Actor));
		const FString ClassName = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();

		auto ContainsIgnoreCase = [](const FString& Src, const FString& Pattern)
		{
			return Src.Contains(Pattern, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		};

		if (!ClassExact.IsEmpty() && !ClassName.Equals(ClassExact, ESearchCase::IgnoreCase))
		{
			return false;
		}

		if (!ClassContains.IsEmpty() && !ContainsIgnoreCase(ClassName, ClassContains))
		{
			return false;
		}
		if (!ClassNotContains.IsEmpty() && ContainsIgnoreCase(ClassName, ClassNotContains))
		{
			return false;
		}

		for (const FString& Exclude : ExcludeClasses)
		{
			if (ClassName.Equals(Exclude, ESearchCase::IgnoreCase))
			{
				return false;
			}
		}

		if (!NameContains.IsEmpty() && !ContainsIgnoreCase(Name, NameContains))
		{
			return false;
		}
		if (!NameNotContains.IsEmpty() && ContainsIgnoreCase(Name, NameNotContains))
		{
			return false;
		}

		return true;
	}

	UClass* ResolveClassFromIdentifier(const FString& Identifier, UClass* ExpectedBase, FString& OutError)
	{
		if (Identifier.IsEmpty())
		{
			OutError = TEXT("Class identifier is empty");
			return nullptr;
		}

		UClass* ResolvedClass = nullptr;

		// 支持完整路径（/Script/...）或短类名
		if (Identifier.StartsWith(TEXT("/")))
		{
			const FSoftClassPath SoftPath(Identifier);
			ResolvedClass = SoftPath.TryLoadClass<UObject>();
		}
		else
		{
			ResolvedClass = FindObject<UClass>(ANY_PACKAGE, *Identifier);
			if (!ResolvedClass)
			{
				const FString WithUPrefix = FString::Printf(TEXT("U%s"), *Identifier);
				ResolvedClass = FindObject<UClass>(ANY_PACKAGE, *WithUPrefix);
			}
		}

		if (!ResolvedClass)
		{
			OutError = FString::Printf(TEXT("Class not found: %s"), *Identifier);
			return nullptr;
		}

		if (ExpectedBase && !ResolvedClass->IsChildOf(ExpectedBase))
		{
			OutError = FString::Printf(TEXT("%s is not a subclass of %s"), *Identifier, *ExpectedBase->GetName());
			return nullptr;
		}

		return ResolvedClass;
	}

	bool ResolveTargetsToActors(const TSharedPtr<FJsonObject>& Targets, UWorld* World, TSet<AActor*>& OutSet, FString& OutError)
	{
		OutSet.Reset();
		OutError.Reset();

		if (!Targets.IsValid())
		{
			OutError = TEXT("Missing object: targets");
			return false;
		}

		// 标记用户是否明确提供了 names 或 paths（用于判断是否应该回退到全量筛选）
		bool bHasExplicitTargets = false;

		// names
		const TArray<TSharedPtr<FJsonValue>>* NamesArr = nullptr;
		if (Targets->TryGetArrayField(TEXT("names"), NamesArr) && NamesArr && NamesArr->Num() > 0)
		{
			bHasExplicitTargets = true;
			for (const TSharedPtr<FJsonValue>& Val : *NamesArr)
			{
				FString Name;
				if (Val.IsValid() && Val->TryGetString(Name))
				{
					if (AActor* Actor = FindActorByLabel(World, Name))
					{
						OutSet.Add(Actor);
					}
				}
			}
		}

		// paths
		const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
		if (Targets->TryGetArrayField(TEXT("paths"), PathsArr) && PathsArr && PathsArr->Num() > 0)
		{
			bHasExplicitTargets = true;
			for (const TSharedPtr<FJsonValue>& Val : *PathsArr)
			{
				FString Path;
				if (Val.IsValid() && Val->TryGetString(Path))
				{
					if (AActor* Actor = Cast<AActor>(StaticFindObject(AActor::StaticClass(), nullptr, *Path)))
					{
						OutSet.Add(Actor);
					}
				}
			}
		}

		// filter
		const TSharedPtr<FJsonObject>* FilterObj = nullptr;
		Targets->TryGetObjectField(TEXT("filter"), FilterObj);
		FString FilterClassContains, FilterNamePattern;
		TArray<FString> FilterExcludeClasses;
		bool bHasFilter = false;
		if (FilterObj && (*FilterObj).IsValid())
		{
			(*FilterObj)->TryGetStringField(TEXT("class"), FilterClassContains);
			(*FilterObj)->TryGetStringField(TEXT("name_pattern"), FilterNamePattern);
			const TArray<TSharedPtr<FJsonValue>>* Excl = nullptr;
			if ((*FilterObj)->TryGetArrayField(TEXT("exclude_classes"), Excl) && Excl)
			{
				for (const TSharedPtr<FJsonValue>& V : *Excl)
				{
					FString S;
					if (V.IsValid() && V->TryGetString(S))
					{
						FilterExcludeClasses.Add(S);
					}
				}
			}
			// 检查是否实际设置了任何过滤条件
			bHasFilter = !FilterClassContains.IsEmpty() || !FilterNamePattern.IsEmpty() || FilterExcludeClasses.Num() > 0;
		}

		auto MatchFilter = [&](AActor* Actor) -> bool
		{
			if (!Actor) return false;
			if (!FilterClassContains.IsEmpty())
			{
				const FString Cls = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
				if (!Cls.Contains(FilterClassContains, ESearchCase::IgnoreCase, ESearchDir::FromStart))
				{
					return false;
				}
			}
			if (!FilterNamePattern.IsEmpty())
			{
				const FString Nm = GetActorFriendlyName(Actor);
				if (!Nm.MatchesWildcard(FilterNamePattern))
				{
					return false;
				}
			}
			for (const FString& Ex : FilterExcludeClasses)
			{
				const FString Cls = Actor->GetClass() ? Actor->GetClass()->GetName() : FString();
				if (Cls.Equals(Ex, ESearchCase::IgnoreCase))
				{
					return false;
				}
			}
			return true;
		};

		// 如果 names/paths 非空，则在该集合上再按 filter 交叉筛选
		// 【关键修复】如果用户明确提供了 names/paths，但未找到匹配，则不回退到全量筛选
		if (OutSet.Num() > 0)
		{
			// 在已选集合上应用 filter
			if (bHasFilter)
			{
				for (auto It = OutSet.CreateIterator(); It; ++It)
				{
					if (!MatchFilter(*It))
					{
						It.RemoveCurrent();
					}
				}
			}
		}
		else if (bHasExplicitTargets)
		{
			// 【关键修复】用户提供了 names/paths，但没有找到任何匹配的 Actor
			// 这种情况下不应该回退到全量筛选，而应该直接返回空集合
			OutError = TEXT("No actor found matching the specified names/paths");
			return false;
		}
		else if (bHasFilter)
		{
			// 只有在用户 仅提供 filter（没有 names/paths）时，才进行全量筛选
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor && MatchFilter(Actor))
				{
					OutSet.Add(Actor);
				}
			}
		}
		else
		{
			// 用户既没有提供 names/paths，也没有提供有效的 filter
			OutError = TEXT("No valid selector provided: must specify names, paths, or filter");
			return false;
		}

		if (OutSet.Num() == 0)
		{
			OutError = TEXT("No actor matched targets");
			return false;
		}

		return true;
	}

	bool ApplyStructValue(FStructProperty* StructProp, UObject* Target, const TSharedPtr<FJsonValue>& JsonValue)
	{
		if (!StructProp || !Target || !JsonValue.IsValid() || JsonValue->Type != EJson::Object)
		{
			return false;
		}

		const TSharedPtr<FJsonObject> Obj = JsonValue->AsObject();
		void* StructPtr = StructProp->ContainerPtrToValuePtr<void>(Target);
		if (!StructPtr || !Obj.IsValid())
		{
			return false;
		}

		const FName StructName = StructProp->Struct ? StructProp->Struct->GetFName() : NAME_None;
		if (StructName == TBaseStructure<FVector>::Get()->GetFName())
		{
			double X = 0, Y = 0, Z = 0;
			Obj->TryGetNumberField(TEXT("x"), X);
			Obj->TryGetNumberField(TEXT("y"), Y);
			Obj->TryGetNumberField(TEXT("z"), Z);
			*static_cast<FVector*>(StructPtr) = FVector(X, Y, Z);
			return true;
		}
		if (StructName == TBaseStructure<FRotator>::Get()->GetFName())
		{
			double Pitch = 0, Yaw = 0, Roll = 0;
			Obj->TryGetNumberField(TEXT("pitch"), Pitch);
			Obj->TryGetNumberField(TEXT("yaw"), Yaw);
			Obj->TryGetNumberField(TEXT("roll"), Roll);
			*static_cast<FRotator*>(StructPtr) = FRotator(Pitch, Yaw, Roll);
			return true;
		}
		
		return false;
	}

	const TArray<FString>& GetDefaultInspectProps()
	{
		static const TArray<FString> Defaults = {
			TEXT("Mobility"),
			TEXT("bHidden"),
			TEXT("CollisionProfileName"),
			TEXT("Tags")
		};
		return Defaults;
	}

	bool TryCollectProperty(UObject* Obj, const FString& PropName, TSharedPtr<FJsonObject>& OutProps)
	{
		if (!Obj || !OutProps.IsValid())
		{
			return false;
		}

		FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), *PropName);
		if (!Prop)
		{
			return false;
		}

		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly | CPF_DisableEditOnInstance))
		{
			return false;
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
		if (!ValuePtr)
		{
			return false;
		}

		TSharedPtr<FJsonValue> JsonValue = PropertyToJsonValueCompat(Prop, ValuePtr);
		if (!JsonValue.IsValid())
		{
			return false;
		}

		OutProps->SetField(PropName, JsonValue);
		return true;
	}

	void CollectPropertyNames(UObject* Obj, TArray<FString>& OutNames)
	{
		if (!Obj)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop)
			{
				continue;
			}
			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly | CPF_DisableEditOnInstance))
			{
				continue;
			}
			if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly))
			{
				continue;
			}
			OutNames.AddUnique(Prop->GetName());
		}
	}

	int32 LevenshteinDistance(const FString& A, const FString& B)
	{
		const int32 LenA = A.Len();
		const int32 LenB = B.Len();
		TArray<int32> Prev, Curr;
		Prev.SetNum(LenB + 1);
		Curr.SetNum(LenB + 1);

		for (int32 j = 0; j <= LenB; ++j)
		{
			Prev[j] = j;
		}

		for (int32 i = 1; i <= LenA; ++i)
		{
			Curr[0] = i;
			for (int32 j = 1; j <= LenB; ++j)
			{
				const int32 Cost = (FChar::ToLower(A[i - 1]) == FChar::ToLower(B[j - 1])) ? 0 : 1;
				Curr[j] = FMath::Min3(
					Curr[j - 1] + 1,
					Prev[j] + 1,
					Prev[j - 1] + Cost
				);
			}
			Swap(Prev, Curr);
		}
		return Prev[LenB];
	}

	void SuggestProperties(const FString& Input, const TArray<FString>& Candidates, TArray<FString>& OutSuggestions, int32 MaxSuggestions = 5)
	{
		struct FScore
		{
			FString Name;
			int32 Distance = 0;
		};

		TArray<FScore> Scores;
		for (const FString& Cand : Candidates)
		{
			FScore S;
			S.Name = Cand;
			S.Distance = LevenshteinDistance(Input, Cand);
			Scores.Add(S);
		}

		Scores.Sort([](const FScore& L, const FScore& R)
		{
			if (L.Distance == R.Distance)
			{
				return L.Name < R.Name;
			}
			return L.Distance < R.Distance;
		});

		for (int32 i = 0; i < Scores.Num() && OutSuggestions.Num() < MaxSuggestions; ++i)
		{
			OutSuggestions.Add(Scores[i].Name);
		}
	}

	FProperty* FindWritableProperty(UObject* Obj, const FString& PropName)
	{
		if (!Obj)
		{
			return nullptr;
		}
		FProperty* Prop = FindFProperty<FProperty>(Obj->GetClass(), *PropName);
		if (!Prop)
		{
			return nullptr;
		}
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly | CPF_DisableEditOnInstance))
		{
			return nullptr;
		}
		if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly))
		{
			return nullptr;
		}
		return Prop;
	}

	FProperty* FindWritablePropertyOnActorHierarchy(AActor* Actor, const FString& PropName, UObject*& OutTargetObj)
	{
		OutTargetObj = nullptr;
		if (!Actor)
		{
			return nullptr;
		}

		// 1) Actor 自身
		if (FProperty* Prop = FindWritableProperty(Actor, PropName))
		{
			OutTargetObj = Actor;
			return Prop;
		}

		// 2) RootComponent
		if (USceneComponent* RootComp = Actor->GetRootComponent())
		{
			if (FProperty* Prop = FindWritableProperty(RootComp, PropName))
			{
				OutTargetObj = RootComp;
				return Prop;
			}
		}

		// 3) 其他组件
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			if (!Comp)
			{
				continue;
			}
			if (FProperty* Prop = FindWritableProperty(Comp, PropName))
			{
				OutTargetObj = Comp;
				return Prop;
			}
		}

		return nullptr;
	}

	bool SetNumericProperty(FNumericProperty* NumProp, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!NumProp || !Obj || !Value.IsValid())
		{
			return false;
		}
		if (Value->Type != EJson::Number)
		{
			OutError = TEXT("expects a number");
			return false;
		}
		const double Num = Value->AsNumber();
		void* Ptr = NumProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		if (NumProp->IsInteger())
		{
			NumProp->SetIntPropertyValue(Ptr, static_cast<int64>(Num));
		}
		else
		{
			NumProp->SetFloatingPointPropertyValue(Ptr, Num);
		}
		return true;
	}

	bool SetStructProperty(FStructProperty* StructProp, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!StructProp || !Obj || !Value.IsValid())
		{
			return false;
		}

		if (StructProp->Struct == TBaseStructure<FVector>::Get() ||
			StructProp->Struct == TBaseStructure<FRotator>::Get())
		{
			if (!ApplyStructValue(StructProp, Obj, Value))
			{
				OutError = TEXT("expects object with matching fields");
				return false;
			}
			return true;
		}

		if (StructProp->Struct == TBaseStructure<FLinearColor>::Get())
		{
			if (Value->Type != EJson::Object)
			{
				OutError = TEXT("expects object with r/g/b(/a)");
				return false;
			}
			const TSharedPtr<FJsonObject> ObjVal = Value->AsObject();
			double R = 0, G = 0, B = 0, A = 1.0;
			ObjVal->TryGetNumberField(TEXT("r"), R);
			ObjVal->TryGetNumberField(TEXT("g"), G);
			ObjVal->TryGetNumberField(TEXT("b"), B);
			ObjVal->TryGetNumberField(TEXT("a"), A);
			void* Ptr = StructProp->ContainerPtrToValuePtr<void>(Obj);
			if (!Ptr)
			{
				return false;
			}
			*static_cast<FLinearColor*>(Ptr) = FLinearColor(R, G, B, A);
			return true;
		}

		if (StructProp->Struct == TBaseStructure<FColor>::Get())
		{
			if (Value->Type != EJson::Object)
			{
				OutError = TEXT("expects object with r/g/b(/a)");
				return false;
			}
			const TSharedPtr<FJsonObject> ObjVal = Value->AsObject();
			int32 R = 0, G = 0, B = 0, A = 255;
			ObjVal->TryGetNumberField(TEXT("r"), R);
			ObjVal->TryGetNumberField(TEXT("g"), G);
			ObjVal->TryGetNumberField(TEXT("b"), B);
			ObjVal->TryGetNumberField(TEXT("a"), A);
			void* Ptr = StructProp->ContainerPtrToValuePtr<void>(Obj);
			if (!Ptr)
			{
				return false;
			}
			*static_cast<FColor*>(Ptr) = FColor(R, G, B, A);
			return true;
		}

		OutError = FString::Printf(TEXT("unsupported struct type: %s"), *StructProp->Struct->GetName());
		return false;
	}

	bool SetSimpleProperty(FProperty* Prop, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		if (!Prop || !Obj || !Value.IsValid())
		{
			return false;
		}

		if (FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
		{
			return SetNumericProperty(NumProp, Obj, Value, OutError);
		}

		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			if (Value->Type != EJson::Boolean)
			{
				OutError = TEXT("expects a boolean");
				return false;
			}
			void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Obj);
			if (!Ptr)
			{
				return false;
			}
			BoolProp->SetPropertyValue(Ptr, Value->AsBool());
			return true;
		}

		if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			if (Value->Type != EJson::String)
			{
				OutError = TEXT("expects a string");
				return false;
			}
			void* Ptr = StrProp->ContainerPtrToValuePtr<void>(Obj);
			if (!Ptr)
			{
				return false;
			}
			StrProp->SetPropertyValue(Ptr, Value->AsString());
			return true;
		}

		if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			if (Value->Type != EJson::String)
			{
				OutError = TEXT("expects a string");
				return false;
			}
			void* Ptr = NameProp->ContainerPtrToValuePtr<void>(Obj);
			if (!Ptr)
			{
				return false;
			}
			NameProp->SetPropertyValue(Ptr, FName(*Value->AsString()));
			return true;
		}

		if (FTextProperty* TextProp = CastField<FTextProperty>(Prop))
		{
			if (Value->Type != EJson::String)
			{
				OutError = TEXT("expects a string");
				return false;
			}
			void* Ptr = TextProp->ContainerPtrToValuePtr<void>(Obj);
			if (!Ptr)
			{
				return false;
			}
			TextProp->SetPropertyValue(Ptr, FText::FromString(Value->AsString()));
			return true;
		}

		if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			return SetStructProperty(StructProp, Obj, Value, OutError);
		}

		OutError = FString::Printf(TEXT("unsupported property type: %s"), *Prop->GetClass()->GetName());
		return false;
	}

	FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer, false);
		Writer->Close();
		return Out;
	}

	TSharedPtr<FJsonObject> BuildSelectedProps(AActor* Actor, const TArray<FString>& WantedProps)
	{
		if (!Actor)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();

		auto TryCollect = [&](UObject* Obj, const FString& PropName) -> bool
		{
			return TryCollectProperty(Obj, PropName, Props);
		};

		for (const FString& PropName : WantedProps)
		{
			if (PropName.IsEmpty())
			{
				continue;
			}

			// 优先 Actor 自身
			if (TryCollect(Actor, PropName))
			{
				continue;
			}

			// 然后 RootComponent
			if (USceneComponent* RootComp = Actor->GetRootComponent())
			{
				if (TryCollect(RootComp, PropName))
				{
					continue;
				}
			}

			// 其他组件（找到一个就停）
			for (UActorComponent* Comp : Actor->GetComponents())
			{
				if (!Comp)
				{
					continue;
				}
				if (TryCollect(Comp, PropName))
				{
					break;
				}
			}
		}

		return Props;
	}
}

FUAL_CommandHandler::FUAL_CommandHandler()
{
	RegisterCommands();
}

void FUAL_CommandHandler::ProcessMessage(const FString& JsonPayload)
{
	if (!IsInGameThread())
	{
		AsyncTask(ENamedThreads::GameThread, [this, Payload = JsonPayload]()
		{
			ProcessMessage(Payload);
		});
		return;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogUALCommand, Warning, TEXT("Invalid JSON payload: %s"), *JsonPayload);
		return;
	}

	FString Type, Method, RequestId;
	Root->TryGetStringField(TEXT("type"), Type);
	Root->TryGetStringField(TEXT("method"), Method);
	Root->TryGetStringField(TEXT("id"), RequestId);

	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	if (Type == TEXT("req"))
	{
		// JSON-RPC 风格：params；兼容旧字段 payload
		if (!Root->TryGetObjectField(TEXT("params"), ParamsObj))
		{
			Root->TryGetObjectField(TEXT("payload"), ParamsObj);
		}
	}
	else if (Type == TEXT("res"))
	{
		// JSON-RPC 风格：result；兼容旧字段 data
		if (!Root->TryGetObjectField(TEXT("result"), ParamsObj))
		{
			Root->TryGetObjectField(TEXT("data"), ParamsObj);
		}
	}

	UE_LOG(LogUALCommand, Display, TEXT("Recv message type=%s method=%s id=%s"), *Type, *Method, *RequestId);

	if (Type != TEXT("req"))
	{
		if (Type == TEXT("res"))
		{
			Handle_Response(Method, ParamsObj ? *ParamsObj : nullptr);
		}
		else
		{
			UE_LOG(LogUALCommand, Verbose, TEXT("Ignore non-request message: %s"), *Type);
		}
		return;
	}

	const FHandlerFunc* Handler = CommandMap.Find(Method);
	if (!Handler)
	{
		SendError(RequestId, 404, FString::Printf(TEXT("Unknown method: %s"), *Method));
		return;
	}

	(*Handler)(ParamsObj ? *ParamsObj : MakeShared<FJsonObject>(), RequestId);
}

void FUAL_CommandHandler::RegisterCommands()
{
	CommandMap.Add(TEXT("cmd.run_python"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_RunPython(Payload, RequestId);
	});

	CommandMap.Add(TEXT("cmd.exec_console"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_ExecConsole(Payload, RequestId);
	});

	CommandMap.Add(TEXT("project.info"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetProjectInfo(Payload, RequestId);
	});

	CommandMap.Add(TEXT("blueprint.create"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_CreateBlueprint(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.spawn"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SpawnActor(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.spawn_batch"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SpawnActorsBatch(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.destroy"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DestroyActor(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.destroy_batch"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DestroyActorsBatch(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.set_transform"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetTransformUnified(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.set_property"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SetProperty(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.get_info"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetActorInfo(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.get"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_GetActor(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.inspect"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_InspectActor(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.find"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_FindActors(Payload, RequestId);
	});
}

void FUAL_CommandHandler::Handle_RunPython(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Script;
	if (!Payload->TryGetStringField(TEXT("script"), Script))
	{
		SendError(RequestId, 400, TEXT("Missing field: script"));
		return;
	}

	bool bExecuted = false;
#if defined(WITH_PYTHON) && WITH_PYTHON
	if (IPythonScriptPlugin::IsAvailable())
	{
		bExecuted = IPythonScriptPlugin::Get()->ExecPythonCommand(*Script);
	}
#else
	UE_LOG(LogUALCommand, Warning, TEXT("WITH_PYTHON is not enabled; skip exec"));
#endif

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), bExecuted);
	SendResponse(RequestId, bExecuted ? 200 : 500, Data);
}

void FUAL_CommandHandler::Handle_ExecConsole(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Command;
	if (!Payload->TryGetStringField(TEXT("command"), Command))
	{
		SendError(RequestId, 400, TEXT("Missing field: command"));
		return;
	}

	bool bResult = false;
	if (GEngine)
	{
#if WITH_EDITOR
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
#else
		UWorld* World = GWorld;
#endif
		bResult = GEngine->Exec(World, *Command);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("result"), bResult ? TEXT("OK") : TEXT("Failed"));
	SendResponse(RequestId, bResult ? 200 : 500, Data);
}

void FUAL_CommandHandler::Handle_GetProjectInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	TSharedPtr<FJsonObject> Data = BuildProjectInfo();
	SendResponse(RequestId, 200, Data);
}

TSharedPtr<FJsonObject> FUAL_CommandHandler::BuildProjectInfo() const
{
	// 基础路径信息
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	Data->SetStringField(TEXT("projectPath"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Data->SetStringField(TEXT("projectFile"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
	Data->SetStringField(TEXT("contentDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
	Data->SetStringField(TEXT("configDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir()));
	Data->SetStringField(TEXT("savedDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
	Data->SetStringField(TEXT("pluginsDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir()));

	// 版本信息（兼容 5.0-5.7，直接读配置）
	FString ProjectVersion;
	GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), ProjectVersion, GGameIni);
	if (!ProjectVersion.IsEmpty())
	{
		Data->SetStringField(TEXT("projectVersion"), ProjectVersion);
	}

	Data->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());

	// GameMaps 设置
	FString GameDefaultMap;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameDefaultMap"), GameDefaultMap, GGameIni))
	{
		Data->SetStringField(TEXT("defaultMap"), GameDefaultMap);
	}
	FString EditorStartupMap;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("EditorStartupMap"), EditorStartupMap, GGameIni))
	{
		Data->SetStringField(TEXT("editorStartupMap"), EditorStartupMap);
	}

	// GeneralProjectSettings
	FString CompanyName;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("CompanyName"), CompanyName, GGameIni))
	{
		Data->SetStringField(TEXT("companyName"), CompanyName);
	}
	FString ProjectId;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectID"), ProjectId, GGameIni))
	{
		Data->SetStringField(TEXT("projectId"), ProjectId);
	}
	FString SupportContact;
	if (GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("SupportContact"), SupportContact, GGameIni))
	{
		Data->SetStringField(TEXT("supportContact"), SupportContact);
	}

	// 读取 .uproject 以补充 EngineAssociation、TargetPlatforms、Modules
	FString ProjectFileContent;
	if (FFileHelper::LoadFileToString(ProjectFileContent, *FPaths::GetProjectFilePath()))
	{
		TSharedPtr<FJsonObject> ProjectJson;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ProjectFileContent);
		if (FJsonSerializer::Deserialize(Reader, ProjectJson) && ProjectJson.IsValid())
		{
			FString EngineAssociation;
			if (ProjectJson->TryGetStringField(TEXT("EngineAssociation"), EngineAssociation))
			{
				Data->SetStringField(TEXT("engineAssociation"), EngineAssociation);
			}

			const TArray<TSharedPtr<FJsonValue>>* TargetPlatforms = nullptr;
			if (ProjectJson->TryGetArrayField(TEXT("TargetPlatforms"), TargetPlatforms))
			{
				TArray<TSharedPtr<FJsonValue>> PlatformArray;
				for (const TSharedPtr<FJsonValue>& Value : *TargetPlatforms)
				{
					if (Value.IsValid() && Value->Type == EJson::String)
					{
						PlatformArray.Add(MakeShared<FJsonValueString>(Value->AsString()));
					}
				}
				if (PlatformArray.Num() > 0)
				{
					Data->SetArrayField(TEXT("targetPlatforms"), PlatformArray);
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
			if (ProjectJson->TryGetArrayField(TEXT("Modules"), Modules))
			{
				TArray<TSharedPtr<FJsonValue>> ModuleArray;
				for (const TSharedPtr<FJsonValue>& Value : *Modules)
				{
					if (Value.IsValid() && Value->Type == EJson::Object)
					{
						const TSharedPtr<FJsonObject> ModuleObj = Value->AsObject();
						if (ModuleObj.IsValid())
						{
							FString ModuleName;
							if (ModuleObj->TryGetStringField(TEXT("Name"), ModuleName))
							{
								ModuleArray.Add(MakeShared<FJsonValueString>(ModuleName));
							}
						}
					}
				}
				if (ModuleArray.Num() > 0)
				{
					Data->SetArrayField(TEXT("modules"), ModuleArray);
				}
			}
		}
	}

	// 启用的插件信息（使用 PluginManager，兼容 5.0-5.7）
	TArray<TSharedPtr<FJsonValue>> PluginArray;
	IPluginManager& PluginManager = IPluginManager::Get();
	const TArray<TSharedRef<IPlugin>> EnabledPlugins = PluginManager.GetEnabledPlugins();
	for (const TSharedRef<IPlugin>& Plugin : EnabledPlugins)
	{
		TSharedPtr<FJsonObject> PluginObj = MakeShared<FJsonObject>();
		PluginObj->SetStringField(TEXT("name"), Plugin->GetName());
		PluginObj->SetStringField(TEXT("versionName"), Plugin->GetDescriptor().VersionName);
		PluginObj->SetStringField(TEXT("category"), Plugin->GetDescriptor().Category);
		PluginObj->SetStringField(TEXT("baseDir"), Plugin->GetBaseDir());
		PluginArray.Add(MakeShared<FJsonValueObject>(PluginObj));
	}
	if (PluginArray.Num() > 0)
	{
		Data->SetArrayField(TEXT("enabledPlugins"), PluginArray);
	}

	return Data;
}
void FUAL_CommandHandler::Handle_CreateBlueprint(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString BlueprintName;
	if (!Payload->TryGetStringField(TEXT("name"), BlueprintName) || BlueprintName.IsEmpty())
	{
		SendError(RequestId, 400, TEXT("Missing field: name"));
		return;
	}

	FString ParentClassStr;
	if (!Payload->TryGetStringField(TEXT("parent_class"), ParentClassStr))
	{
		Payload->TryGetStringField(TEXT("parentClass"), ParentClassStr);
	}
	if (ParentClassStr.IsEmpty())
	{
		ParentClassStr = TEXT("/Script/Engine.Actor");
	}

	FString PackagePath;
	if (!Payload->TryGetStringField(TEXT("path"), PackagePath))
	{
		FString Folder;
		if (!Payload->TryGetStringField(TEXT("folder"), Folder))
		{
			Folder = TEXT("/Game/UnrealAgent/Blueprints");
		}
		if (!Folder.StartsWith(TEXT("/")))
		{
			Folder = FString::Printf(TEXT("/Game/%s"), *Folder);
		}
		if (!Folder.EndsWith(TEXT("/")))
		{
			Folder += TEXT("/");
		}
		PackagePath = Folder + BlueprintName;
	}

	// 1. Resolve Parent Class
	FString ClassError;
	UClass* ParentClass = ResolveClassFromIdentifier(ParentClassStr, AActor::StaticClass(), ClassError);
	if (!ParentClass)
	{
		SendError(RequestId, 404, ClassError);
		return;
	}

	// 2. Check Package
	FString PackageName;
	if (!FPackageName::TryConvertFilenameToLongPackageName(PackagePath, PackageName))
	{
		PackageName = PackagePath;
	}
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		SendError(RequestId, 500, TEXT("Failed to create package"));
		return;
	}

	// 3. Create Blueprint
	EBlueprintType BlueprintType = BPTYPE_Normal;
	if (ParentClass->IsChildOf(UInterface::StaticClass()))
	{
		BlueprintType = BPTYPE_Interface;
	}
	else if (ParentClass->IsChildOf(ALevelScriptActor::StaticClass()))
	{
		BlueprintType = BPTYPE_LevelScript;
	}
	else if (ParentClass->IsChildOf(UFunction::StaticClass())) // MacroLibrary etc handled differently, keep simple
	{
		BlueprintType = BPTYPE_FunctionLibrary;
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*BlueprintName),
		BlueprintType,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);

	if (!Blueprint)
	{
		SendError(RequestId, 500, TEXT("Failed to create Blueprint asset"));
		return;
	}

	// 4. Add Components
	const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
	if (Payload->TryGetArrayField(TEXT("components"), Components) && Components)
	{
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (SCS)
		{
			for (const TSharedPtr<FJsonValue>& CompVal : *Components)
			{
				const TSharedPtr<FJsonObject> CompObj = CompVal->AsObject();
				if (!CompObj.IsValid()) continue;

				FString CompType, CompName, AttachTo;
				CompObj->TryGetStringField(TEXT("component_type"), CompType);
				CompObj->TryGetStringField(TEXT("component_name"), CompName);
				CompObj->TryGetStringField(TEXT("attach_to"), AttachTo);

				if (CompType.IsEmpty()) continue;

				FString CompError;
				UClass* CompClass = ResolveClassFromIdentifier(CompType, USceneComponent::StaticClass(), CompError);
				if (!CompClass)
				{
					UE_LOG(LogUALCommand, Warning, TEXT("Component class not found: %s"), *CompType);
					continue;
				}

				USCS_Node* NewNode = SCS->CreateNode(CompClass, FName(*CompName));
				if (NewNode)
				{
					// Attach
					if (AttachTo.IsEmpty() || AttachTo.Equals(TEXT("root"), ESearchCase::IgnoreCase) || AttachTo.Equals(TEXT("DefaultSceneRoot"), ESearchCase::IgnoreCase))
					{
						SCS->AddNode(NewNode); // Default root or next to it
					}
					else
					{
						// Find parent
						USCS_Node* ParentNode = nullptr;
						for (USCS_Node* Node : SCS->GetAllNodes())
						{
							if (Node && Node->GetVariableName().ToString().Equals(AttachTo))
							{
								ParentNode = Node;
								break;
							}
						}
						if (ParentNode)
						{
							ParentNode->AddChildNode(NewNode);
						}
						else
						{
							SCS->AddNode(NewNode); // Fallback
						}
					}

					// Set Transform
					if (USceneComponent* Template = Cast<USceneComponent>(NewNode->ComponentTemplate))
					{
						const FVector Loc = ReadVectorDirect(CompObj->GetObjectField(TEXT("location")));
						const FRotator Rot = ReadRotatorDirect(CompObj->GetObjectField(TEXT("rotation")));
						const FVector Scale = ReadVectorDirect(CompObj->GetObjectField(TEXT("scale")), FVector(1, 1, 1));
						
						Template->SetRelativeLocation(Loc);
						Template->SetRelativeRotation(Rot);
						Template->SetRelativeScale3D(Scale);

						// Set Properties
						const TSharedPtr<FJsonObject>* Props = nullptr;
						if (CompObj->TryGetObjectField(TEXT("properties"), Props) && Props && Props->IsValid())
						{
							for (auto& Pair : (*Props)->Values)
							{
								FString Key = Pair.Key;
								TSharedPtr<FJsonValue> Val = Pair.Value;
								
								FProperty* Prop = CompClass->FindPropertyByName(FName(*Key));
								if (Prop)
								{
									void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);
									// Basic types only for now
									if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
									{
										StrProp->SetPropertyValue(ValuePtr, Val->AsString());
									}
									else if (FDoubleProperty* DblProp = CastField<FDoubleProperty>(Prop))
									{
										DblProp->SetPropertyValue(ValuePtr, Val->AsNumber());
									}
									else if (FFloatProperty* FltProp = CastField<FFloatProperty>(Prop))
									{
										FltProp->SetPropertyValue(ValuePtr, (float)Val->AsNumber());
									}
									else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
									{
										BoolProp->SetPropertyValue(ValuePtr, Val->AsBool());
									}
									// Add more as needed
								}
							}
						}
					}
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	
	// Save
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs);

	// Asset Registry
	FAssetRegistryModule::AssetCreated(Blueprint);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("name"), BlueprintName);
	Result->SetStringField(TEXT("path"), PackageName);
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
	Result->SetBoolField(TEXT("saved"), true);

	TArray<TSharedPtr<FJsonValue>> CompResults;
	if (Blueprint->SimpleConstructionScript)
	{
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			NodeObj->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown"));
			NodeObj->SetStringField(TEXT("attach_to"), Node->ParentComponentOrVariableName.ToString());
			CompResults.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
	}
	Result->SetArrayField(TEXT("components"), CompResults);
	Result->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>()); // Placeholder

	SendResponse(RequestId, 200, Result);
}

// 辅助函数：单体生成逻辑
TSharedPtr<FJsonObject> FUAL_CommandHandler::SpawnSingleActor(const TSharedPtr<FJsonObject>& Item)
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

	UWorld* World = GetTargetWorld();
	if (!World)
	{
		return nullptr;
	}

	FUALResolvedSpawnRequest Resolved;
	FString ResolveError;

	if (!AssetId.IsEmpty())
	{
		if (!ResolveSpawnFromAssetId(AssetId, Resolved, ResolveError))
		{
			UE_LOG(LogUALCommand, Warning, TEXT("Spawn failed to resolve asset_id=%s error=%s"), *AssetId, *ResolveError);
			return nullptr;
		}
	}
	else if (!PresetName.IsEmpty())
	{
		FUALSpawnPreset Preset;
		if (!ResolvePreset(PresetName, Preset))
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
	ReadTransformFromItem(Item, Location, Rotation, Scale);

	FActorSpawnParameters Params;
	if (!DesiredName.IsEmpty())
	{
		Params.Name = FName(*DesiredName);
		Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	}

	// UWorld::SpawnActor API 在 5.0-5.7 统一接受指针参数版本，使用显式地址避免版本差异
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

	if (!SetStaticMeshIfNeeded(Actor, MeshPath))
	{
		Actor->Destroy();
		return nullptr;
	}

	Actor->SetActorScale3D(Scale);
#if WITH_EDITOR
	if (!DesiredName.IsEmpty())
	{
		Actor->SetActorLabel(DesiredName);
	}
#endif

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), GetActorFriendlyName(Actor));
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

void FUAL_CommandHandler::Handle_SpawnActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
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

		SendResponse(RequestId, SuccessCount > 0 ? 200 : 500, Data);
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
			// 这里无法简单拿到Actor指针，除非重新查找，或者修改辅助函数返回值。
			// 简单起见，这里再按路径查一次，或者暂时不自动选中（保持与旧逻辑一致则需要Actor*）
			// 为保持简洁，这里暂不执行 SelectActor，如需可自行扩展。
			
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
		SendResponse(RequestId, 200, Data);
	}
	else
	{
		SendError(RequestId, 500, TEXT("Spawn failed"));
	}
}

void FUAL_CommandHandler::Handle_SpawnActorsBatch(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TArray<TSharedPtr<FJsonValue>>* Batch = nullptr;
	if (!Payload->TryGetArrayField(TEXT("batch"), Batch) || !Batch)
	{
		SendError(RequestId, 400, TEXT("Missing batch array"));
		return;
	}

	TSharedPtr<FJsonObject> ForwardPayload = MakeShared<FJsonObject>();
	ForwardPayload->SetArrayField(TEXT("instances"), *Batch);
	Handle_SpawnActor(ForwardPayload, RequestId);
}

// 辅助函数：单体删除逻辑
bool FUAL_CommandHandler::DestroySingleActor(const FString& Name, const FString& Path)
{
	if (Name.IsEmpty() && Path.IsEmpty())
	{
		return false;
	}

	UWorld* World = GetTargetWorld();
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
		TargetActor = FindActorByLabel(World, Name);
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

void FUAL_CommandHandler::Handle_DestroyActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
#if WITH_EDITOR
	// 创建撤销事务，使删除操作可通过 Ctrl+Z 撤销
	FScopedTransaction Transaction(LText(TEXT("删除Actor"), TEXT("Delete Actor")));
#endif

	// 新版：targets 选择器
	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (Payload->TryGetObjectField(TEXT("targets"), TargetsObj) && TargetsObj && TargetsObj->IsValid())
	{
		UWorld* World = GetTargetWorld();
		if (!World)
		{
			SendError(RequestId, 500, TEXT("World not available"));
			return;
		}

		TSet<AActor*> TargetSet;
		FString TargetError;
		if (!ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError))
		{
			SendError(RequestId, 404, TargetError);
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
			const FString FriendlyName = GetActorFriendlyName(Actor);
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
		SendResponse(RequestId, Code, Data);
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
		SendError(RequestId, 404, TEXT("Actor not found or failed to destroy"));
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

	SendResponse(RequestId, 200, Data);
}

void FUAL_CommandHandler::Handle_DestroyActorsBatch(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TArray<TSharedPtr<FJsonValue>>* Batch = nullptr;
	if (!Payload->TryGetArrayField(TEXT("batch"), Batch) || !Batch)
	{
		SendError(RequestId, 400, TEXT("Missing batch array"));
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

void FUAL_CommandHandler::Handle_SetActorTransform(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Label;
	Payload->TryGetStringField(TEXT("name"), Label);

	FString Path;
	Payload->TryGetStringField(TEXT("path"), Path);

	if (Label.IsEmpty() && Path.IsEmpty())
	{
		SendError(RequestId, 400, TEXT("Missing field: name or path"));
		return;
	}

	UWorld* World = GetTargetWorld();
	if (!World)
	{
		SendError(RequestId, 500, TEXT("World not available"));
		return;
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

	if (!TargetActor && !Label.IsEmpty())
	{
		TargetActor = FindActorByLabel(World, Label);
	}

	if (!TargetActor)
	{
		SendError(RequestId, 404, TEXT("Actor not found"));
		return;
	}

#if WITH_EDITOR
	FScopedTransaction Transaction(LText(TEXT("修改Actor变换"), TEXT("Modify Actor Transform")));
#endif

	FVector NewLocation = TargetActor->GetActorLocation();
	FRotator NewRotation = TargetActor->GetActorRotation();
	FVector NewScale = TargetActor->GetActorScale3D();

	// 兼容旧：直接在顶层传 location / rotation / scale（绝对值）
	TSharedPtr<FJsonObject> LocationObj, RotationObj, ScaleObj;
	const bool bHasLocationLegacy = TryGetObjectFieldFlexible(Payload, TEXT("location"), LocationObj);
	const bool bHasRotationLegacy = TryGetObjectFieldFlexible(Payload, TEXT("rotation"), RotationObj);
	const bool bHasScaleLegacy = TryGetObjectFieldFlexible(Payload, TEXT("scale"), ScaleObj);

	// 新：set / addition / reduction 三段式
	TSharedPtr<FJsonObject> SetObj, AddObj, SubObj;
	TryGetObjectFieldFlexible(Payload, TEXT("set"), SetObj);
	TryGetObjectFieldFlexible(Payload, TEXT("addition"), AddObj);
	TryGetObjectFieldFlexible(Payload, TEXT("reduction"), SubObj);

	TSharedPtr<FJsonObject> SetLoc, SetRot, SetScale;
	TSharedPtr<FJsonObject> AddLoc, AddRot, AddScale;
	TSharedPtr<FJsonObject> SubLoc, SubRot, SubScale;

	const bool bSetLoc = SetObj.IsValid() && TryGetObjectFieldFlexible(SetObj, TEXT("location"), SetLoc);
	const bool bSetRot = SetObj.IsValid() && TryGetObjectFieldFlexible(SetObj, TEXT("rotation"), SetRot);
	const bool bSetScale = SetObj.IsValid() && TryGetObjectFieldFlexible(SetObj, TEXT("scale"), SetScale);

	const bool bAddLoc = AddObj.IsValid() && TryGetObjectFieldFlexible(AddObj, TEXT("location"), AddLoc);
	const bool bAddRot = AddObj.IsValid() && TryGetObjectFieldFlexible(AddObj, TEXT("rotation"), AddRot);
	const bool bAddScale = AddObj.IsValid() && TryGetObjectFieldFlexible(AddObj, TEXT("scale"), AddScale);

	const bool bSubLoc = SubObj.IsValid() && TryGetObjectFieldFlexible(SubObj, TEXT("location"), SubLoc);
	const bool bSubRot = SubObj.IsValid() && TryGetObjectFieldFlexible(SubObj, TEXT("rotation"), SubRot);
	const bool bSubScale = SubObj.IsValid() && TryGetObjectFieldFlexible(SubObj, TEXT("scale"), SubScale);

	// Space
	FString SpaceStr;
	Payload->TryGetStringField(TEXT("space"), SpaceStr);
	const bool bLocalSpace = SpaceStr.Equals(TEXT("Local"), ESearchCase::IgnoreCase);

	// 绝对值（旧 & set）
	if (bHasLocationLegacy)
	{
		NewLocation = ReadVectorDirect(LocationObj, NewLocation);
	}
	if (bHasRotationLegacy)
	{
		NewRotation = ReadRotatorDirect(RotationObj, NewRotation);
	}
	if (bHasScaleLegacy)
	{
		NewScale = ReadVectorDirect(ScaleObj, NewScale);
	}

	if (bSetLoc)
	{
		NewLocation = ReadVectorDirect(SetLoc, NewLocation);
	}
	if (bSetRot)
	{
		NewRotation = ReadRotatorDirect(SetRot, NewRotation);
	}
	if (bSetScale)
	{
		NewScale = ReadVectorDirect(SetScale, NewScale);
	}

	// 增量 addition
	if (bAddLoc)
	{
		FVector Delta = ReadVectorDirect(AddLoc, FVector::ZeroVector);
		if (bLocalSpace)
		{
			Delta = TargetActor->GetActorRotation().RotateVector(Delta);
		}
		NewLocation += Delta;
	}
	if (bAddRot)
	{
		FRotator Delta = ReadRotatorDirect(AddRot, FRotator::ZeroRotator);
		if (bLocalSpace)
		{
			// Local rotation application is tricky with FRotator addition
			// Using AddActorLocalRotation equivalent logic for accumulated result
			// But here we need to calculate final FRotator to Set
			// Simulating:
			FQuat CurrentQuat = NewRotation.Quaternion();
			FQuat DeltaQuat = Delta.Quaternion();
			// Local rotation: New = Current * Delta
			NewRotation = (CurrentQuat * DeltaQuat).Rotator();
		}
		else
		{
			NewRotation += Delta;
		}
	}
	if (bAddScale)
	{
		const FVector DeltaScale = ReadVectorDirect(AddScale, FVector::ZeroVector);
		NewScale += DeltaScale;
	}

	// 递减 reduction
	if (bSubLoc)
	{
		FVector Delta = ReadVectorDirect(SubLoc, FVector::ZeroVector);
		if (bLocalSpace)
		{
			Delta = TargetActor->GetActorRotation().RotateVector(Delta);
		}
		NewLocation -= Delta;
	}
	if (bSubRot)
	{
		FRotator Delta = ReadRotatorDirect(SubRot, FRotator::ZeroRotator);
		if (bLocalSpace)
		{
			FQuat CurrentQuat = NewRotation.Quaternion();
			FQuat DeltaQuat = Delta.Quaternion();
			// Inverse operation
			NewRotation = (CurrentQuat * DeltaQuat.Inverse()).Rotator();
		}
		else
		{
			NewRotation -= Delta;
		}
	}
	if (bSubScale)
	{
		const FVector DeltaScale = ReadVectorDirect(SubScale, FVector::ZeroVector);
		NewScale -= DeltaScale;
	}

	const bool bAny =
		bHasLocationLegacy || bHasRotationLegacy || bHasScaleLegacy ||
		bSetLoc || bSetRot || bSetScale ||
		bAddLoc || bAddRot || bAddScale ||
		bSubLoc || bSubRot || bSubScale;

	if (!bAny)
	{
		SendError(RequestId, 400, TEXT("Missing fields: location/rotation/scale/set/addition/reduction"));
		return;
	}

	TargetActor->Modify();
	TargetActor->SetActorLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
	TargetActor->SetActorScale3D(NewScale);

	// Snap to floor
	bool bSnapToFloor = false;
	if (Payload->TryGetBoolField(TEXT("snap_to_floor"), bSnapToFloor) && bSnapToFloor)
	{
#if WITH_EDITOR
		if (GEditor)
		{
			// 需要选中才能执行 SNAPTOFLOOR
			const bool bWasSelected = TargetActor->IsSelected();
			if (!bWasSelected)
			{
				GEditor->SelectActor(TargetActor, true, false);
			}
			GEditor->Exec(World, TEXT("SNAPTOFLOOR"));
			// Update location after snap
			NewLocation = TargetActor->GetActorLocation();
			if (!bWasSelected)
			{
				GEditor->SelectActor(TargetActor, false, false);
			}
		}
#endif
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), true);
	Data->SetStringField(TEXT("name"), GetActorFriendlyName(TargetActor));
	Data->SetStringField(TEXT("path"), TargetActor->GetPathName());
	Data->SetStringField(TEXT("class"), TargetActor->GetClass()->GetName());
	Data->SetObjectField(TEXT("location"), MakeVectorJson(NewLocation));
	Data->SetObjectField(TEXT("rotation"), MakeRotatorJson(NewRotation));
	Data->SetObjectField(TEXT("scale"), MakeVectorJson(NewScale));

	SendResponse(RequestId, 200, Data);
}

void FUAL_CommandHandler::Handle_GetActorInfo(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		SendError(RequestId, 400, TEXT("Missing object: targets"));
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

	UWorld* World = GetTargetWorld();
	if (!World)
	{
		SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError))
	{
		SendError(RequestId, 404, TargetError);
		return;
	}

	const int32 TotalFound = TargetSet.Num();

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = GetActorFriendlyName(A);
		const FString NameB = GetActorFriendlyName(B);
		return NameA < NameB;
	});

	TArray<TSharedPtr<FJsonValue>> ActorsJson;
	for (int32 Index = 0; Index < TargetArray.Num() && Index < Limit; ++Index)
	{
		if (TSharedPtr<FJsonObject> Info = BuildActorInfoWithOptions(TargetArray[Index], bReturnTransform, bReturnBounds))
		{
			ActorsJson.Add(MakeShared<FJsonValueObject>(Info));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), ActorsJson.Num());
	Data->SetNumberField(TEXT("total_found"), TotalFound);
	Data->SetArrayField(TEXT("actors"), ActorsJson);

	SendResponse(RequestId, 200, Data);
}

void FUAL_CommandHandler::Handle_GetActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
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
			SendError(RequestId, 400, TEXT("Missing field: name or path"));
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
		SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	UWorld* World = GetTargetWorld();
	if (!World)
	{
		SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!ResolveTargetsToActors(Targets, World, TargetSet, TargetError))
	{
		SendError(RequestId, 404, TargetError);
		return;
	}

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = GetActorFriendlyName(A);
		const FString NameB = GetActorFriendlyName(B);
		return NameA < NameB;
	});

	AActor* TargetActor = TargetArray.Num() > 0 ? TargetArray[0] : nullptr;
	if (!TargetActor)
	{
		SendError(RequestId, 404, TEXT("Actor not found"));
		return;
	}

	if (TSharedPtr<FJsonObject> Info = BuildActorInfo(TargetActor))
	{
		SendResponse(RequestId, 200, Info);
	}
	else
	{
		SendError(RequestId, 500, TEXT("Failed to build actor info"));
	}
}

void FUAL_CommandHandler::Handle_InspectActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TSharedPtr<FJsonObject>* TargetsObjPtr = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObjPtr) || !TargetsObjPtr || !TargetsObjPtr->IsValid())
	{
		SendError(RequestId, 400, TEXT("Missing object: targets"));
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
		WantedProps = GetDefaultInspectProps();
	}

	UWorld* World = GetTargetWorld();
	if (!World)
	{
		SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!ResolveTargetsToActors(Targets, World, TargetSet, TargetError))
	{
		SendError(RequestId, 404, TargetError);
		return;
	}

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = GetActorFriendlyName(A);
		const FString NameB = GetActorFriendlyName(B);
		return NameA < NameB;
	});

	TArray<TSharedPtr<FJsonValue>> Results;
	for (AActor* Actor : TargetArray)
	{
		if (!Actor)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Obj = BuildActorInfo(Actor);
		if (!Obj.IsValid())
		{
			continue;
		}

		if (TSharedPtr<FJsonObject> Props = BuildSelectedProps(Actor, WantedProps))
		{
			Obj->SetObjectField(TEXT("props"), Props);
		}

		Results.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Results.Num());
	Data->SetArrayField(TEXT("actors"), Results);

	SendResponse(RequestId, 200, Data);
}

void FUAL_CommandHandler::Handle_SetProperty(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !PropsObj->IsValid())
	{
		SendError(RequestId, 400, TEXT("Missing object: properties"));
		return;
	}

	UWorld* World = GetTargetWorld();
	if (!World)
	{
		SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!ResolveTargetsToActors(*TargetsObj, World, TargetSet, TargetError))
	{
		SendError(RequestId, 404, TargetError);
		return;
	}

	TArray<AActor*> TargetArray = TargetSet.Array();
	Algo::Sort(TargetArray, [](AActor* A, AActor* B)
	{
		const FString NameA = GetActorFriendlyName(A);
		const FString NameB = GetActorFriendlyName(B);
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

		TSharedPtr<FJsonObject> ActorObj = BuildActorInfo(Actor);
		if (!ActorObj.IsValid())
		{
			continue;
		}

		TSharedPtr<FJsonObject> Updated = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Errors;

		TArray<FString> CandidateNames;
		CollectPropertyNames(Actor, CandidateNames);
		if (USceneComponent* RootComp = Actor->GetRootComponent())
		{
			CollectPropertyNames(RootComp, CandidateNames);
		}
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			CollectPropertyNames(Comp, CandidateNames);
		}

		for (const auto& Pair : (*PropsObj)->Values)
		{
			const FString PropName = Pair.Key;
			const TSharedPtr<FJsonValue>& DesiredValue = Pair.Value;

			UObject* TargetObj = nullptr;
			FProperty* Prop = FindWritablePropertyOnActorHierarchy(Actor, PropName, TargetObj);

			if (!Prop)
			{
				TArray<FString> Suggestions;
				SuggestProperties(PropName, CandidateNames, Suggestions);

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
			if (SetSimpleProperty(Prop, TargetObj, DesiredValue, TypeError))
			{
				if (TSharedPtr<FJsonValue> JsonValue = PropertyToJsonValueCompat(Prop, Prop->ContainerPtrToValuePtr<void>(TargetObj)))
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
			if (TSharedPtr<FJsonValue> Current = PropertyToJsonValueCompat(Prop, Prop->ContainerPtrToValuePtr<void>(TargetObj)))
			{
				Err->SetStringField(TEXT("expected_type"), Prop->GetClass()->GetName());
				Err->SetStringField(TEXT("current_value"), JsonValueToString(Current));
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

	SendResponse(RequestId, 200, Data);
}

void FUAL_CommandHandler::Handle_FindActors(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	FString Keyword;
	if (!Payload->TryGetStringField(TEXT("name"), Keyword))
	{
		Keyword.Reset();
	}

	bool bExact = false;
	Payload->TryGetBoolField(TEXT("exact"), bExact);

	if (Keyword.IsEmpty())
	{
		SendError(RequestId, 400, TEXT("Missing field: name"));
		return;
	}

	UWorld* World = GetTargetWorld();
	if (!World)
	{
		SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	int32 Limit = 100;
	Payload->TryGetNumberField(TEXT("limit"), Limit);
	if (Limit <= 0)
	{
		Limit = 100;
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		const FString Name = GetActorFriendlyName(Actor);

		const bool bMatch = bExact
			? Name.Equals(Keyword, ESearchCase::IgnoreCase)
			: Name.Contains(Keyword, ESearchCase::IgnoreCase, ESearchDir::FromStart);

		if (!bMatch)
		{
			continue;
		}

		if (TSharedPtr<FJsonObject> Info = BuildActorInfo(Actor))
		{
			Results.Add(MakeShared<FJsonValueObject>(Info));
		}

		if (Results.Num() >= Limit)
		{
			break;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Results.Num());
	Data->SetArrayField(TEXT("actors"), Results);

	SendResponse(RequestId, 200, Data);
}

void FUAL_CommandHandler::Handle_TransformActors(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = GetTargetWorld();
	if (!World)
	{
		SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	const TSharedPtr<FJsonObject>* FilterObj = nullptr;
	Payload->TryGetObjectField(TEXT("filter"), FilterObj);

	const TSharedPtr<FJsonObject>* TransformObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("transform"), TransformObj) || !TransformObj || !TransformObj->IsValid())
	{
		SendError(RequestId, 400, TEXT("Missing object: transform"));
		return;
	}

	FString NameContains, NameNotContains, ClassContains, ClassNotContains, ClassExact;
	TArray<FString> ExcludeClasses;
	if (FilterObj && *FilterObj)
	{
		(*FilterObj)->TryGetStringField(TEXT("name_contains"), NameContains);
		(*FilterObj)->TryGetStringField(TEXT("name_not_contains"), NameNotContains);
		(*FilterObj)->TryGetStringField(TEXT("class_contains"), ClassContains);
		(*FilterObj)->TryGetStringField(TEXT("class_not_contains"), ClassNotContains);
		(*FilterObj)->TryGetStringField(TEXT("class_exact"), ClassExact);

		const TArray<TSharedPtr<FJsonValue>>* ExcludeArray = nullptr;
		if ((*FilterObj)->TryGetArrayField(TEXT("exclude_classes"), ExcludeArray) && ExcludeArray)
		{
			for (const TSharedPtr<FJsonValue>& Val : *ExcludeArray)
			{
				FString Item;
				if (Val.IsValid() && Val->TryGetString(Item))
				{
					ExcludeClasses.Add(Item);
				}
			}
		}
	}

	const TSharedPtr<FJsonObject>& TObj = *TransformObj;
	const bool bHasAbsLocation = TObj->HasTypedField<EJson::Object>(TEXT("location"));
	const bool bHasAbsRotation = TObj->HasTypedField<EJson::Object>(TEXT("rotation"));
	const bool bHasAbsScale = TObj->HasTypedField<EJson::Object>(TEXT("scale"));
	const bool bHasLocDelta = TObj->HasTypedField<EJson::Object>(TEXT("location_delta"));
	const bool bHasRotDelta = TObj->HasTypedField<EJson::Object>(TEXT("rotation_delta"));
	const bool bHasScaleMultiply = TObj->HasTypedField<EJson::Object>(TEXT("scale_multiply"));

	if (!bHasAbsLocation && !bHasAbsRotation && !bHasAbsScale && !bHasLocDelta && !bHasRotDelta && !bHasScaleMultiply)
	{
		SendError(RequestId, 400, TEXT("Missing transform fields"));
		return;
	}

	const FVector LocDelta = bHasLocDelta ? ReadVector(TObj, TEXT("location_delta"), FVector::ZeroVector) : FVector::ZeroVector;
	const FRotator RotDelta = bHasRotDelta ? ReadRotator(TObj, TEXT("rotation_delta"), FRotator::ZeroRotator) : FRotator::ZeroRotator;
	const FVector ScaleMultiply = bHasScaleMultiply ? ReadVector(TObj, TEXT("scale_multiply"), FVector(1.0f, 1.0f, 1.0f)) : FVector(1.0f, 1.0f, 1.0f);

#if WITH_EDITOR
	FScopedTransaction Transaction(LText(TEXT("批量修改Actor变换"), TEXT("Batch Modify Actor Transform")));
#endif

	int32 AffectedCount = 0;
	TArray<TSharedPtr<FJsonValue>> Affected;
	const int32 MaxReport = 100;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		if (!ShouldIncludeActorAdvanced(Actor, NameContains, NameNotContains, ClassContains, ClassNotContains, ClassExact, ExcludeClasses))
		{
			continue;
		}

		FVector NewLocation = Actor->GetActorLocation();
		FRotator NewRotation = Actor->GetActorRotation();
		FVector NewScale = Actor->GetActorScale3D();

		if (bHasAbsLocation)
		{
			NewLocation = ReadVector(TObj, TEXT("location"), NewLocation);
		}
		if (bHasAbsRotation)
		{
			NewRotation = ReadRotator(TObj, TEXT("rotation"), NewRotation);
		}
		if (bHasAbsScale)
		{
			NewScale = ReadVector(TObj, TEXT("scale"), NewScale);
		}

		NewLocation += LocDelta;
		NewRotation += RotDelta;
		NewScale.X *= ScaleMultiply.X;
		NewScale.Y *= ScaleMultiply.Y;
		NewScale.Z *= ScaleMultiply.Z;

		Actor->Modify();
		Actor->SetActorLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::TeleportPhysics);
		Actor->SetActorScale3D(NewScale);

		AffectedCount++;
		if (Affected.Num() < MaxReport)
		{
			TSharedPtr<FJsonObject> Obj = BuildActorInfo(Actor);
			if (Obj.IsValid())
			{
				Obj->SetObjectField(TEXT("location"), MakeVectorJson(NewLocation));
				Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(NewRotation));
				Obj->SetObjectField(TEXT("scale"), MakeVectorJson(NewScale));
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

	SendResponse(RequestId, 200, Data);
}

void FUAL_CommandHandler::Handle_SetTransformUnified(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	UWorld* World = GetTargetWorld();
	if (!World)
	{
		SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	const TSharedPtr<FJsonObject>* TargetsObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("targets"), TargetsObj) || !TargetsObj || !TargetsObj->IsValid())
	{
		SendError(RequestId, 400, TEXT("Missing object: targets"));
		return;
	}

	const TSharedPtr<FJsonObject>* OpObj = nullptr;
	if (!Payload->TryGetObjectField(TEXT("operation"), OpObj) || !OpObj || !OpObj->IsValid())
	{
		SendError(RequestId, 400, TEXT("Missing object: operation"));
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
	TryGetObjectFieldFlexible(Operation, TEXT("set"), SetObj);
	TryGetObjectFieldFlexible(Operation, TEXT("add"), AddObj);
	TryGetObjectFieldFlexible(Operation, TEXT("multiply"), MulObj);

	if (!SetObj.IsValid() && !AddObj.IsValid() && !MulObj.IsValid())
	{
		SendError(RequestId, 400, TEXT("Missing operation fields: set/add/multiply"));
		return;
	}

	TSet<AActor*> TargetSet;
	FString TargetError;
	if (!ResolveTargetsToActors(Targets, World, TargetSet, TargetError))
	{
		SendError(RequestId, 404, TargetError);
		return;
	}

#if WITH_EDITOR
	FScopedTransaction Transaction(LText(TEXT("批量修改Actor变换"), TEXT("Batch Modify Actor Transform")));
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
			if (TryGetObjectFieldFlexible(SetObj, TEXT("location"), LocObj))
			{
				NewLocation = ReadVectorDirect(LocObj, NewLocation);
			}
			if (TryGetObjectFieldFlexible(SetObj, TEXT("rotation"), RotObj))
			{
				NewRotation = ReadRotatorDirect(RotObj, NewRotation);
			}
			if (TryGetObjectFieldFlexible(SetObj, TEXT("scale"), ScaleObj))
			{
				NewScale = ReadVectorDirect(ScaleObj, NewScale);
			}
		}

		if (AddObj.IsValid())
		{
			TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
			if (TryGetObjectFieldFlexible(AddObj, TEXT("location"), LocObj))
			{
				FVector Delta = ReadVectorDirect(LocObj, FVector::ZeroVector);
				if (bLocalSpace)
				{
					Delta = Actor->GetActorRotation().RotateVector(Delta);
				}
				NewLocation += Delta;
			}
			if (TryGetObjectFieldFlexible(AddObj, TEXT("rotation"), RotObj))
			{
				FRotator Delta = ReadRotatorDirect(RotObj, FRotator::ZeroRotator);
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
			if (TryGetObjectFieldFlexible(AddObj, TEXT("scale"), ScaleObj))
			{
				NewScale += ReadVectorDirect(ScaleObj, FVector::ZeroVector);
			}
		}

		if (MulObj.IsValid())
		{
			TSharedPtr<FJsonObject> LocObj, RotObj, ScaleObj;
			if (TryGetObjectFieldFlexible(MulObj, TEXT("location"), LocObj))
			{
				const FVector Mul = ReadVectorDirect(LocObj, FVector(1, 1, 1));
				NewLocation.X *= Mul.X;
				NewLocation.Y *= Mul.Y;
				NewLocation.Z *= Mul.Z;
			}
			if (TryGetObjectFieldFlexible(MulObj, TEXT("rotation"), RotObj))
			{
				const FRotator MulRot = ReadRotatorDirect(RotObj, FRotator(1, 1, 1));
				NewRotation.Roll *= MulRot.Roll;
				NewRotation.Pitch *= MulRot.Pitch;
				NewRotation.Yaw *= MulRot.Yaw;
			}
			if (TryGetObjectFieldFlexible(MulObj, TEXT("scale"), ScaleObj))
			{
				const FVector Mul = ReadVectorDirect(ScaleObj, FVector(1, 1, 1));
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
			TSharedPtr<FJsonObject> Obj = BuildActorInfo(Actor);
			if (Obj.IsValid())
			{
				Obj->SetObjectField(TEXT("location"), MakeVectorJson(NewLocation));
				Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(NewRotation));
				Obj->SetObjectField(TEXT("scale"), MakeVectorJson(NewScale));
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

	SendResponse(RequestId, 200, Data);
}

// 响应处理（用于弹出通知）
void FUAL_CommandHandler::Handle_Response(const FString& Method, const TSharedPtr<FJsonObject>& Payload)
{
	if (!Payload.IsValid())
	{
		return;
	}

	bool bOk = true;
	Payload->TryGetBoolField(TEXT("ok"), bOk);

	int32 Count = 0;
	if (!Payload->TryGetNumberField(TEXT("count"), Count))
	{
		Count = 0;
	}

	FString ImportedPath;
	Payload->TryGetStringField(TEXT("importedPath"), ImportedPath);

	FString Error;
	Payload->TryGetStringField(TEXT("error"), Error);

	const bool bIsImportFolder = Method == TEXT("content.import_folder");
	const bool bIsImportAssets = Method == TEXT("content.import_assets");
	if (!bIsImportFolder && !bIsImportAssets)
	{
		return; // 非导入相关响应不提示
	}

	const FString Title = bIsImportFolder
		? LStr(TEXT("导入文件夹到虚幻助手资产库"), TEXT("Import Folder to Unreal Agent Asset Library"))
		: LStr(TEXT("导入资产到虚幻助手资产库"), TEXT("Import Assets to Unreal Agent Asset Library"));

	FString Body;
	if (bOk)
	{
		if (!ImportedPath.IsEmpty())
		{
			Body = FString::Printf(TEXT("%s: %s"),
				*LStr(TEXT("成功"), TEXT("Succeeded")),
				*ImportedPath);
		}
		else
		{
			Body = FString::Printf(TEXT("%s: %d"),
				*LStr(TEXT("成功"), TEXT("Succeeded")),
				Count);
		}
	}
	else
	{
		if (Error.IsEmpty())
		{
			Error = LStr(TEXT("导入失败"), TEXT("Import failed"));
		}
		Body = FString::Printf(TEXT("%s (%s)"),
			*LStr(TEXT("失败"), TEXT("Failed")),
			*Error);
	}

	FNotificationInfo Info(FText::FromString(Title));
	Info.SubText = FText::FromString(Body);
	Info.ExpireDuration = 4.0f;
	Info.FadeOutDuration = 0.5f;
	Info.bUseThrobber = false;
	Info.bFireAndForget = true;

#if WITH_EDITOR
	TSharedPtr<SNotificationItem> Notification;
	if (FSlateApplication::IsInitialized())
	{
		Notification = FSlateNotificationManager::Get().AddNotification(Info);
		if (Notification.IsValid())
		{
			Notification->SetCompletionState(bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	if (!Notification.IsValid())
	{
		const FText DialogText = FText::Format(
			FText::FromString(TEXT("{0}\n{1}")),
			FText::FromString(Title),
			FText::FromString(Body));
		FMessageDialog::Open(EAppMsgType::Ok, DialogText);
	}
#else
	FSlateNotificationManager::Get().AddNotification(Info);
#endif
}

void FUAL_CommandHandler::SendResponse(const FString& RequestId, int32 Code, const TSharedPtr<FJsonObject>& Data)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ver"), TEXT("1.0"));
	Root->SetStringField(TEXT("type"), TEXT("res"));
	Root->SetStringField(TEXT("id"), RequestId);
	Root->SetNumberField(TEXT("code"), Code);

	if (Data.IsValid())
	{
		Root->SetObjectField(TEXT("result"), Data);
	}

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	FUAL_NetworkManager::Get().SendMessage(OutJson);
}

void FUAL_CommandHandler::SendError(const FString& RequestId, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetNumberField(TEXT("code"), Code);
	ErrorObj->SetStringField(TEXT("message"), Message);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ver"), TEXT("1.0"));
	Root->SetStringField(TEXT("type"), TEXT("res"));
	Root->SetStringField(TEXT("id"), RequestId);
	Root->SetNumberField(TEXT("code"), Code);
	Root->SetObjectField(TEXT("error"), ErrorObj);

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	FUAL_NetworkManager::Get().SendMessage(OutJson);
}
