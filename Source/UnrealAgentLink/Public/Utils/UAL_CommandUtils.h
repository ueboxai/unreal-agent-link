#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UAL_CommandUtils
{
public:
	static bool IsZh();
	static FString LStr(const TCHAR* Zh, const TCHAR* En);
	static FText LText(const TCHAR* Zh, const TCHAR* En);

	static UWorld* GetTargetWorld();

	static FVector ReadVector(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector& DefaultValue = FVector::ZeroVector);
	static FRotator ReadRotator(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FRotator& DefaultValue = FRotator::ZeroRotator);
	static FVector ReadVectorDirect(const TSharedPtr<FJsonObject>& Obj, const FVector& DefaultValue = FVector::ZeroVector);
	static FRotator ReadRotatorDirect(const TSharedPtr<FJsonObject>& Obj, const FRotator& DefaultValue = FRotator::ZeroRotator);
	
	static bool TryGetObjectFieldFlexible(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Field, TSharedPtr<FJsonObject>& OutObj);

	struct FUALSpawnPreset
	{
		FName Key;
		UClass* Class; // TSubclassOf<AActor>
		const TCHAR* AssetPath;
	};
	static bool ResolvePreset(const FString& Name, FUALSpawnPreset& OutPreset);
	
	static bool SetStaticMeshIfNeeded(AActor* Actor, const TCHAR* MeshPath);

	struct FUALResolvedSpawnRequest
	{
		UClass* SpawnClass = nullptr;
		FString MeshPath;
		FString ResolvedType;
		FString SourceId;
		bool bFromAlias = false;
	};
	static bool ResolveSpawnFromAssetId(const FString& AssetId, FUALResolvedSpawnRequest& OutResolved, FString& OutError);

	static void ReadTransformFromItem(const TSharedPtr<FJsonObject>& Item, FVector& OutLocation, FRotator& OutRotation, FVector& OutScale);

	static AActor* FindActorByLabel(UWorld* World, const FString& Label);
	static FString GetActorFriendlyName(AActor* Actor);

	static TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Vec);
	static TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Rot);
	
	static TSharedPtr<FJsonValue> PropertyToJsonValueCompat(FProperty* Prop, const void* ValuePtr);
	
	static TSharedPtr<FJsonObject> BuildActorInfo(AActor* Actor);
	static TSharedPtr<FJsonObject> BuildActorInfoWithOptions(AActor* Actor, bool bIncludeTransform, bool bIncludeBounds);

	static bool ShouldIncludeActor(const AActor* Actor, const FString& NameKeyword, bool bNameExact, const FString& ClassKeyword, bool bClassExact);

	static bool ShouldIncludeActorAdvanced(
		const AActor* Actor,
		const FString& NameContains,
		const FString& NameNotContains,
		const FString& ClassContains,
		const FString& ClassNotContains,
		const FString& ClassExact,
		const TArray<FString>& ExcludeClasses);

	static UClass* ResolveClassFromIdentifier(const FString& Identifier, UClass* ExpectedBase, FString& OutError);

	static bool ResolveTargetsToActors(const TSharedPtr<FJsonObject>& Targets, UWorld* World, TSet<AActor*>& OutSet, FString& OutError);

	/**
	 * 检查 Actor 的属性是否匹配指定条件
	 * @param Actor 要检查的 Actor
	 * @param PropName 属性名（会自动在 Actor 和 RootComponent 上搜索）
	 * @param ExpectedValue 期望匹配的值（模糊包含匹配，忽略大小写）
	 * @return 是否匹配
	 */
	static bool CheckPropertyMatch(AActor* Actor, const FString& PropName, const FString& ExpectedValue);

	static bool ApplyStructValue(FStructProperty* StructProp, UObject* Target, const TSharedPtr<FJsonValue>& JsonValue);

	static const TArray<FString>& GetDefaultInspectProps();
	
	static bool TryCollectProperty(UObject* Obj, const FString& PropName, TSharedPtr<FJsonObject>& OutProps);
	static void CollectPropertyNames(UObject* Obj, TArray<FString>& OutNames);

	static int32 LevenshteinDistance(const FString& A, const FString& B);
	static void SuggestProperties(const FString& Input, const TArray<FString>& Candidates, TArray<FString>& OutSuggestions, int32 MaxSuggestions = 5);

	static FProperty* FindWritableProperty(UObject* Obj, const FString& PropName);
	static FProperty* FindWritablePropertyOnActorHierarchy(AActor* Actor, const FString& PropName, UObject*& OutTargetObj);

	static bool SetNumericProperty(FNumericProperty* NumProp, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError);
	static bool SetStructProperty(FStructProperty* StructProp, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError);
	static bool SetSimpleProperty(FProperty* Prop, UObject* Obj, const TSharedPtr<FJsonValue>& Value, FString& OutError);

	static FString JsonValueToString(const TSharedPtr<FJsonValue>& Value);
	static TSharedPtr<FJsonObject> BuildSelectedProps(AActor* Actor, const TArray<FString>& WantedProps);

	// Network Helpers
	static void SendResponse(const FString& RequestId, int32 Code, const TSharedPtr<FJsonObject>& Data = nullptr);
	static void SendError(const FString& RequestId, int32 Code, const FString& Message);
};
