// Copyright Hiro. All Rights Reserved.

#include "HiroGeo.h"

void FHiroGeoModule::StartupModule()
{
    UE_LOG(LogTemp, Log, TEXT("HiroGeo Module Started"));
}

void FHiroGeoModule::ShutdownModule()
{
    UE_LOG(LogTemp, Log, TEXT("HiroGeo Module Shut Down"));
}

IMPLEMENT_MODULE(FHiroGeoModule, HiroGeo)
