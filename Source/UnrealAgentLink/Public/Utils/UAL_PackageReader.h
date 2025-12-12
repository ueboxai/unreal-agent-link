// Copyright UnrealAgent. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Serialization/ArchiveUObject.h"
#include "UObject/ObjectResource.h"
#include "UObject/PackageFileSummary.h"

/**
 * 轻量级包读取器
 * 用于从外部 .uasset/.umap 文件中提取依赖信息
 */
class UNREALAGENTLINK_API FUALPackageReader : public FArchiveUObject
{
public:
    FUALPackageReader();
    ~FUALPackageReader();

    /**
     * 打开包文件
     * @param InFilePath - 包文件的绝对路径
     * @return 成功返回 true
     */
    bool OpenPackageFile(const FString& InFilePath);

    /**
     * 读取依赖包列表（硬引用）
     * @param OutDependencies - 输出依赖的包路径
     * @return 成功返回 true
     */
    bool ReadDependencies(TArray<FName>& OutDependencies);

    /** 
     * 获取主要资产类型 
     * @param OutClassName - 输出类名（不带前缀，例如 "StaticMesh", "Texture2D", "Material"）
     * @return 成功返回 true
     */
    bool GetAssetClass(FString& OutClassName);

    // FArchive 接口实现
    virtual void Serialize(void* V, int64 Length) override;
    virtual void Seek(int64 InPos) override;
    virtual int64 Tell() override;
    virtual int64 TotalSize() override;
    virtual FArchive& operator<<(FName& Name) override;
    virtual FString GetArchiveName() const override { return PackageFilename; }

private:
    /** 序列化 NameMap */
    bool SerializeNameMap();
    
    /** 序列化 ImportMap */
    bool SerializeImportMap();

    /** 序列化 ExportMap */
    bool SerializeExportMap();

    FString PackageFilename;
    FArchive* Loader;
    FPackageFileSummary PackageFileSummary;
    TArray<FName> NameMap;
    TArray<FObjectImport> ImportMap;
    TArray<FObjectExport> ExportMap;
    int64 PackageFileSize;
};
