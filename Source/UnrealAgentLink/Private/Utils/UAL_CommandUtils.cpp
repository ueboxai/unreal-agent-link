#include "UAL_CommandUtils.h"
#include "UAL_NetworkManager.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/RectLight.h"
#include "Camera/CameraActor.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Components/SceneComponent.h"
#include "Serialization/JsonSerializer.h"
#include "JsonObjectConverter.h"
#include "Algo/Sort.h"
#include "Misc/EngineVersion.h"
#include "AssetRegistry/AssetRegistryModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALUtils, Log, All);

bool UAL_CommandUtils::IsZh()
{
	FString Name;
	if (const TSharedPtr<const FCulture> Culture = FInternationalization::Get().GetCurrentCulture())
	{
		Name = Culture->GetName();
	}
	return Name.StartsWith(TEXT("zh"));
}

FString UAL_CommandUtils::LStr(const TCHAR* Zh, const TCHAR* En)
{
	return IsZh() ? Zh : En;
}

FText UAL_CommandUtils::LText(const TCHAR* Zh, const TCHAR* En)
{
	return FText::FromString(LStr(Zh, En));
}

UWorld* UAL_CommandUtils::GetTargetWorld()
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

FVector UAL_CommandUtils::ReadVector(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector& DefaultValue)
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

FRotator UAL_CommandUtils::ReadRotator(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FRotator& DefaultValue)
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

FVector UAL_CommandUtils::ReadVectorDirect(const TSharedPtr<FJsonObject>& Obj, const FVector& DefaultValue)
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

FRotator UAL_CommandUtils::ReadRotatorDirect(const TSharedPtr<FJsonObject>& Obj, const FRotator& DefaultValue)
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

bool UAL_CommandUtils::TryGetObjectFieldFlexible(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field, TSharedPtr<FJsonObject>& OutObj)
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

bool UAL_CommandUtils::ResolvePreset(const FString& Name, FUALSpawnPreset& OutPreset)
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

bool UAL_CommandUtils::SetStaticMeshIfNeeded(AActor* Actor, const TCHAR* MeshPath)
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

