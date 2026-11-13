#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
class FOSMLoader;

class FHiroGeoRustOsmAdapter
{
public:
    static TUniquePtr<FOSMLoader> LoadXml(const FString& DllPath, const TArray<uint8>& Source,
        const FOSMLoader& ReferenceBounds, FString& Error);
};
#endif
