// Copyright Hiro. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "OSMLoader.h"
#include "HiroGeoRustOsmAdapter.h"

DEFINE_LOG_CATEGORY_STATIC(LogHiroGeoOsmBenchmark, Log, All);

namespace
{
    void AppendRing(FString& Row, const TArray<FDoublePoint2D>& Points)
    {
        for (const FDoublePoint2D& Point : Points)
        {
            Row += FString::Printf(TEXT("%.9f,%.9f;"), Point.X, Point.Y);
        }
    }

    void AppendGeometry(FString& Dump, const TCHAR* Kind, const TArray<FDoublePoint2D>& Outer,
        const TArray<TArray<FDoublePoint2D>>& Holes)
    {
        Dump += Kind;
        Dump += TEXT("\t\t");
        AppendRing(Dump, Outer);
        for (const TArray<FDoublePoint2D>& Hole : Holes)
        {
            Dump += TEXT("|");
            AppendRing(Dump, Hole);
        }
        Dump += TEXT("\n");
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoRustOsmComparison, "HiroGeo.OSM.RustComparison",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)

bool FHiroGeoRustOsmComparison::RunTest(const FString& Parameters)
{
    FString Path, DllPath;
    if (!FParse::Value(FCommandLine::Get(), TEXT("HiroGeoOsmInput="), Path) ||
        !FParse::Value(FCommandLine::Get(), TEXT("HiroGeoRustDll="), DllPath))
    {
        AddError(TEXT("Supply -HiroGeoOsmInput=<file> and -HiroGeoRustDll=<file>."));
        return false;
    }

    TArray<uint8> Source;
    FOSMLoader Cpp;
    if (!FFileHelper::LoadFileToArray(Source, *Path) || !Cpp.LoadOSMFile(Path))
    {
        AddError(FString::Printf(TEXT("Cannot load OSM input: %s"), *Path));
        return false;
    }

    FString Error;
    const double Start = FPlatformTime::Seconds();
    const auto Rust = FHiroGeoRustOsmAdapter::LoadXml(DllPath, Source, Cpp, Error);
    const double Elapsed = (FPlatformTime::Seconds() - Start) * 1000.0;
    if (!Rust)
    {
        AddError(Error);
        return false;
    }

    auto Dump = [](const FOSMLoader& Loader)
    {
        FString Result;
        for (const FGeoBuilding& Building : Loader.GetBuildings())
            AppendGeometry(Result, Building.bIsBuildingPart ? TEXT("part") : TEXT("building"),
                Building.RawLonLat, Building.HolesRaw);
        for (const FGeoRoad& Road : Loader.GetRoads())
            AppendGeometry(Result, TEXT("road"), Road.RawLonLat, {});
        for (const FGeoWater& Water : Loader.GetWaters())
            AppendGeometry(Result, Water.bIsClosed ? TEXT("water") : TEXT("water_line"),
                Water.RawLonLat, Water.HolesRaw);
        return Result;
    };
    FString DumpBase;
    if (FParse::Value(FCommandLine::Get(), TEXT("HiroGeoOsmDump="), DumpBase) &&
        (!FFileHelper::SaveStringToFile(Dump(Cpp), *(DumpBase + TEXT("-cpp.tsv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ||
         !FFileHelper::SaveStringToFile(Dump(*Rust), *(DumpBase + TEXT("-rust.tsv")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
    {
        AddError(TEXT("Cannot save the geometry comparison dumps."));
        return false;
    }
    UE_LOG(LogHiroGeoOsmBenchmark, Display,
        TEXT("Rust adapter: buildings=%d roads=%d waters=%d parse_and_convert_ms=%.3f"),
        Rust->GetBuildings().Num(), Rust->GetRoads().Num(), Rust->GetWaters().Num(), Elapsed);
    return TestEqual(TEXT("Road count"), Rust->GetRoads().Num(), Cpp.GetRoads().Num());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoOsmBenchmark, "HiroGeo.OSM.Benchmark",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)

bool FHiroGeoOsmBenchmark::RunTest(const FString& Parameters)
{
    FString Path;
    FString DumpPath;
    FParse::Value(FCommandLine::Get(), TEXT("HiroGeoOsmDump="), DumpPath);
    int32 Runs = 30;
    FParse::Value(FCommandLine::Get(), TEXT("HiroGeoOsmRuns="), Runs);
    if (!FParse::Value(FCommandLine::Get(), TEXT("HiroGeoOsmInput="), Path)
        || Runs < 1 || Runs > 1000)
    {
        AddError(TEXT("Supply -HiroGeoOsmInput=<file> and optionally -HiroGeoOsmRuns=<1..1000>."));
        return false;
    }

    FString Source;
    if (!FFileHelper::LoadFileToString(Source, *Path))
    {
        AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
        return false;
    }

    TArray<double> Times;
    Times.Reserve(Runs);
    for (int32 Run = 0; Run <= Runs; ++Run)
    {
        FOSMLoader Loader;
        const double Start = FPlatformTime::Seconds();
        const bool bLoaded = Loader.LoadOSMFromString(Source);
        const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
        if (!bLoaded)
        {
            AddError(Loader.GetLastError());
            return false;
        }

        if (Run != 0)
        {
            Times.Add(Milliseconds);
            continue;
        }

        int32 Parts = 0;
        int32 WaterPolygons = 0;
        int32 WaterHoles = 0;
        for (const FGeoBuilding& Building : Loader.GetBuildings())
        {
            Parts += Building.bIsBuildingPart;
        }
        for (const FGeoWater& Water : Loader.GetWaters())
        {
            WaterPolygons += Water.bIsClosed;
            WaterHoles += Water.HolesRaw.Num();
        }
        UE_LOG(LogHiroGeoOsmBenchmark, Display,
            TEXT("buildings=%d parts=%d roads=%d water_polygons=%d water_lines=%d water_holes=%d"),
            Loader.GetBuildings().Num() - Parts, Parts, Loader.GetRoads().Num(),
            WaterPolygons, Loader.GetWaters().Num() - WaterPolygons, WaterHoles);
        if (!DumpPath.IsEmpty())
        {
            FString Dump;
            for (const FGeoBuilding& Building : Loader.GetBuildings())
            {
                AppendGeometry(Dump, Building.bIsBuildingPart ? TEXT("part") : TEXT("building"),
                    Building.RawLonLat, Building.HolesRaw);
            }
            for (const FGeoRoad& Road : Loader.GetRoads())
            {
                AppendGeometry(Dump, TEXT("road"), Road.RawLonLat, {});
            }
            for (const FGeoWater& Water : Loader.GetWaters())
            {
                AppendGeometry(Dump, Water.bIsClosed ? TEXT("water") : TEXT("water_line"),
                    Water.RawLonLat, Water.HolesRaw);
            }
            if (!FFileHelper::SaveStringToFile(Dump, *DumpPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            {
                AddError(FString::Printf(TEXT("Cannot write %s"), *DumpPath));
                return false;
            }
        }
    }

    Times.Sort();
    const double Median = (Times[(Runs - 1) / 2] + Times[Runs / 2]) / 2.0;
    const double P95 = Times[(Runs * 95 + 99) / 100 - 1];
    UE_LOG(LogHiroGeoOsmBenchmark, Display, TEXT("runs=%d median_ms=%.3f p95_ms=%.3f"),
        Runs, Median, P95);
    return true;
}

#endif
