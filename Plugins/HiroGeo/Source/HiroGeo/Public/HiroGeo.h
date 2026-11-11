// Copyright Hiro. All Rights Reserved.


#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Main module class for the HiroGeo plugin.
 * Handles startup and shutdown of the runtime module.
 * The HIROGEO_API macro is automatically defined by the UE build system
 * based on the module name in HiroGeo.Build.cs.
 */
class FHiroGeoModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
