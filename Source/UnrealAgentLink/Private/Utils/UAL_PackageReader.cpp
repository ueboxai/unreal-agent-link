// Copyright UnrealAgent. All Rights Reserved.

#include "Utils/UAL_PackageReader.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogUALPackageReader, Log, All);

FUALPackageReader::FUALPackageReader()
    : Loader(nullptr)
    , PackageFileSize(0)
{
    SetIsLoading(true);
    SetIsPersistent(true);
}

FUALPackageReader::~FUALPackageReader()
{
    if (Loader)
    {
        delete Loader;
        Loader = nullptr;
    }
}

bool FUALPackageReader::OpenPackageFile(const FString& InFilePath)
{
    PackageFilename = InFilePath;
    
    // 创建文件读取器
    Loader = IFileManager::Get().CreateFileReader(*PackageFilename);
    if (!Loader)
    {
        UE_LOG(LogUALPackageReader, Warning, TEXT("无法打开文件: %s"), *PackageFilename);
        return false;
    }
    
    // 读取包摘要
    *Loader << PackageFileSummary;
    
    // 验证是否是有效的 UE 包
    if (PackageFileSummary.Tag != PACKAGE_FILE_TAG || Loader->IsError())
    {
        UE_LOG(LogUALPackageReader, Warning, TEXT("无效的包文件: %s"), *PackageFilename);
        return false;
    }
    
    // 设置版本信息
    SetUEVer(PackageFileSummary.GetFileVersionUE());
    SetLicenseeUEVer(PackageFileSummary.GetFileVersionLicenseeUE());
    SetEngineVer(PackageFileSummary.SavedByEngineVersion);
    
    Loader->SetUEVer(PackageFileSummary.GetFileVersionUE());
    Loader->SetLicenseeUEVer(PackageFileSummary.GetFileVersionLicenseeUE());
    Loader->SetEngineVer(PackageFileSummary.SavedByEngineVersion);
    
    SetByteSwapping(Loader->ForceByteSwapping());
    
    const FCustomVersionContainer& Versions = PackageFileSummary.GetCustomVersionContainer();
    SetCustomVersions(Versions);
    Loader->SetCustomVersions(Versions);
    
    PackageFileSize = Loader->TotalSize();
    
    return true;
}

bool FUALPackageReader::SerializeNameMap()
{
    if (PackageFileSummary.NameCount <= 0)
    {
        return true;
    }
    
    if (PackageFileSummary.NameOffset <= 0 || PackageFileSummary.NameOffset > PackageFileSize)
    {
        return false;
    }
    
    Seek(PackageFileSummary.NameOffset);
    NameMap.Reserve(PackageFileSummary.NameCount);
    
    for (int32 i = 0; i < PackageFileSummary.NameCount; ++i)
    {
        FNameEntrySerialized NameEntry(ENAME_LinkerConstructor);
        *Loader << NameEntry;
        
        if (Loader->IsError())
        {
            UE_LOG(LogUALPackageReader, Warning, TEXT("读取 NameMap 失败 [%d]"), i);
            return false;
        }
        
        NameMap.Add(FName(NameEntry));
    }
    
    return true;
}

bool FUALPackageReader::SerializeImportMap()
{
    if (ImportMap.Num() > 0)
    {
        return true;
    }

    if (PackageFileSummary.ImportCount <= 0)
    {
        return true;
    }
    
    if (PackageFileSummary.ImportOffset <= 0 || PackageFileSummary.ImportOffset > PackageFileSize)
    {
        return false;
    }
    
    Seek(PackageFileSummary.ImportOffset);
    ImportMap.Reserve(PackageFileSummary.ImportCount);
    
    for (int32 i = 0; i < PackageFileSummary.ImportCount; ++i)
    {
        FObjectImport& Import = ImportMap.AddDefaulted_GetRef();
        *this << Import;
        
        if (IsError())
        {
            UE_LOG(LogUALPackageReader, Warning, TEXT("读取 ImportMap 失败 [%d]"), i);
            ImportMap.Reset();
            return false;
        }
    }
    
    return true;
}

bool FUALPackageReader::SerializeExportMap()
{
    if (ExportMap.Num() > 0)
    {
        return true;
    }

    if (PackageFileSummary.ExportCount <= 0)
    {
        return true;
    }

    if (PackageFileSummary.ExportOffset <= 0 || PackageFileSummary.ExportOffset > PackageFileSize)
    {
        return false;
    }

    Seek(PackageFileSummary.ExportOffset);
    ExportMap.Reserve(PackageFileSummary.ExportCount);

    for (int32 i = 0; i < PackageFileSummary.ExportCount; ++i)
    {
        FObjectExport& Export = ExportMap.AddDefaulted_GetRef();
        *this << Export;

        if (IsError())
        {
            UE_LOG(LogUALPackageReader, Warning, TEXT("读取 ExportMap 失败 [%d]"), i);
            ExportMap.Reset();
            return false;
        }
    }

    return true;
}

