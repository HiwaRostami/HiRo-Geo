// Copyright Hiro. All Rights Reserved.

using UnrealBuildTool;

public class HiroGeoEditor : ModuleRules
{
    public HiroGeoEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "HTTP",
            "Projects"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Json",
            "CoreUObject",
            "Engine",
            "Slate",
            "SlateCore",
            "UnrealEd",
            "EditorStyle",
            "LevelEditor",  
            "HiroGeo",
            "ProceduralMeshComponent",
            "WebBrowser",
            "WebBrowserWidget",
            "DesktopPlatform",
            "ToolMenus" 
        });
        
    }
}