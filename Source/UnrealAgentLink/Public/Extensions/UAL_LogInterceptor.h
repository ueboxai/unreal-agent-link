#pragma once

#include "CoreMinimal.h"

/**
 * 日志拦截并上报到 Agent
 */
class FUAL_LogInterceptor : public FOutputDevice
{
public:
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category) override;

	// 是否开启拦截
	bool bIsCaptureEnabled = true;
};

