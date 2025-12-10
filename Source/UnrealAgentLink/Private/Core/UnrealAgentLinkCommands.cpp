// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealAgentLinkCommands.h"

#define LOCTEXT_NAMESPACE "FUnrealAgentLinkModule"

void FUnrealAgentLinkCommands::RegisterCommands()
{
	UI_COMMAND(PluginAction, "UnrealAgentLink", "Unreal Agent Link Status", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
