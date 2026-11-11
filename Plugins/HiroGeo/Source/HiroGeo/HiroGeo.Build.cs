// Copyright Hiro. All Rights Reserved.

using UnrealBuildTool;

public class HiroGeo : ModuleRules
{
    public HiroGeo(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "ProceduralMeshComponent",
            "Json",
            "HTTP",
            "Projects",
            "JsonUtilities",
            "XmlParser",
            "HiroGeoCGAL",
            "HiroGeoLERC"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
        }
    }
}