#include "UAL_VersionCompat.h"

#include "Modules/ModuleManager.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialExpression.h"

namespace UALCompat
{
	/**
	 * 获取压缩后的图像数据
	 * 兼容 UE 5.0-5.7：统一使用 GetCompressed(Quality) 返回值
	 */
	bool GetCompressedPNG(const TSharedPtr<IImageWrapper>& Wrapper, int32 Quality, TArray<uint8>& OutData)
	{
		if (!Wrapper.IsValid())
		{
			return false;
		}

		// 所有 UE5 版本都使用 GetCompressed(Quality) 返回 TArray64<uint8>
		const TArray64<uint8>& CompressedRef = Wrapper->GetCompressed(Quality);
		OutData.Reset(CompressedRef.Num());
		OutData.Append(CompressedRef.GetData(), CompressedRef.Num());
		return OutData.Num() > 0;
	}

	bool IsNaniteEnabled(const UStaticMesh* Mesh)
	{
		if (!Mesh)
		{
			return false;
		}
// UStaticMesh::IsNaniteEnabled() 是 **5.3** 才有的。
		// 逐版本查过引擎头文件确认，不要凭印象改这个数字。
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 3)
		return Mesh->IsNaniteEnabled();
#else
		// 5.0–5.2 没有访问器，直接读设置结构
		return Mesh->NaniteSettings.bEnabled;
#endif
	}

	FString GetObjectPathString(const FAssetData& AssetData)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		return AssetData.GetObjectPathString();
#else
		return AssetData.ObjectPath.ToString();
#endif
	}

	int32 CountInputs(UMaterialExpression* Expression)
	{
		if (!Expression)
		{
			return 0;
		}
// UMaterialExpression::CountInputs() 是 **5.5** 才公开的。
		// 逐版本查过引擎头文件确认，不要凭印象改这个数字。
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5)
		return Expression->CountInputs();
#else
		// 5.0–5.4 上没有这个方法，遍历到第一个空输入为止。
		// 加个上限兜底：GetInput 的实现万一不返回 nullptr 就会转成死循环。
		constexpr int32 MaxInputs = 64;
		int32 Count = 0;
		while (Count < MaxInputs && Expression->GetInput(Count) != nullptr)
		{
			++Count;
		}
		return Count;
#endif
	}
}
