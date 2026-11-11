// Copyright Hiro. All Rights Reserved.

#include "HiroGeoJSBridge.h"

void UHiroGeoJSBridge::OnMapAreaSelected(const FString& MinLat, const FString& MinLon, const FString& MaxLat, const FString& MaxLon, const FString& CellKey)
{
    OnBoundsSelected.Broadcast(MinLat, MinLon, MaxLat, MaxLon, CellKey);
}