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
	TSharedRef<FExtender> OnExtendAssetMenu(const TArray<FAssetData>& SelectedAssets);
	void AddMenuEntry(FMenuBuilder& MenuBuilder, TArray<FString> SelectedPaths);
	void AddAssetMenuEntry(FMenuBuilder& MenuBuilder, TArray<FAssetData> SelectedAssets);
	void HandleImportToAgent(const TArray<FString>& SelectedPaths);
	void HandleImportAssets(const TArray<FAssetData>& SelectedAssets);
	void AddProjectMeta(TSharedPtr<FJsonObject>& Payload) const;

private:
	FDelegateHandle PathExtenderHandle;
	FDelegateHandle AssetExtenderHandle;
	bool bRegistered = false;
};

