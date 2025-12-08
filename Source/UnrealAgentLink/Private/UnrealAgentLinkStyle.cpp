// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealAgentLinkStyle.h"
#include "UnrealAgentLink.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateStyleRegistry.h"
#include "Slate/SlateGameResources.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"

#define RootToContentDir Style->RootToContentDir

TSharedPtr<FSlateStyleSet> FUnrealAgentLinkStyle::StyleInstance = nullptr;

void FUnrealAgentLinkStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FUnrealAgentLinkStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FUnrealAgentLinkStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("UnrealAgentLinkStyle"));
	return StyleSetName;
}


const FVector2D Icon16x16(16.0f, 16.0f);
const FVector2D Icon20x20(20.0f, 20.0f);

TSharedRef< FSlateStyleSet > FUnrealAgentLinkStyle::Create()
{
	TSharedRef< FSlateStyleSet > Style = MakeShareable(new FSlateStyleSet("UnrealAgentLinkStyle"));
	Style->SetContentRoot(IPluginManager::Get().FindPlugin("UnrealAgentLink")->GetBaseDir() / TEXT("Resources"));

	Style->Set("UnrealAgentLink.PluginAction", new IMAGE_BRUSH_SVG(TEXT("PlaceholderButtonIcon"), Icon20x20));
	return Style;
}

void FUnrealAgentLinkStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FUnrealAgentLinkStyle::Get()
{
	return *StyleInstance;
}
