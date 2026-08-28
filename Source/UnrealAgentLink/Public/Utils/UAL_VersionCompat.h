#pragma once

#include "CoreMinimal.h"
#include "IImageWrapper.h"
#include "AssetRegistry/AssetData.h"

class UStaticMesh;
class UMaterialExpression;

/**
 * 版本兼容适配层，集中处理 5.0 - 5.7 API 差异
 */
namespace UALCompat
{
	/**
	 * PNG 压缩兼容：5.1+ 支持双参 GetCompressed，5.0 仅有返回引用的单参版本。
	 */
	bool GetCompressedPNG(const TSharedPtr<IImageWrapper>& Wrapper, int32 Quality, TArray<uint8>& OutData);

	/**
	 * 静态网格有没有开 Nanite。
	 *
	 * `UStaticMesh::IsNaniteEnabled()` 是 5.1 才加的；5.0 上要自己读
	 * NaniteSettings。5.0 没有 Nanite 的这个访问器不代表没有 Nanite。
	 */
	bool IsNaniteEnabled(const UStaticMesh* Mesh);

	/**
	 * 资产的对象路径字符串。
	 *
	 * 5.1 起 `FAssetData::ObjectPath`（FName）被 `GetObjectPathString()` 取代，
	 * 5.0 上只有前者。两边返回的字符串是一样的。
	 */
	FString GetObjectPathString(const FAssetData& AssetData);

	/**
	 * 材质表达式有几个输入引脚。
	 *
	 * `UMaterialExpression::CountInputs()` 是 5.1 才公开的；5.0 上遍历
	 * `GetInput(i)` 直到拿到空指针。
	 */
	int32 CountInputs(UMaterialExpression* Expression);
}

