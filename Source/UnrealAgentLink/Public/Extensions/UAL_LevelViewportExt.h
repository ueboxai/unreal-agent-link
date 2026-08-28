#pragma once

#include "CoreMinimal.h"

class AActor;

/**
 * 关卡视口Actor右键菜单扩展
 * 在视口中选中Actor后右键，可以看到"导入到虚幻盒子资产库"选项
 */
class FUAL_LevelViewportExt
{
public:
	/**
	 * 注册视口Actor上下文菜单扩展
	 */
	void Register();

	/**
	 * 取消注册
	 */
	void Unregister();

private:
	/**
	 * 菜单扩展回调
	 * @param Extender 菜单扩展器
	 * @param CommandList UI命令列表
	 * @param SelectedActors 选中的Actor列表
	 */
	TSharedRef<FExtender> OnExtendActorContextMenu(
		const TSharedRef<FUICommandList> CommandList,
		const TArray<AActor*> SelectedActors);

	/**
	 * 添加菜单项
	 * @param MenuBuilder 菜单构建器
	 * @param SelectedActors 选中的Actor列表
	 */
	void AddMenuEntry(FMenuBuilder& MenuBuilder, TArray<AActor*> SelectedActors);

	/**
	 * 处理导入选中Actor的资产到虚幻盒子
	 * @param SelectedActors 选中的Actor列表
	 */
	void HandleImportActorAssets(const TArray<AActor*>& SelectedActors);

	/**
	 * 添加项目元数据到Payload
	 */
	void AddProjectMeta(TSharedPtr<FJsonObject>& Payload) const;

private:
	/** 扩展器委托句柄 */
	FDelegateHandle ExtenderHandle;
	
	/** 是否已注册 */
	bool bRegistered = false;
};
