// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "UnrealAgentLinkStyle.h"

class FUnrealAgentLinkCommands : public TCommands<FUnrealAgentLinkCommands>
{
public:

	FUnrealAgentLinkCommands()
		: TCommands<FUnrealAgentLinkCommands>(TEXT("UnrealAgentLink"), NSLOCTEXT("Contexts", "UnrealAgentLink", "UnrealAgentLink Plugin"), NAME_None, FUnrealAgentLinkStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > PluginAction;
};
