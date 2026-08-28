#include "UAL_VersionCompat.h"

#include "Modules/ModuleManager.h"

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
}

