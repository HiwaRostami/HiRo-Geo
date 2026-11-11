// Copyright Hiro. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class HiroGeoCGAL : ModuleRules
{
    public HiroGeoCGAL(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.CPlusPlus;

        // ========================================================================
        // CRITICAL: Disable PCH and Unity build to isolate CGAL from UE5 headers
        // ========================================================================
        PCHUsage = PCHUsageMode.NoPCHs;
        bUseUnity = false;

        // CGAL needs C++ exceptions
        bEnableExceptions = true;

        // CGAL uses dynamic_cast internally (e.g., in triangulation data structures)
        // UE5 disables RTTI by default (/GR-), so we must enable it for this module
        bUseRTTI = true;

        // DO NOT treat CGAL warnings as errors - CGAL generates many warnings
        // that are harmless but would break the build
        bWarningsAsErrors = false;

        // UE Core dependency (for IModuleInterface only)
        PublicDependencyModuleNames.AddRange(new string[] { "Core" });

        // ========================================================================
        // CGAL Include Path
        // PRIVATE on purpose: only CGALSkeletonImpl.cpp (in this module's
        // Private folder) includes <CGAL/...>. The public bridge header
        // (CGALSkeletonImpl.h) pulls in <vector> only. Keeping this private
        // guarantees CGAL headers never leak into dependent modules
        // (HiroGeo / HiroGeoEditor), which are compiled without RTTI,
        // without exceptions, and with Unity builds enabled.
        // ========================================================================
        string CGALIncludePath = Path.GetFullPath(
            Path.Combine(ModuleDirectory, "..", "ThirdParty", "CGAL", "include")
        );
        PrivateIncludePaths.Add(CGALIncludePath);

        // ========================================================================
        // CGAL Preprocessor Definitions
        // NOTE: Do NOT define CGAL_USE_LEDA here! Even as =0, it makes
        // #ifdef CGAL_USE_LEDA true, which causes CGAL to include LEDA headers.
        // We #undef it in CGALSkeletonImpl.cpp after including CGAL/config.h.
        // ========================================================================
        PrivateDefinitions.Add("CGAL_DISABLE_ROUNDING_MATH_CHECK=1");
        PrivateDefinitions.Add("CGAL_NO_DEPRECATED_CODE=1");
        PrivateDefinitions.Add("CGAL_NO_ASSERTIONS=1");
        PrivateDefinitions.Add("CGAL_TEST_SUITE=0");

        // Define CGAL version macros to silence version warnings
        PrivateDefinitions.Add("CGAL_COMPATIBLE_VERSION_MAJOR=5");
        PrivateDefinitions.Add("CGAL_NOT_HEADER_ONLY=0");
        PrivateDefinitions.Add("CGAL_USE_BARE_STD_SET=1");
        PrivateDefinitions.Add("CGAL_USE_BARE_STD_MAP=1");

        // Export macro - tells CGALSkeletonImpl.h we're building the DLL
        PrivateDefinitions.Add("CGALBRIDGE_IMPL=1");

        // ========================================================================
        // Library Linking - GMP and MPFR required by CGAL
        // ========================================================================
        string CGALLibPath = Path.GetFullPath(
            Path.Combine(ModuleDirectory, "..", "ThirdParty", "CGAL", "lib")
        );

        PublicAdditionalLibraries.Add(Path.Combine(CGALLibPath, "gmp.lib"));
        PublicAdditionalLibraries.Add(Path.Combine(CGALLibPath, "mpfr.lib"));
    }
}
