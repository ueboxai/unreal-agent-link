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
#include "UObject/SoftObjectPath.h"

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

	const TSharedPtr<FJsonObject>* PayloadObj = nullptr;
	Root->TryGetObjectField(TEXT("payload"), PayloadObj);

	UE_LOG(LogUALCommand, Display, TEXT("Recv message type=%s method=%s id=%s"), *Type, *Method, *RequestId);

	if (Type != TEXT("req"))
	{
		if (Type == TEXT("res"))
		{
			Handle_Response(Method, PayloadObj ? *PayloadObj : nullptr);
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

	(*Handler)(PayloadObj ? *PayloadObj : MakeShared<FJsonObject>(), RequestId);
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

	CommandMap.Add(TEXT("actor.spawn"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_SpawnActor(Payload, RequestId);
	});

	CommandMap.Add(TEXT("actor.destroy"), [this](const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
	{
		Handle_DestroyActor(Payload, RequestId);
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

void FUAL_CommandHandler::Handle_SpawnActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const FString PresetName = Payload->GetStringField(TEXT("preset"));
	const FString ClassPath = Payload->GetStringField(TEXT("class"));
	const FString DesiredName = Payload->GetStringField(TEXT("name"));

	if (PresetName.IsEmpty() && ClassPath.IsEmpty())
	{
		SendError(RequestId, 400, TEXT("Missing field: preset or class"));
		return;
	}

	UWorld* World = GetTargetWorld();
	if (!World)
	{
		SendError(RequestId, 500, TEXT("World not available"));
		return;
	}

	FUALSpawnPreset Preset;
	TSubclassOf<AActor> SpawnClass = nullptr;
	const TCHAR* MeshPath = nullptr;

	if (!PresetName.IsEmpty())
	{
		if (!ResolvePreset(PresetName, Preset))
		{
			SendError(RequestId, 404, *FString::Printf(TEXT("Unknown preset: %s"), *PresetName));
			return;
		}
		SpawnClass = Preset.Class;
		MeshPath = Preset.AssetPath;
	}

	if (SpawnClass == nullptr && !ClassPath.IsEmpty())
	{
		UObject* LoadedClassObj = StaticLoadObject(UClass::StaticClass(), nullptr, *ClassPath);
		if (LoadedClassObj)
		{
			SpawnClass = Cast<UClass>(LoadedClassObj);
		}
	}

	if (SpawnClass == nullptr)
	{
		SendError(RequestId, 404, *FString::Printf(TEXT("Cannot load class: %s"), *ClassPath));
		return;
	}

	const FVector Location = ReadVector(Payload, TEXT("location"), FVector::ZeroVector);
	const FRotator Rotation = ReadRotator(Payload, TEXT("rotation"), FRotator::ZeroRotator);
	const FVector Scale = ReadVector(Payload, TEXT("scale"), FVector(1, 1, 1));

	FActorSpawnParameters Params;
	if (!DesiredName.IsEmpty())
	{
		Params.Name = FName(*DesiredName);
		Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
	}

	// UWorld::SpawnActor API 在 5.0-5.7 统一接受指针参数版本，使用显式地址避免版本差异
	const FTransform SpawnTransform(Rotation, Location);
	AActor* Actor = World->SpawnActor(SpawnClass, &SpawnTransform, Params);
	if (!Actor)
	{
		SendError(RequestId, 500, TEXT("Spawn failed"));
		return;
	}

	if (!SetStaticMeshIfNeeded(Actor, MeshPath))
	{
		Actor->Destroy();
		SendError(RequestId, 404, TEXT("Failed to load mesh for preset"));
		return;
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
	if (!PresetName.IsEmpty())
	{
		Data->SetStringField(TEXT("preset"), PresetName);
	}

	SendResponse(RequestId, 200, Data);
}

void FUAL_CommandHandler::Handle_DestroyActor(const TSharedPtr<FJsonObject>& Payload, const FString RequestId)
{
	const FString Label = Payload->GetStringField(TEXT("name"));
	const FString Path = Payload->GetStringField(TEXT("path"));

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

	const FString ActorLabel = GetActorFriendlyName(TargetActor);

	const bool bDestroyed = TargetActor->Destroy();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), bDestroyed);
	Data->SetStringField(TEXT("name"), ActorLabel);
	if (!Path.IsEmpty())
	{
		Data->SetStringField(TEXT("path"), Path);
	}

	SendResponse(RequestId, bDestroyed ? 200 : 500, Data);
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
		Root->SetObjectField(TEXT("data"), Data);
	}

	FString OutJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	FUAL_NetworkManager::Get().SendMessage(OutJson);
}

void FUAL_CommandHandler::SendError(const FString& RequestId, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("message"), Message);
	SendResponse(RequestId, Code, Data);
}

