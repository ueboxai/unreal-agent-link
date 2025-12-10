#include "UAL_VersionCompat.h"

#include "Modules/ModuleManager.h"

namespace UALCompat
{
	bool GetCompressedPNG(const TSharedPtr<IImageWrapper>& Wrapper, int32 Quality, TArray<uint8>& OutData)
	{
		if (!Wrapper.IsValid())
		{
			return false;
		}

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1)
		return Wrapper->GetCompressed(Quality, OutData);
#else
		const auto& CompressedRef = Wrapper->GetCompressed(Quality);
		OutData.Reset(CompressedRef.Num());
		OutData.Append(CompressedRef.GetData(), CompressedRef.Num());
		return OutData.Num() > 0;
#endif
	}
}

