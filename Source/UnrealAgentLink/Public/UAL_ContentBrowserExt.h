#pragma once

#include "CoreMinimal.h"

/**
 * 内容浏览器路径（文件夹）右键扩展
 */
class FUAL_ContentBrowserExt
{
public:
	void Register();
	void Unregister();

private:
	TSharedRef<FExtender> OnExtendPathMenu(const TArray<FString>& SelectedPaths);
	void AddMenuEntry(FMenuBuilder& MenuBuilder, TArray<FString> SelectedPaths);
	void HandleImportToAgent(const TArray<FString>& SelectedPaths);

private:
	FDelegateHandle ExtenderHandle;
	bool bRegistered = false;
};