bool FUALPackageReader::GetAssetClass(FString& OutClassName)
{
    // 需要先序列化所有 Map
    if (NameMap.Num() == 0 && !SerializeNameMap()) return false;
    if (ImportMap.Num() == 0 && !SerializeImportMap()) return false;
    if (ExportMap.Num() == 0 && !SerializeExportMap()) return false;

    // 定义需要跳过的辅助类型（这些不是真正的资产类型）
    auto IsAuxiliaryClass = [](const FString& ClassName) -> bool
    {
        static const TSet<FString> AuxClasses = {
            TEXT("MetaData"),
            TEXT("ObjectRedirector"),
            TEXT("AssetUserData"),
            TEXT("ThumbnailInfo")
        };
        return AuxClasses.Contains(ClassName);
    };

    // 第一遍：查找 Outer 为 NULL 的顶级对象，跳过辅助类型
    FString FirstFoundClass;
    bool bFoundAny = false;
    
    for (const FObjectExport& Export : ExportMap)
    {
        if (Export.OuterIndex.IsNull())
        {
            if (Export.ClassIndex.IsImport())
            {
                int32 ImportIdx = Export.ClassIndex.ToImport();
                if (ImportMap.IsValidIndex(ImportIdx))
                {
                    FString ClassName = ImportMap[ImportIdx].ObjectName.ToString();
                    
                    // 记录第一个找到的（作为备用）
                    if (!bFoundAny)
                    {
                        FirstFoundClass = ClassName;
                        bFoundAny = true;
                    }
                    
                    // 如果不是辅助类型，直接返回
                    if (!IsAuxiliaryClass(ClassName))
                    {
                        OutClassName = ClassName;
                        return true;
                    }
                }
            }
        }
    }

    // 第二遍：如果只找到了辅助类型，遍历所有 Export 查找非辅助类型
    for (const FObjectExport& Export : ExportMap)
    {
        if (Export.ClassIndex.IsImport())
        {
            int32 ImportIdx = Export.ClassIndex.ToImport();
            if (ImportMap.IsValidIndex(ImportIdx))
            {
                FString ClassName = ImportMap[ImportIdx].ObjectName.ToString();
                if (!IsAuxiliaryClass(ClassName))
                {
                    OutClassName = ClassName;
                    return true;
                }
            }
        }
    }

    // 如果全部都是辅助类型，返回第一个找到的
    if (bFoundAny)
    {
        OutClassName = FirstFoundClass;
        return true;
    }

    return false;
}



bool FUALPackageReader::ReadDependencies(TArray<FName>& OutDependencies)
{
    // 先序列化 NameMap（用于 FName 反序列化）
    if (!SerializeNameMap())
    {
        UE_LOG(LogUALPackageReader, Warning, TEXT("SerializeNameMap 失败"));
        return false;
    }
    
    // 再序列化 ImportMap
    if (!SerializeImportMap())
    {
        UE_LOG(LogUALPackageReader, Warning, TEXT("SerializeImportMap 失败"));
        return false;
    }
    
    // 从 ImportMap 提取依赖
    TSet<FName> UniquePackages;
    FName LinkerName(*PackageFilename);
    
    for (int32 i = 0; i < ImportMap.Num(); ++i)
    {
        const FObjectImport& Import = ImportMap[i];
        FName DependentPackageName = NAME_None;
        
        // 查找导入的根包
        if (!Import.OuterIndex.IsNull())
        {
            FPackageIndex OutermostIndex = Import.OuterIndex;
            
            // 遍历找到最外层的包
            while (!OutermostIndex.IsNull() && OutermostIndex.IsImport())
            {
                int32 ImportIndex = OutermostIndex.ToImport();
                if (ImportMap.IsValidIndex(ImportIndex))
                {
                    if (ImportMap[ImportIndex].OuterIndex.IsNull())
                    {
                        DependentPackageName = ImportMap[ImportIndex].ObjectName;
                        break;
                    }
                    OutermostIndex = ImportMap[ImportIndex].OuterIndex;
                }
                else
                {
                    break;
                }
            }
        }
        
        // 如果是顶层 Package 类型的导入
        if (DependentPackageName == NAME_None && Import.ClassName == NAME_Package)
        {
            DependentPackageName = Import.ObjectName;
        }
        
        // 过滤并添加
        if (DependentPackageName != NAME_None && DependentPackageName != LinkerName)
        {
            FString PackageStr = DependentPackageName.ToString();
            
            // 跳过引擎包
            if (!PackageStr.StartsWith(TEXT("/Script/")) && 
                !PackageStr.StartsWith(TEXT("/Engine/")))
            {
                UniquePackages.Add(DependentPackageName);
            }
        }
        
        // 也添加 ClassPackage
        if (Import.ClassPackage != NAME_None && Import.ClassPackage != LinkerName)
        {
            FString ClassPackageStr = Import.ClassPackage.ToString();
            if (!ClassPackageStr.StartsWith(TEXT("/Script/")) && 
                !ClassPackageStr.StartsWith(TEXT("/Engine/")))
            {
                UniquePackages.Add(Import.ClassPackage);
            }
        }
    }
    
    // 输出结果
    OutDependencies = UniquePackages.Array();
    
    UE_LOG(LogUALPackageReader, Log, TEXT("从 %s 读取到 %d 个依赖"), 
        *FPaths::GetCleanFilename(PackageFilename), OutDependencies.Num());
    
    return true;
}

// FArchive 接口实现
void FUALPackageReader::Serialize(void* V, int64 Length)
{
    if (Loader)
    {
        Loader->Serialize(V, Length);
    }
}

void FUALPackageReader::Seek(int64 InPos)
{
    if (Loader)
    {
        Loader->Seek(InPos);
    }
}

int64 FUALPackageReader::Tell()
{
    return Loader ? Loader->Tell() : 0;
}

int64 FUALPackageReader::TotalSize()
{
    return PackageFileSize;
}

FArchive& FUALPackageReader::operator<<(FName& Name)
{
    int32 NameIndex = 0;
    int32 Number = 0;
    
    *Loader << NameIndex;
    *Loader << Number;
    
    if (NameMap.IsValidIndex(NameIndex))
    {
        Name = FName(NameMap[NameIndex], Number);
    }
    else
    {
        Name = NAME_None;
        SetError();
    }
    
    return *this;
}
