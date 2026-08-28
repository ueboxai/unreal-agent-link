#pragma once

#include "CoreMinimal.h"
#include "IImageWrapper.h"

/**
 * 版本兼容适配层，集中处理 5.0 - 5.7 API 差异
 */
namespace UALCompat
{
	/**
	 * PNG 压缩兼容：5.1+ 支持双参 GetCompressed，5.0 仅有返回引用的单参版本。
	 */
	bool GetCompressedPNG(const TSharedPtr<IImageWrapper>& Wrapper, int32 Quality, TArray<uint8>& OutData);
}

