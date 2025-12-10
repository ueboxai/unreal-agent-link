// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FUAL_CommandHandler;
class FUAL_LogInterceptor;
class FUAL_NetworkManager;
class FUAL_ContentBrowserExt;

class FToolBarBuilder;
class FMenuBuilder;

class FUnrealAgentLinkModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	
	/** This function will be bound to Command. */
	void PluginButtonClicked();
	
private:

	void HandleSocketMessage(const FString& Data);
	void HandleSocketConnected();
	void RegisterMenus();


private:
	TSharedPtr<class FUICommandList> PluginCommands;

	TUniquePtr<FUAL_CommandHandler> CommandHandler;
	TSharedPtr<FUAL_LogInterceptor> LogInterceptor;
	TUniquePtr<FUAL_ContentBrowserExt> ContentBrowserExt;
};