bool UAL_CommandUtils::ResolveSpawnFromAssetId(const FString& AssetId, FUALResolvedSpawnRequest& OutResolved, FString& OutError)
{
	OutResolved = FUALResolvedSpawnRequest();
	OutError.Reset();

	if (AssetId.IsEmpty())
	{
		OutError = TEXT("asset_id is empty");
		return false;
	}

	// Level 1：Preset
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

	// Level 2/3：Asset Path
	if (AssetId.StartsWith(TEXT("/")))
	{
		FSoftObjectPath SoftObjPath(AssetId);
		FSoftClassPath SoftClassPath(AssetId);

		UObject* LoadedObj = SoftObjPath.IsValid() ? SoftObjPath.TryLoad() : nullptr;

		if (UClass* LoadedClass = SoftClassPath.IsValid() ? SoftClassPath.TryLoadClass<AActor>() : nullptr)
		{
			OutResolved.SpawnClass = LoadedClass;
			OutResolved.ResolvedType = LoadedClass->GetName();
			OutResolved.SourceId = AssetId;
			return true;
		}

		if (LoadedObj)
		{
			// Blueprint
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

			// Class
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

			// Static Mesh
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

	// Fallback
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

void UAL_CommandUtils::ReadTransformFromItem(const TSharedPtr<FJsonObject>& Item, FVector& OutLocation, FRotator& OutRotation, FVector& OutScale)
{
	// Legacy
	OutLocation = ReadVector(Item, TEXT("location"), OutLocation);
	OutRotation = ReadRotator(Item, TEXT("rotation"), OutRotation);
	OutScale = ReadVector(Item, TEXT("scale"), OutScale);

	// New: transform { location/rotation/scale }
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

UClass* UAL_CommandUtils::ResolveClassFromIdentifier(const FString& Identifier, UClass* ExpectedBase, FString& OutError)
{
	if (Identifier.IsEmpty())
	{
		OutError = TEXT("Class identifier is empty");
		return nullptr;
	}

	UClass* ResolvedClass = nullptr;

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

bool UAL_CommandUtils::ResolveTargetsToActors(const TSharedPtr<FJsonObject>& Targets, UWorld* World, TSet<AActor*>& OutSet, FString& OutError)
{
	OutSet.Reset();
	OutError.Reset();

	if (!Targets.IsValid())
	{
		OutError = TEXT("Missing object: targets");
		return false;
	}

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
	// property_match: 属性匹配规则数组 [{ name: "StaticMesh", value: "Cube" }, ...]
	TArray<TPair<FString, FString>> PropertyMatchRules;
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
		// 解析 property_match 数组
		const TArray<TSharedPtr<FJsonValue>>* PropMatchArr = nullptr;
		if ((*FilterObj)->TryGetArrayField(TEXT("property_match"), PropMatchArr) && PropMatchArr)
		{
			for (const TSharedPtr<FJsonValue>& Item : *PropMatchArr)
			{
				if (!Item.IsValid() || Item->Type != EJson::Object) continue;
				const TSharedPtr<FJsonObject> Rule = Item->AsObject();
				FString Name, Value;
				if (Rule->TryGetStringField(TEXT("name"), Name) && Rule->TryGetStringField(TEXT("value"), Value))
				{
					PropertyMatchRules.Add(TPair<FString, FString>(Name, Value));
				}
			}
		}
		// 只要 filter 对象存在（即使是空对象 {}），就视为有效的过滤器
		// 空的 filter 将匹配所有 Actor（MatchFilter lambda 会返回 true）
		bHasFilter = true;
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
		// property_match 检查：所有规则必须全部匹配（AND 逻辑）
		for (const TPair<FString, FString>& Rule : PropertyMatchRules)
		{
			if (!CheckPropertyMatch(Actor, Rule.Key, Rule.Value))
			{
				return false;
			}
		}
		return true;
	};

	if (OutSet.Num() > 0)
	{
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
		OutError = TEXT("No actor found matching the specified names/paths");
		return false;
	}
	else if (bHasFilter)
	{
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

bool UAL_CommandUtils::CheckPropertyMatch(AActor* Actor, const FString& PropName, const FString& ExpectedValue)
{
	if (!Actor || PropName.IsEmpty() || ExpectedValue.IsEmpty())
	{
		return false;
	}

	// 要搜索的对象列表：Actor 本身 -> RootComponent -> 其他常用组件
	TArray<UObject*> ObjectsToSearch;
	ObjectsToSearch.Add(Actor);
	if (Actor->GetRootComponent())
	{
		ObjectsToSearch.Add(Actor->GetRootComponent());
	}

	for (UObject* TargetObj : ObjectsToSearch)
	{
		if (!TargetObj) continue;

		FProperty* Prop = FindFProperty<FProperty>(TargetObj->GetClass(), *PropName);
		if (!Prop) continue;

		FString ActualValueStr;

		// 特殊处理：对象引用属性 (如 StaticMesh, Material)
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			UObject* RefObj = ObjProp->GetObjectPropertyValue_InContainer(TargetObj);
			if (RefObj)
			{
				// 获取资产名称 (如 "Cube", "SM_Rock_01")
				ActualValueStr = RefObj->GetName();
			}
		}
		// 软对象引用
		else if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Prop))
		{
			const FSoftObjectPtr* SoftPtr = SoftObjProp->GetPropertyValuePtr_InContainer(TargetObj);
			if (SoftPtr && !SoftPtr->IsNull())
			{
				// 从路径中提取资产名
				FString AssetPath = SoftPtr->ToString();
				int32 DotIndex;
				if (AssetPath.FindLastChar('.', DotIndex))
				{
					ActualValueStr = AssetPath.RightChop(DotIndex + 1);
				}
				else
				{
					ActualValueStr = FPaths::GetBaseFilename(AssetPath);
				}
			}
		}
		// 其他属性：转为字符串比较
		else
		{
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetObj);
			if (ValuePtr)
			{
				// ExportText_InContainer 需要 6 个参数: Index, ValueStr, Container, DefaultContainer, Parent, PortFlags
				Prop->ExportText_InContainer(0, ActualValueStr, TargetObj, nullptr, TargetObj, PPF_None);
			}
		}

		// 执行模糊匹配（包含匹配，忽略大小写）
		if (!ActualValueStr.IsEmpty() && ActualValueStr.Contains(ExpectedValue, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	return false;
}

bool UAL_CommandUtils::ApplyStructValue(FStructProperty* StructProp, UObject* Target, const TSharedPtr<FJsonValue>& JsonValue)
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

const TArray<FString>& UAL_CommandUtils::GetDefaultInspectProps()
{
	static const TArray<FString> Defaults = {
		TEXT("Mobility"),
		TEXT("bHidden"),
		TEXT("CollisionProfileName"),
		TEXT("Tags")
	};
	return Defaults;
}

bool UAL_CommandUtils::TryCollectProperty(UObject* Obj, const FString& PropName, TSharedPtr<FJsonObject>& OutProps)
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

void UAL_CommandUtils::CollectPropertyNames(UObject* Obj, TArray<FString>& OutNames)
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

int32 UAL_CommandUtils::LevenshteinDistance(const FString& A, const FString& B)
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

void UAL_CommandUtils::SuggestProperties(const FString& Input, const TArray<FString>& Candidates, TArray<FString>& OutSuggestions, int32 MaxSuggestions)
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

AActor* UAL_CommandUtils::FindActorByLabel(UWorld* World, const FString& Label)
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

FString UAL_CommandUtils::GetActorFriendlyName(AActor* Actor)
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

TSharedPtr<FJsonObject> UAL_CommandUtils::MakeVectorJson(const FVector& Vec)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("x"), Vec.X);
	Obj->SetNumberField(TEXT("y"), Vec.Y);
	Obj->SetNumberField(TEXT("z"), Vec.Z);
	return Obj;
}

TSharedPtr<FJsonObject> UAL_CommandUtils::MakeRotatorJson(const FRotator& Rot)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("pitch"), Rot.Pitch);
	Obj->SetNumberField(TEXT("yaw"), Rot.Yaw);
	Obj->SetNumberField(TEXT("roll"), Rot.Roll);
	return Obj;
}

TSharedPtr<FJsonValue> UAL_CommandUtils::PropertyToJsonValueCompat(FProperty* Prop, const void* ValuePtr)
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

TSharedPtr<FJsonObject> UAL_CommandUtils::BuildActorInfo(AActor* Actor)
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

TSharedPtr<FJsonObject> UAL_CommandUtils::BuildActorInfoWithOptions(AActor* Actor, bool bIncludeTransform, bool bIncludeBounds)
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

bool UAL_CommandUtils::ShouldIncludeActor(const AActor* Actor, const FString& NameKeyword, bool bNameExact, const FString& ClassKeyword, bool bClassExact)
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

bool UAL_CommandUtils::ShouldIncludeActorAdvanced(
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

FProperty* UAL_CommandUtils::FindWritableProperty(UObject* Obj, const FString& PropName)
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

FProperty* UAL_CommandUtils::FindWritablePropertyOnActorHierarchy(AActor* Actor, const FString& PropName, UObject*& OutTargetObj)
{
	OutTargetObj = nullptr;
	if (!Actor)
	{
		return nullptr;
	}

	// 1) Actor Self
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

	// 3) Other Components
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

bool UAL_CommandUtils::SetNumericProperty(FNumericProperty* NumProp, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError)
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

bool UAL_CommandUtils::SetStructProperty(FStructProperty* StructProp, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError)
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
		
		double R = 0, G = 0, B = 0, A = 255.0;
		ObjVal->TryGetNumberField(TEXT("r"), R);
		ObjVal->TryGetNumberField(TEXT("g"), G);
		ObjVal->TryGetNumberField(TEXT("b"), B);
		const bool bHasAlpha = ObjVal->TryGetNumberField(TEXT("a"), A);
		
		// Auto-Detect Normalized Color
		const bool bIsNormalized = (R <= 1.0 && G <= 1.0 && B <= 1.0 && (!bHasAlpha || A <= 1.0));
		const bool bIsNotBlack = (R > 0.0 || G > 0.0 || B > 0.0);
		
		if (bIsNormalized && bIsNotBlack)
		{
			UE_LOG(LogUALUtils, Log, TEXT("[SmartFix] Detected 0-1 range for FColor, scaling by 255."));
			R *= 255.0;
			G *= 255.0;
			B *= 255.0;
			A = bHasAlpha ? A * 255.0 : 255.0;
		}
		else if (!bHasAlpha)
		{
			A = 255.0;
		}
		
		void* Ptr = StructProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		*static_cast<FColor*>(Ptr) = FColor(
			static_cast<uint8>(FMath::Clamp(R, 0.0, 255.0)),
			static_cast<uint8>(FMath::Clamp(G, 0.0, 255.0)),
			static_cast<uint8>(FMath::Clamp(B, 0.0, 255.0)),
			static_cast<uint8>(FMath::Clamp(A, 0.0, 255.0))
		);
		return true;
	}

	OutError = FString::Printf(TEXT("unsupported struct type: %s"), *StructProp->Struct->GetName());
	return false;
}

bool UAL_CommandUtils::SetSimpleProperty(FProperty* Prop, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError)
{
	if (!Prop || !Obj || !Value.IsValid())
	{
		return false;
	}

	// FEnumProperty
	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		UEnum* Enum = EnumProp->GetEnum();
		if (Enum)
		{
			int64 EnumValue = 0;
			if (Value->Type == EJson::String)
			{
				FString EnumName = Value->AsString();
				EnumValue = Enum->GetValueByNameString(EnumName);
				if (EnumValue == INDEX_NONE)
				{
					FString QualifiedName = FString::Printf(TEXT("%s::%s"), *Enum->GetName(), *EnumName);
					EnumValue = Enum->GetValueByNameString(QualifiedName);
				}
				if (EnumValue == INDEX_NONE)
				{
					// Fuzzy Match
					for (int32 i = 0; i < Enum->NumEnums() - 1; ++i)
					{
						FString FullName = Enum->GetNameStringByIndex(i);
						if (FullName.Contains(EnumName, ESearchCase::IgnoreCase))
						{
							EnumValue = Enum->GetValueByIndex(i);
							UE_LOG(LogUALUtils, Log, TEXT("[SmartFix] Fuzzy matched enum '%s' to '%s'"), *EnumName, *FullName);
							break;
						}
					}
				}
				if (EnumValue == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("Invalid enum value '%s' for %s"), *EnumName, *Enum->GetName());
					return false;
				}
			}
			else if (Value->Type == EJson::Number)
			{
				EnumValue = static_cast<int64>(Value->AsNumber());
			}
			else
			{
				OutError = TEXT("expects a string (enum name) or number (enum index)");
				return false;
			}
			FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			void* Ptr = EnumProp->ContainerPtrToValuePtr<void>(Obj);
			if (!Ptr || !UnderlyingProp)
			{
				return false;
			}
			UnderlyingProp->SetIntPropertyValue(Ptr, EnumValue);
			return true;
		}
	}

	if (FNumericProperty* NumProp = CastField<FNumericProperty>(Prop))
	{
		return SetNumericProperty(NumProp, Obj, Value, OutError);
	}

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		bool bVal = false;
		if (Value->Type == EJson::Boolean)
		{
			bVal = Value->AsBool();
		}
		else if (Value->Type == EJson::String)
		{
			const FString StrVal = Value->AsString();
			bVal = StrVal.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
				   StrVal.Equals(TEXT("1"), ESearchCase::IgnoreCase) ||
				   StrVal.Equals(TEXT("yes"), ESearchCase::IgnoreCase);
			UE_LOG(LogUALUtils, Log, TEXT("[SmartFix] Converted string '%s' to bool: %s"), *StrVal, bVal ? TEXT("true") : TEXT("false"));
		}
		else if (Value->Type == EJson::Number)
		{
			bVal = Value->AsNumber() > 0;
			UE_LOG(LogUALUtils, Log, TEXT("[SmartFix] Converted number to bool: %s"), bVal ? TEXT("true") : TEXT("false"));
		}
		else
		{
			OutError = TEXT("expects a boolean (or string/number that can be converted)");
			return false;
		}
		
		void* Ptr = BoolProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		BoolProp->SetPropertyValue(Ptr, bVal);
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

	// === FObjectProperty 支持（硬引用，如 StaticMesh, Material 等） ===
	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		// 从 JSON 获取资产路径字符串
		FString AssetPath;
		if (Value->Type == EJson::String)
		{
			AssetPath = Value->AsString();
		}
		else if (Value->Type == EJson::Object)
		{
			// 支持对象格式: { "path": "/Game/..." } 或 { "asset_path": "..." }
			const TSharedPtr<FJsonObject> ObjVal = Value->AsObject();
			if (!ObjVal->TryGetStringField(TEXT("path"), AssetPath))
			{
				ObjVal->TryGetStringField(TEXT("asset_path"), AssetPath);
			}
		}
		else if (Value->Type == EJson::Null)
		{
			// 允许设置为 null（清空引用）
			void* Ptr = ObjProp->ContainerPtrToValuePtr<void>(Obj);
			if (Ptr)
			{
				ObjProp->SetPropertyValue(Ptr, nullptr);
				UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Cleared object reference for '%s'"), *Prop->GetName());
				return true;
			}
			return false;
		}
		else
		{
			OutError = TEXT("expects a string (asset path) or object with 'path' field, or null");
			return false;
		}

		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("empty asset path provided");
			return false;
		}

		// 尝试加载资产
		UObject* LoadedAsset = nullptr;
		
		// 尝试不同的路径格式
		TArray<FString> PathsToTry;
		PathsToTry.Add(AssetPath);  // 原始路径
		
		// 如果路径不包含资产名后缀，尝试添加
		if (!AssetPath.Contains(TEXT(".")))
		{
			FString BaseName = FPaths::GetBaseFilename(AssetPath);
			PathsToTry.Add(AssetPath + TEXT(".") + BaseName);
		}
		
		// 如果不是完整路径，尝试常见前缀
		if (!AssetPath.StartsWith(TEXT("/")))
		{
			PathsToTry.Add(TEXT("/Game/") + AssetPath);
			PathsToTry.Add(TEXT("/Engine/") + AssetPath);
		}

		for (const FString& PathToTry : PathsToTry)
		{
			LoadedAsset = LoadObject<UObject>(nullptr, *PathToTry);
			if (LoadedAsset)
			{
				UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Loaded asset from path: %s"), *PathToTry);
				break;
			}
		}

		if (!LoadedAsset)
		{
			// 尝试通过 AssetRegistry 查找（模糊匹配）
			IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
			
			TArray<FAssetData> AssetList;
			FString SearchName = FPaths::GetBaseFilename(AssetPath);
			
			// 尝试获取期望的资产类
			UClass* ExpectedClass = ObjProp->PropertyClass;
			FARFilter Filter;
			if (ExpectedClass)
			{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
				Filter.ClassPaths.Add(ExpectedClass->GetClassPathName());
#else
				Filter.ClassNames.Add(ExpectedClass->GetFName());
#endif
			}
			Filter.bRecursiveClasses = true;
			AssetRegistry.GetAssets(Filter, AssetList);
			
			// 查找匹配的资产
			for (const FAssetData& Asset : AssetList)
			{
				FString AssetName = Asset.AssetName.ToString();
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
				FString FoundPath = Asset.GetObjectPathString();
#else
				FString FoundPath = Asset.ObjectPath.ToString();
#endif
				// 精确匹配或包含匹配
				if (AssetName.Equals(SearchName, ESearchCase::IgnoreCase) ||
					FoundPath.Contains(AssetPath, ESearchCase::IgnoreCase))
				{
					LoadedAsset = Asset.GetAsset();
					if (LoadedAsset)
					{
						UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Found asset via registry: %s"), *FoundPath);
						break;
					}
				}
			}
		}

		if (!LoadedAsset)
		{
			OutError = FString::Printf(TEXT("Failed to load asset: %s (expected type: %s)"), 
				*AssetPath, ObjProp->PropertyClass ? *ObjProp->PropertyClass->GetName() : TEXT("Unknown"));
			return false;
		}

		// 验证类型兼容性
		if (ObjProp->PropertyClass && !LoadedAsset->IsA(ObjProp->PropertyClass))
		{
			OutError = FString::Printf(TEXT("Asset type mismatch: loaded '%s' but expected '%s'"),
				*LoadedAsset->GetClass()->GetName(), *ObjProp->PropertyClass->GetName());
			return false;
		}

		// 设置属性值
		void* Ptr = ObjProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			OutError = TEXT("Failed to get property value pointer");
			return false;
		}
		ObjProp->SetPropertyValue(Ptr, LoadedAsset);
		UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Successfully set object property '%s' to '%s'"),
			*Prop->GetName(), *LoadedAsset->GetPathName());
		return true;
	}

	// === FSoftObjectProperty 支持（软引用） ===
	if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Prop))
	{
		FString AssetPath;
		if (Value->Type == EJson::String)
		{
			AssetPath = Value->AsString();
		}
		else if (Value->Type == EJson::Null)
		{
			void* Ptr = SoftObjProp->ContainerPtrToValuePtr<void>(Obj);
			if (Ptr)
			{
				*static_cast<FSoftObjectPtr*>(Ptr) = FSoftObjectPtr();
				return true;
			}
			return false;
		}
		else
		{
			OutError = TEXT("expects a string (asset path) or null");
			return false;
		}

		void* Ptr = SoftObjProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		*static_cast<FSoftObjectPtr*>(Ptr) = FSoftObjectPath(AssetPath);
		UE_LOG(LogUALUtils, Log, TEXT("[SetSimpleProperty] Set soft object path to: %s"), *AssetPath);
		return true;
	}

	// === FSoftClassProperty 支持（软类引用） ===
	if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
	{
		FString ClassPath;
		if (Value->Type == EJson::String)
		{
			ClassPath = Value->AsString();
		}
		else
		{
			OutError = TEXT("expects a string (class path)");
			return false;
		}

		void* Ptr = SoftClassProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		*static_cast<FSoftObjectPtr*>(Ptr) = FSoftObjectPath(ClassPath);
		return true;
	}

	// === FClassProperty 支持（UClass 引用） ===
	if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
	{
		FString ClassName;
		if (Value->Type == EJson::String)
		{
			ClassName = Value->AsString();
		}
		else if (Value->Type == EJson::Null)
		{
			void* Ptr = ClassProp->ContainerPtrToValuePtr<void>(Obj);
			if (Ptr)
			{
				ClassProp->SetPropertyValue(Ptr, nullptr);
				return true;
			}
			return false;
		}
		else
		{
			OutError = TEXT("expects a string (class name/path)");
			return false;
		}

		// 尝试查找类
		UClass* FoundClass = FindObject<UClass>(ANY_PACKAGE, *ClassName);
		if (!FoundClass)
		{
			FoundClass = LoadObject<UClass>(nullptr, *ClassName);
		}
		if (!FoundClass)
		{
			OutError = FString::Printf(TEXT("Class not found: %s"), *ClassName);
			return false;
		}

		void* Ptr = ClassProp->ContainerPtrToValuePtr<void>(Obj);
		if (!Ptr)
		{
			return false;
		}
		ClassProp->SetPropertyValue(Ptr, FoundClass);
		return true;
	}

	OutError = FString::Printf(TEXT("unsupported property type: %s"), *Prop->GetClass()->GetName());
	return false;
}

