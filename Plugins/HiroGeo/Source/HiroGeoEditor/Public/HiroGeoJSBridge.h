// Copyright Hiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "HiroGeoJSBridge.generated.h"

// دیلیگیت با ۵ پارامتر: MinLat, MinLon, MaxLat, MaxLon, CellKey
// (CellKey فقط در حالت Grid مقداردهی می‌شود)
DECLARE_MULTICAST_DELEGATE_FiveParams(FOnBoundsSelected, FString, FString, FString, FString, FString);

/**
 * بریج ارتباطی بین C++ و جاوااسکریپت در وب‌براوزر
 */
UCLASS()
class HIROGEOEDITOR_API UHiroGeoJSBridge : public UObject
{
    GENERATED_BODY()

public:
    // این دیلیگیت در Slate UI بایند می‌شود
    FOnBoundsSelected OnBoundsSelected;

    /**
     * این تابع از طرف جاوااسکریپت (ExecuteJS) فراخوانی می‌شود.
     * از const FString& استفاده شده برای جلوگیری از کپی غیرضروری رشته‌ها.
     */
    UFUNCTION()
    void OnMapAreaSelected(const FString& MinLat, const FString& MinLon, const FString& MaxLat, const FString& MaxLon, const FString& CellKey);
};