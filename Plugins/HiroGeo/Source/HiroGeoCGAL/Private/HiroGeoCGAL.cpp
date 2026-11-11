// Copyright Hiro. All Rights Reserved.

#include "HiroGeoCGAL.h"

void FHiroGeoCGALModule::StartupModule()
{
	UE_LOG(LogTemp, Log, TEXT("HiroGeoCGAL Module Started"));
}

void FHiroGeoCGALModule::ShutdownModule()
{
	UE_LOG(LogTemp, Log, TEXT("HiroGeoCGAL Module Shut Down"));
}

IMPLEMENT_MODULE(FHiroGeoCGALModule, HiroGeoCGAL)
