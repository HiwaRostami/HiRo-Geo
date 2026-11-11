// Copyright Hiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Templates/SharedPointer.h"
#include "Styling/SlateStyle.h"

class FSlateStyleSet;

class FHiroGeoEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterToolMenus();

    TSharedPtr<FSlateStyleSet> PluginStyleSet;
};