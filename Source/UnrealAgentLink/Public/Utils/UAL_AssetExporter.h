// Copyright 2024 Unreal Box. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"

/**
 * 资产导出工具类
 * 统一处理从视口和内容浏览器导出资产到虚幻盒子的逻辑
 */
class UNREALAGENTLINK_API FUAL_AssetExporter
{
public:
	/**
	 * 资产导出结果
	 */
	struct FExportResult
	{
		/** 资产包路径数组 */
		TArray<TSharedPtr<FJsonValue>> AssetPathsArray;
		/** 资产真实文件路径数组 */
		TArray<TSharedPtr<FJsonValue>> AssetRealPathsArray;
		/** 资产元数据数组 */
		TArray<TSharedPtr<FJsonValue>> AssetMetadataArray;
		/** 用户选中的主资产数量 */
		int32 MainAssetCount = 0;
		/** 依赖资产数量 */
		int32 DependencyCount = 0;
	};

	/**
	 * 收集资产及其依赖并构建导出数据
	 * @param SelectedAssets 用户选中的资产列表
	 * @param bCollectDependencies 是否收集依赖（默认 true）
	 * @param ThumbnailSize 缩略图大小（默认 512）
	 * @return 导出结果
	 */
	static FExportResult CollectAssetsForExport(
		const TArray<FAssetData>& SelectedAssets,
		bool bCollectDependencies = true,
		int32 ThumbnailSize = 512
	);

	/**
	 * 构建完整的导出 JSON 消息
	 * @param ExportResult 导出结果
	 * @param Method 消息方法名（如 "content.import_assets"）
	 * @return JSON 字符串
	 */
	static FString BuildExportMessage(const FExportResult& ExportResult, const FString& Method);

	/**
	 * 添加项目元数据到 Payload
	 * @param Payload 目标 JSON 对象
	 */
	static void AddProjectMeta(TSharedPtr<FJsonObject>& Payload);

	/**
	 * 获取资产缩略图并保存到临时文件
	 * @param AssetData 资产数据
	 * @param ThumbnailSize 缩略图大小
	 * @return PNG 文件路径，失败返回空字符串
	 */
	static FString SaveAssetThumbnailToFile(const FAssetData& AssetData, int32 ThumbnailSize = 512);
};
