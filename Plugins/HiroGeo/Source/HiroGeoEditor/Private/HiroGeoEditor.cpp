// Copyright Hiro. All Rights Reserved.

#include "HiroGeoEditor.h"
#include "SHiroGeoImporterWindow.h"
#include "Modules/ModuleManager.h"
#include "EditorStyleSet.h"
#include "SlateOptMacros.h"
#include "Styling/SlateStyleRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "Brushes/SlateImageBrush.h"
#include "ToolMenus.h"
#include "ToolMenuSection.h"
#include "ToolMenuEntry.h"
#include "ToolMenuOwner.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"

#define LOCTEXT_NAMESPACE "FHiroGeoEditorModule"

void FHiroGeoEditorModule::StartupModule()
{
    if (!IsRunningCommandlet())
    {
        PluginStyleSet = MakeShareable(new FSlateStyleSet("HiroGeoEditorStyle"));

        FString PluginBaseDir = IPluginManager::Get().FindPlugin("HiroGeo")->GetBaseDir();

        FString IconPath = FPaths::Combine(PluginBaseDir, TEXT("Resources/Icon.png"));
        FString Icon2Path = FPaths::Combine(PluginBaseDir, TEXT("Resources/Icon2.png"));
        FPaths::MakeStandardFilename(IconPath);
        FPaths::MakeStandardFilename(Icon2Path);

        const FVector2D Icon40x40(40.0f, 40.0f);
        const FVector2D Icon20x20(20.0f, 20.0f);

        if (FPaths::FileExists(IconPath))
        {
            PluginStyleSet->Set("HiroGeo.ToolbarIcon", new FSlateImageBrush(IconPath, Icon40x40));
        }

        if (FPaths::FileExists(Icon2Path))
        {
            PluginStyleSet->Set("HiroGeo.ToolbarIcon.Small", new FSlateImageBrush(Icon2Path, Icon20x20));
        }

        FSlateStyleRegistry::RegisterSlateStyle(*PluginStyleSet.Get());
    }

    // ✅ ثبت از طریق سیستم جدید UToolMenus (UE5+)
    // به جای GetToolBarExtensibilityManager که در UE5 منسوخ شده و دیگه به تولبار واقعی وصل نمی‌شه
    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FHiroGeoEditorModule::RegisterToolMenus));
}

void FHiroGeoEditorModule::RegisterToolMenus()
{
    // Owner Scoped باعث می‌شه همه‌ی entry هایی که اینجا ثبت می‌شن،
    // به نام این ماژول تحت مالکیت قرار بگیرن تا موقع Unregister به درستی پاک بشن.
    FToolMenuOwnerScoped OwnerScoped(this);

    // ⬅️ این دقیقاً همون فضای خالیِ سمت راستِ Settings در تولبار اصلی Level Editor هست
    // که پلاگین‌هایی مثل Cesium هم دکمه‌شون رو اونجا می‌ذارن.
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
    if (!Menu)
    {
        return;
    }

    FToolMenuSection& Section = Menu->FindOrAddSection("HiroGeoSection");

    const FName StyleSetName = PluginStyleSet.IsValid()
        ? PluginStyleSet->GetStyleSetName()
        : FName("HiroGeoEditorStyle");

    Section.AddEntry(FToolMenuEntry::InitToolBarButton(
        "HiroGeoImporterButton",
        FExecuteAction::CreateLambda([]()
        {
            TSharedRef<SWindow> Window = SNew(SWindow)
                .Title(LOCTEXT("HiroGeoImporterTitle", "HiroGeo Importer"))
                .ClientSize(FVector2D(820, 600))
                .SizingRule(ESizingRule::UserSized)
                .SupportsMinimize(false)
                .SupportsMaximize(false)
                [
                    SNew(SHiroGeoImporterWindow)
                ];
            FSlateApplication::Get().AddWindow(Window);
        }),
        LOCTEXT("HiroGeoImporterLabel", "HiroGeo Importer"),
        LOCTEXT("HiroGeoImporterTooltip", "Open Map Importer (Online/Local)"),
        FSlateIcon(StyleSetName, "HiroGeo.ToolbarIcon", "HiroGeo.ToolbarIcon.Small")
    ));
}

void FHiroGeoEditorModule::ShutdownModule()
{
    // ✅ لغو ثبت کال‌بک UToolMenus (و تمام entry هایی که تحت مالکیت این ماژول ثبت شده بودن)
    UToolMenus::UnRegisterStartupCallback(this);

    if (PluginStyleSet.IsValid())
    {
        FSlateStyleRegistry::UnRegisterSlateStyle(*PluginStyleSet.Get());
        PluginStyleSet.Reset();
    }
}

IMPLEMENT_MODULE(FHiroGeoEditorModule, HiroGeoEditor)

#undef LOCTEXT_NAMESPACE