FString UAL_CommandUtils::JsonValueToString(const TSharedPtr<FJsonValue>& Value)
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

TSharedPtr<FJsonObject> UAL_CommandUtils::BuildSelectedProps(AActor* Actor, const TArray<FString>& WantedProps)
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

		// Self
		if (TryCollect(Actor, PropName))
		{
			continue;
		}

		// RootComponent
		if (USceneComponent* RootComp = Actor->GetRootComponent())
		{
			if (TryCollect(RootComp, PropName))
			{
				continue;
			}
		}

		// Other Components
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

void UAL_CommandUtils::SendResponse(const FString& RequestId, int32 Code, const TSharedPtr<FJsonObject>& Data)
{
	if (RequestId.IsEmpty())
	{
		return;
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("ver"), TEXT("1.0"));
	Root->SetStringField(TEXT("type"), TEXT("res"));
	Root->SetStringField(TEXT("id"), RequestId);
	Root->SetNumberField(TEXT("code"), Code);
	if (Data.IsValid())
	{
		Root->SetObjectField(TEXT("result"), Data);
	}

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	
	FUAL_NetworkManager::Get().SendMessage(OutputString);
}

void UAL_CommandUtils::SendError(const FString& RequestId, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
	ErrObj->SetStringField(TEXT("message"), Message);
	SendResponse(RequestId, Code, ErrObj);
}
