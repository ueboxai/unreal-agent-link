// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealAgentLink.h"
#include "UnrealAgentLinkStyle.h"
#include "UnrealAgentLinkCommands.h"
#include "Misc/MessageDialog.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "ToolMenus.h"
#include "UAL_NetworkManager.h"
#include "UAL_CommandHandler.h"
#include "UAL_LogInterceptor.h"
#include "UAL_ContentBrowserExt.h"
#include "Async/Async.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

static const FName UnrealAgentLinkTabName("UnrealAgentLink");

#define LOCTEXT_NAMESPACE "FUnrealAgentLinkModule"

void FUnrealAgentLinkModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	
	FUnrealAgentLinkStyle::Initialize();
	FUnrealAgentLinkStyle::ReloadTextures();

	FUnrealAgentLinkCommands::Register();
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FUnrealAgentLinkCommands::Get().PluginAction,
		FExecuteAction::CreateRaw(this, &FUnrealAgentLinkModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUnrealAgentLinkModule::RegisterMenus));

	// 初始化核心组件
	CommandHandler = MakeUnique<FUAL_CommandHandler>();
	LogInterceptor = MakeShared<FUAL_LogInterceptor>();
	ContentBrowserExt = MakeUnique<FUAL_ContentBrowserExt>();

	if (GLog && LogInterceptor.IsValid())
	{
		GLog->AddOutputDevice(LogInterceptor.Get());
	}

	// 默认连接到本地 Agent
	const FString DefaultUrl = TEXT("ws://127.0.0.1:17860");
	FUAL_NetworkManager::Get().OnMessageReceived().AddRaw(this, &FUnrealAgentLinkModule::HandleSocketMessage);
	FUAL_NetworkManager::Get().OnConnected().AddRaw(this, &FUnrealAgentLinkModule::HandleSocketConnected);
	FUAL_NetworkManager::Get().Init(DefaultUrl);

	// 注册内容浏览器菜单扩展
	if (ContentBrowserExt)
	{
		ContentBrowserExt->Register();
	}
}

void FUnrealAgentLinkModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.

	// 在关闭连接前发送项目关闭事件
	if (FUAL_NetworkManager::Get().IsConnected())
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("ver"), TEXT("1.0"));
		Root->SetStringField(TEXT("type"), TEXT("evt"));
		Root->SetStringField(TEXT("method"), TEXT("project.closed"));
		
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("projectName"), FApp::GetProjectName());
		Payload->SetStringField(TEXT("projectPath"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		Root->SetObjectField(TEXT("payload"), Payload);

		FString OutJson;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

		FUAL_NetworkManager::Get().SendMessage(OutJson);
		
		// 等待一小段时间确保消息发送完成
		FPlatformProcess::Sleep(0.1f);
	}

	UToolMenus::UnRegisterStartupCallback(this);

	UToolMenus::UnregisterOwner(this);

	FUnrealAgentLinkStyle::Shutdown();

	FUnrealAgentLinkCommands::Unregister();

	FUAL_NetworkManager::Get().OnMessageReceived().RemoveAll(this);
	FUAL_NetworkManager::Get().OnConnected().RemoveAll(this);
	FUAL_NetworkManager::Get().Shutdown();

	if (GLog && LogInterceptor.IsValid())
	{
		GLog->RemoveOutputDevice(LogInterceptor.Get());
	}
	LogInterceptor.Reset();
	CommandHandler.Reset();

	if (ContentBrowserExt)
	{
		ContentBrowserExt->Unregister();
		ContentBrowserExt.Reset();
	}
}

void FUnrealAgentLinkModule::PluginButtonClicked()
{
	const FString Status = FUAL_NetworkManager::Get().IsConnected() ? TEXT("已连接") : TEXT("未连接");
	FText DialogText = FText::FromString(FString::Printf(TEXT("UnrealAgentLink 状态：%s"), *Status));
	FMessageDialog::Open(EAppMsgType::Ok, DialogText);
}

void FUnrealAgentLinkModule::RegisterMenus()
{
	// Owner will be used for cleanup in call to UToolMenus::UnregisterOwner
	FToolMenuOwnerScoped OwnerScoped(this);

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			Section.AddMenuEntryWithCommandList(FUnrealAgentLinkCommands::Get().PluginAction, PluginCommands);
		}
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
			{
				FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FUnrealAgentLinkCommands::Get().PluginAction));
				Entry.SetCommandList(PluginCommands);
			}
		}
	}
}

void FUnrealAgentLinkModule::HandleSocketMessage(const FString& Data)
{
	// 将 Socket 线程收到的数据切回 GameThread
	AsyncTask(ENamedThreads::GameThread, [this, Data]()
	{
		if (CommandHandler)
		{
			CommandHandler->ProcessMessage(Data);
		}
	});
}

void FUnrealAgentLinkModule::HandleSocketConnected()
{
	// 将回调切回 GameThread 构建数据并发送
	AsyncTask(ENamedThreads::GameThread, [this]()
	{
		if (!CommandHandler)
		{
			return;
		}

		const TSharedPtr<FJsonObject> Payload = CommandHandler->BuildProjectInfo();
		if (!Payload.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("ver"), TEXT("1.0"));
		Root->SetStringField(TEXT("type"), TEXT("evt"));
		Root->SetStringField(TEXT("method"), TEXT("project.info"));
		Root->SetObjectField(TEXT("payload"), Payload);

		FString OutJson;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
		FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

		FUAL_NetworkManager::Get().SendMessage(OutJson);
	});
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUnrealAgentLinkModule, UnrealAgentLink)