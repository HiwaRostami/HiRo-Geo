#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "GeoJSONLoader.h"
#include "GeoMeshGenerator.h"
#include "HiroGeoElevationFetcher.h"
#include "HiroGeoGenerationGate.h"
#include "HiroGeoActor.h"
#include "HiroGeoLercDecoder.h"
#include "OSMLoader.h"
#include "Async/ParallelFor.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Tests/AutomationEditorCommon.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoGeoJsonImportTest, "HiroGeo.GeoJSON.Import", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiroGeoGeoJsonImportTest::RunTest(const FString& Parameters)
{
    FGeoJSONLoader Loader;
    const FString Building = TEXT(R"({"type":"Feature","properties":{"building":"yes","height":12.5},"geometry":{"type":"Polygon","coordinates":[[[0,0],[0.001,0],[0.001,0.001],[0,0.001],[0,0]],[[0.0002,0.0002],[0.0004,0.0002],[0.0004,0.0004],[0.0002,0.0002]]]}})");
    TestTrue(TEXT("Building loads"), Loader.LoadGeoJSONFromString(Building));
    TestEqual(TEXT("One building"), Loader.GetBuildings().Num(), 1);
    if (Loader.GetBuildings().Num() == 1)
    {
        TestEqual(TEXT("Courtyard retained"), Loader.GetBuildings()[0].Holes.Num(), 1);
        TestTrue(TEXT("Numeric height retained"), FMath::IsNearlyEqual(Loader.GetBuildings()[0].Height, 12.5));
    }
    TestFalse(TEXT("Empty collection fails"), Loader.LoadGeoJSONFromString(TEXT(R"({"type":"FeatureCollection","features":[]})")));
    TestFalse(TEXT("Failed load resets state"), Loader.IsLoaded());
    const FString Lines = TEXT(R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"route":"bus"},"geometry":{"type":"LineString","coordinates":[[0,0],[0.001,0]]}},{"type":"Feature","properties":{"highway":"residential"},"geometry":{"type":"LineString","coordinates":[[0,0],[0.001,0]]}}]})");
    TestTrue(TEXT("Lines load"), Loader.LoadGeoJSONFromString(Lines));
    TestEqual(TEXT("Only highway is a road"), Loader.GetRoads().Num(), 1);
    TestFalse(TEXT("Out-of-range coordinates fail"), Loader.LoadGeoJSONFromString(
        TEXT(R"({"type":"Feature","properties":{"highway":"residential"},"geometry":{"type":"LineString","coordinates":[[181,0],[0,0]]}})")));
    TestFalse(TEXT("Unclassified polygon is not invented as building"), Loader.LoadGeoJSONFromString(
        TEXT(R"({"type":"Feature","properties":{"landuse":"residential"},"geometry":{"type":"Polygon","coordinates":[[[0,0],[1,0],[0,1],[0,0]]]}})")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoGeoJsonHoleMeshTest, "HiroGeo.GeoJSON.HoleMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiroGeoGeoJsonHoleMeshTest::RunTest(const FString& Parameters)
{
    FGeoJSONLoader Loader;
    const FString Water = TEXT(R"({"type":"Feature","properties":{"natural":"water"},"geometry":{"type":"Polygon","coordinates":[[[0,0],[0.01,0],[0.01,0.01],[0,0.01],[0,0]],[[0.004,0.004],[0.006,0.004],[0.006,0.006],[0.004,0.006],[0.004,0.004]]]}})");
    TestTrue(TEXT("Water polygon loads"), Loader.LoadGeoJSONFromString(Water));
    TestEqual(TEXT("Water hole retained"), Loader.GetWaters().Num() == 1 ? Loader.GetWaters()[0].Holes.Num() : 0, 1);
    if (Loader.GetWaters().Num() != 1 || Loader.GetWaters()[0].Holes.Num() != 1) return false;

    FGeoMeshGenerator Generator;
    Generator.ScaleFactor = 100.f;
    FGeoMeshGenerator::FMeshData Mesh;
    Generator.GenerateWaterMeshes(Loader.GetWaters(), Mesh);
    TestTrue(TEXT("Hole-aware water mesh generated"), Mesh.Triangles.Num() >= 3);

    TArray<FVector2D> Hole;
    for (const FVector2D& P : Loader.GetWaters()[0].Holes[0])
        Hole.Add(FVector2D(P.X * 100.f, -P.Y * 100.f));
    auto InHole = [&Hole](const FVector2D& P)
    {
        bool bInside = false;
        for (int32 i = 0, j = Hole.Num() - 1; i < Hole.Num(); j = i++)
            if (((Hole[i].Y > P.Y) != (Hole[j].Y > P.Y)) &&
                P.X < (Hole[j].X - Hole[i].X) * (P.Y - Hole[i].Y) / (Hole[j].Y - Hole[i].Y + KINDA_SMALL_NUMBER) + Hole[i].X)
                bInside = !bInside;
        return bInside;
    };
    for (int32 i = 0; i + 2 < Mesh.Triangles.Num(); i += 3)
    {
        const FVector C = (Mesh.Vertices[Mesh.Triangles[i]] + Mesh.Vertices[Mesh.Triangles[i + 1]] +
            Mesh.Vertices[Mesh.Triangles[i + 2]]) / 3.f;
        TestFalse(TEXT("No water triangle centroid fills courtyard"), InHole(FVector2D(C.X, C.Y)));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoOsmValidationTest, "HiroGeo.OSM.ValidationAndHoles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiroGeoOsmValidationTest::RunTest(const FString& Parameters)
{
    FOSMLoader Loader;
    TestFalse(TEXT("Wrong XML root fails"), Loader.LoadOSMFromString(TEXT("<html/>")));
    TestFalse(TEXT("Failed load state remains false"), Loader.IsLoaded());

    const FString MissingRef = TEXT(R"(<osm version="0.6"><node id="1" lat="0" lon="0"/><way id="2"><nd ref="1"/><nd ref="99"/><tag k="highway" v="residential"/></way></osm>)");
    TestFalse(TEXT("Way with missing node is rejected"), Loader.LoadOSMFromString(MissingRef));
    TestEqual(TEXT("Rejected way leaves no road"), Loader.GetRoads().Num(), 0);

    const FString WaterRelation = TEXT(R"(<osm version="0.6">
<node id="1" lat="0" lon="0"/><node id="2" lat="0" lon="0.01"/><node id="3" lat="0.01" lon="0.01"/><node id="4" lat="0.01" lon="0"/>
<node id="5" lat="0.004" lon="0.004"/><node id="6" lat="0.004" lon="0.006"/><node id="7" lat="0.006" lon="0.006"/><node id="8" lat="0.006" lon="0.004"/>
<way id="10"><nd ref="1"/><nd ref="2"/><nd ref="3"/><nd ref="4"/><nd ref="1"/></way>
<way id="11"><nd ref="5"/><nd ref="6"/><nd ref="7"/><nd ref="8"/><nd ref="5"/></way>
<relation id="20"><member type="way" ref="10" role="outer"/><member type="way" ref="11" role="inner"/><tag k="type" v="multipolygon"/><tag k="natural" v="water"/></relation>
</osm>)");
    TestTrue(TEXT("Water multipolygon loads"), Loader.LoadOSMFromString(WaterRelation));
    TestEqual(TEXT("One water relation"), Loader.GetWaters().Num(), 1);
    TestEqual(TEXT("OSM water inner ring retained"), Loader.GetWaters().Num() == 1 ? Loader.GetWaters()[0].Holes.Num() : 0, 1);

    const FString ParkRelation = WaterRelation.Replace(
        TEXT("<tag k=\"natural\" v=\"water\"/>"), TEXT("<tag k=\"leisure\" v=\"park\"/>"));
    TestTrue(TEXT("Green multipolygon loads"), Loader.LoadOSMFromString(ParkRelation));
    TestEqual(TEXT("One green relation"), Loader.GetVegetations().Num(), 1);
    TestEqual(TEXT("OSM green inner ring retained"), Loader.GetVegetations().Num() == 1 ? Loader.GetVegetations()[0].Holes.Num() : 0, 1);
    TestFalse(TEXT("Missing OSM file fails"), Loader.LoadOSMFile(TEXT("Z:/HiroGeo/does-not-exist.osm")));
    TestFalse(TEXT("Missing file clears loaded state"), Loader.IsLoaded());
    TestEqual(TEXT("Missing file clears old data"), Loader.GetVegetations().Num(), 0);

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HiroGeo"));
    if (!TestTrue(TEXT("HiroGeo plugin is discoverable for fixture test"), Plugin.IsValid())) return false;
    const FString RealFixture = Plugin->GetBaseDir() / TEXT("Tests/Fixtures/overpass-real-20261105.osm");
    TestTrue(TEXT("Saved real Overpass fixture loads"), Loader.LoadOSMFile(RealFixture));
    TestTrue(TEXT("Saved real Overpass fixture contains roads"), Loader.GetRoads().Num() > 0);
    TestTrue(TEXT("Saved real Overpass fixture contains buildings"), Loader.GetBuildings().Num() > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoElevationCompletionOrderTest,
    "HiroGeo.Elevation.SyncAsyncCompletionOrder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiroGeoElevationCompletionOrderTest::RunTest(const FString& Parameters)
{
    auto RunGeneration = [](bool bCompleteInline, TArray<FString>& OutOrder)
    {
        TSharedRef<FHiroGeoGenerationGate> Gate = MakeShared<FHiroGeoGenerationGate>();
        TFunction<void()> ApplyDem = [&OutOrder]() { OutOrder.Add(TEXT("DEM")); };

        if (bCompleteInline)
        {
            // Full-cache FetchForBounds completion occurs here, before the rest
            // of Generate(). The production gate must retain it.
            Gate->Submit(MoveTemp(ApplyDem));
        }

        OutOrder.Add(TEXT("Roofs"));
        OutOrder.Add(TEXT("Vegetation"));
        OutOrder.Add(TEXT("Instances"));
        Gate->Open();

        if (!bCompleteInline)
        {
            // HTTP completion occurs after Generate() has returned.
            Gate->Submit(MoveTemp(ApplyDem));
        }
    };

    TArray<FString> CachedOrder;
    TArray<FString> AsyncOrder;
    RunGeneration(true, CachedOrder);
    RunGeneration(false, AsyncOrder);
    TestTrue(TEXT("Full-cache and async completion produce identical ordering"), CachedOrder == AsyncOrder);
    TestEqual(TEXT("DEM is the final writer"), CachedOrder.Last(), FString(TEXT("DEM")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoActorElevationIntegrationTest,
    "HiroGeo.Elevation.RealActorSyncAsyncEquivalent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiroGeoActorElevationIntegrationTest::RunTest(const FString& Parameters)
{
    struct FMeshSnapshot
    {
        TArray<TArray<FVector>> Positions;
        TArray<TArray<uint32>> Indices;
    };
    struct FActorSnapshot
    {
        FMeshSnapshot Ground;
        FMeshSnapshot Roof;
        FMeshSnapshot Railway;
        FMeshSnapshot Vegetation;
        TArray<FTransform> Trees;
        TArray<FTransform> Grass;
    };

    auto CaptureMesh = [](UProceduralMeshComponent* Component)
    {
        FMeshSnapshot Result;
        const int32 NumSections = Component ? Component->GetNumSections() : 0;
        Result.Positions.SetNum(NumSections);
        Result.Indices.SetNum(NumSections);
        for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
        {
            const FProcMeshSection* Section = Component->GetProcMeshSection(SectionIndex);
            if (!Section) continue;
            for (const FProcMeshVertex& Vertex : Section->ProcVertexBuffer)
            {
                Result.Positions[SectionIndex].Add(Vertex.Position);
            }
            Result.Indices[SectionIndex] = Section->ProcIndexBuffer;
        }
        return Result;
    };
    auto CaptureInstances = [](UInstancedStaticMeshComponent* Component)
    {
        TArray<FTransform> Result;
        if (!Component) return Result;
        for (int32 Index = 0; Index < Component->GetInstanceCount(); ++Index)
        {
            FTransform Transform;
            Component->GetInstanceTransform(Index, Transform, false);
            Result.Add(Transform);
        }
        return Result;
    };
    auto CaptureActor = [&](AHiroGeoActor* Actor)
    {
        FActorSnapshot Result;
        Result.Ground = CaptureMesh(Actor->GroundMeshComp);
        Result.Roof = CaptureMesh(Actor->RoofMeshComp);
        Result.Railway = CaptureMesh(Actor->RailwayMeshComp);
        Result.Vegetation = CaptureMesh(Actor->VegetationMeshComp);
        Result.Trees = CaptureInstances(Actor->TreeInstancedComp);
        Result.Grass = CaptureInstances(Actor->GrassInstancedComp);
        return Result;
    };
    auto CompareMesh = [&](const TCHAR* Label, const FMeshSnapshot& A, const FMeshSnapshot& B)
    {
        bool bEqual = A.Positions.Num() == B.Positions.Num() && A.Indices.Num() == B.Indices.Num();
        if (bEqual)
        {
            for (int32 Section = 0; Section < A.Positions.Num() && bEqual; ++Section)
            {
                bEqual = A.Positions[Section].Num() == B.Positions[Section].Num()
                    && A.Indices[Section] == B.Indices[Section];
                for (int32 Vertex = 0; Vertex < A.Positions[Section].Num() && bEqual; ++Vertex)
                {
                    bEqual = A.Positions[Section][Vertex].Equals(B.Positions[Section][Vertex], 0.01);
                }
            }
        }
        TestTrue(Label, bEqual);
        return bEqual;
    };
    auto CompareInstances = [&](const TCHAR* Label, const TArray<FTransform>& A, const TArray<FTransform>& B)
    {
        bool bEqual = A.Num() == B.Num();
        for (int32 Index = 0; Index < A.Num() && bEqual; ++Index)
        {
            bEqual = A[Index].Equals(B[Index], 0.01);
        }
        TestTrue(Label, bEqual);
    };

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HiroGeo"));
    if (!TestTrue(TEXT("HiroGeo plugin is discoverable"), Plugin.IsValid())) return false;
    const FString FixturePath = Plugin->GetBaseDir() / TEXT("Tests/Fixtures/minimal-city.osm");
    if (!TestTrue(TEXT("Controlled OSM fixture exists"), FPaths::FileExists(FixturePath))) return false;

    constexpr int32 Zoom = 15;
    constexpr double CenterLat = 35.6995;
    constexpr double CenterLon = 51.3995;
    const double WorldPixels = 256.0 * FMath::Pow(2.0, Zoom);
    const double CenterPixelX = (CenterLon + 180.0) / 360.0 * WorldPixels;
    const double LatRad = FMath::DegreesToRadians(CenterLat);
    const double CenterPixelY = (1.0 - FMath::Loge(FMath::Tan(LatRad) + 1.0 / FMath::Cos(LatRad)) / UE_PI)
        * 0.5 * WorldPixels;
    TSharedPtr<FHiroGeoHeightmap> Heightmap = MakeShared<FHiroGeoHeightmap>();
    Heightmap->Width = 512;
    Heightmap->Height = 512;
    Heightmap->Zoom = Zoom;
    Heightmap->OriginPixelX = FMath::FloorToDouble(CenterPixelX) - 256.0;
    Heightmap->OriginPixelY = FMath::FloorToDouble(CenterPixelY) - 256.0;
    Heightmap->Heights.SetNumUninitialized(Heightmap->Width * Heightmap->Height);
    for (int32 Y = 0; Y < Heightmap->Height; ++Y)
    {
        for (int32 X = 0; X < Heightmap->Width; ++X)
        {
            Heightmap->Heights[Y * Heightmap->Width + X] = 100.0f + X * 0.02f + Y * 0.03f;
        }
    }

    UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
    if (!TestNotNull(TEXT("Editor automation world created"), World)) return false;
    auto SpawnConfiguredActor = [&](bool bInline)
    {
        AHiroGeoActor* Actor = World->SpawnActor<AHiroGeoActor>();
        Actor->bUseGeoJSON = false;
        Actor->OSMFilePath = FixturePath;
        Actor->bUseTerrainElevation = true;
        Actor->bFetchOnlineElevation = true;
        Actor->bShowRailways = true;
        Actor->TerrainGridSubdivisions = 12;
        Actor->SetAutomationElevation(Heightmap, bInline);
        FMath::RandInit(0x4849524f);
        Actor->Generate();
        return Actor;
    };

    AHiroGeoActor* InlineActor = SpawnConfiguredActor(true);
    const FActorSnapshot Inline = CaptureActor(InlineActor);
    AHiroGeoActor* DeferredActor = SpawnConfiguredActor(false);
    TestTrue(TEXT("Deferred elevation completes after Generate returns"),
        DeferredActor->CompleteAutomationElevation());
    const FActorSnapshot Deferred = CaptureActor(DeferredActor);

    auto HasVertices = [](const FMeshSnapshot& Mesh)
    {
        for (const TArray<FVector>& Section : Mesh.Positions)
        {
            if (Section.Num() > 0) return true;
        }
        return false;
    };
    TestTrue(TEXT("Ground fixture generated vertices"), HasVertices(Inline.Ground));
    TestTrue(TEXT("Roof fixture generated vertices"), HasVertices(Inline.Roof));
    TestTrue(TEXT("Railway fixture generated vertices"), HasVertices(Inline.Railway));
    TestTrue(TEXT("Vegetation fixture generated vertices"), HasVertices(Inline.Vegetation));
    TestTrue(TEXT("Vegetation fixture generated instances"), Inline.Trees.Num() + Inline.Grass.Num() > 0);

    CompareMesh(TEXT("Ground geometry matches within 0.01 UE units"), Inline.Ground, Deferred.Ground);
    CompareMesh(TEXT("Roof geometry matches within 0.01 UE units"), Inline.Roof, Deferred.Roof);
    CompareMesh(TEXT("Railway geometry matches within 0.01 UE units"), Inline.Railway, Deferred.Railway);
    CompareMesh(TEXT("Vegetation geometry matches within 0.01 UE units"), Inline.Vegetation, Deferred.Vegetation);
    CompareInstances(TEXT("Tree instance transforms match within 0.01 UE units"), Inline.Trees, Deferred.Trees);
    CompareInstances(TEXT("Grass instance transforms match within 0.01 UE units"), Inline.Grass, Deferred.Grass);

    InlineActor->Destroy();
    DeferredActor->Destroy();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoElevationNoDataTest,
    "HiroGeo.Elevation.NoDataInteriorAndBorder",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiroGeoElevationNoDataTest::RunTest(const FString& Parameters)
{
    TSharedRef<FHiroGeoElevationFetcher> Fetcher = MakeShared<FHiroGeoElevationFetcher>();
    Fetcher->TileXMin = 10;
    Fetcher->TileYMin = 20;
    Fetcher->Heightmap = MakeShared<FHiroGeoHeightmap>();
    Fetcher->Heightmap->Width = 256;
    Fetcher->Heightmap->Height = 256;
    Fetcher->Heightmap->Heights.SetNumZeroed(256 * 256);

    constexpr int32 TileSize = 257;
    TArray<float> Heights;
    Heights.Init(123.0f, TileSize * TileSize);
    TArray<uint8> Mask;
    Mask.Init(1, TileSize * TileSize);

    Mask[0] = 0; // unused leading-border corner
    TestTrue(TEXT("No-data confined to unused border is accepted"),
        Fetcher->ApplyDecodedTile(10, 20, Heights, Mask, TileSize, TileSize));

    Mask[0] = 1;
    Mask[TileSize + 1] = 0; // first consumed sample
    TestFalse(TEXT("No-data in usable tile interior rejects the tile"),
        Fetcher->ApplyDecodedTile(10, 20, Heights, Mask, TileSize, TileSize));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoElevationFailureCancelTest,
    "HiroGeo.Elevation.PartialFailureAndCancel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiroGeoElevationFailureCancelTest::RunTest(const FString& Parameters)
{
    bool bPartialCalled = false;
    bool bPartialSucceeded = true;
    TSharedRef<FHiroGeoElevationFetcher> Partial = MakeShared<FHiroGeoElevationFetcher>();
    Partial->Heightmap = MakeShared<FHiroGeoHeightmap>();
    Partial->Heightmap->Width = 1;
    Partial->Heightmap->Height = 1;
    Partial->Heightmap->Heights.Add(10.0f);
    Partial->NumTilesX = 2;
    Partial->NumTilesY = 1;
    Partial->SuccessfulTiles = 1;
    Partial->bAnyTileFailed = true;
    Partial->CompletionDelegate = FHiroGeoElevationFetcher::FOnComplete::CreateLambda(
        [&bPartialCalled, &bPartialSucceeded](bool bSuccess, TSharedPtr<FHiroGeoHeightmap> Result)
        {
            bPartialCalled = true;
            bPartialSucceeded = bSuccess || Result.IsValid();
        });
    Partial->Finish(Partial->SuccessfulTiles > 0 && !Partial->bAnyTileFailed);
    TestTrue(TEXT("Partial coverage invokes completion"), bPartialCalled);
    TestFalse(TEXT("Partial coverage is rejected instead of zero-filling"), bPartialSucceeded);

    bool bCancelledCalled = false;
    TSharedRef<FHiroGeoElevationFetcher> Cancelled = MakeShared<FHiroGeoElevationFetcher>();
    Cancelled->CompletionDelegate = FHiroGeoElevationFetcher::FOnComplete::CreateLambda(
        [&bCancelledCalled](bool, TSharedPtr<FHiroGeoHeightmap>) { bCancelledCalled = true; });
    Cancelled->Cancel();
    Cancelled->Finish(true);
    TestTrue(TEXT("Cancel marks fetch finished"), Cancelled->IsFinished());
    TestFalse(TEXT("Cancel suppresses completion"), bCancelledCalled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoElevationCacheSafetyTest,
    "HiroGeo.Elevation.CacheSafeSave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiroGeoElevationCacheSafetyTest::RunTest(const FString& Parameters)
{
    const int32 Zoom = 29;
    const int64 TileX = 987654321;
    const int64 TileY = 123456789;
    const FString FinalPath = FHiroGeoElevationFetcher::MakeCacheFilePath(Zoom, TileX, TileY);
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*FHiroGeoElevationFetcher::GetCacheDirectory());
    PF.DeleteFile(*FinalPath);

    TArray<uint8> Original;
    Original.Init(0x11, 64);
    Original[0] = 'L'; Original[1] = 'e'; Original[2] = 'r'; Original[3] = 'c';
    TestTrue(TEXT("Initial cache save succeeds"),
        FHiroGeoElevationFetcher::SaveCachedTile(Zoom, TileX, TileY, Original));

    const FString MissingTemp = FinalPath + TEXT(".missing.tmp");
    TestFalse(TEXT("Failed replacement reports failure"),
        FHiroGeoElevationFetcher::CommitCacheTempFile(MissingTemp, FinalPath));
    TArray<uint8> AfterFailure;
    TestTrue(TEXT("Valid destination survives failed replacement"),
        FFileHelper::LoadFileToArray(AfterFailure, *FinalPath));
    TestTrue(TEXT("Failed replacement preserves exact old bytes"), AfterFailure == Original);

    TArray<TArray<uint8>> WriterPayloads;
    for (int32 Writer = 0; Writer < 12; ++Writer)
    {
        TArray<uint8>& Payload = WriterPayloads.AddDefaulted_GetRef();
        Payload.Init(static_cast<uint8>(Writer + 1), 64);
        Payload[0] = 'L'; Payload[1] = 'e'; Payload[2] = 'r'; Payload[3] = 'c';
    }
    TArray<int32> WriterResults;
    WriterResults.SetNumZeroed(WriterPayloads.Num());
    ParallelFor(WriterPayloads.Num(), [&](int32 Writer)
    {
        WriterResults[Writer] =
            FHiroGeoElevationFetcher::SaveCachedTile(Zoom, TileX, TileY, WriterPayloads[Writer]) ? 1 : -1;
    });
    int32 SuccessfulWrites = 0;
    int32 FailedWrites = 0;
    for (const int32 Result : WriterResults)
    {
        SuccessfulWrites += Result == 1 ? 1 : 0;
        FailedWrites += Result == -1 ? 1 : 0;
    }
    AddInfo(FString::Printf(TEXT("Concurrent cache writers: successful=%d failed=%d"),
        SuccessfulWrites, FailedWrites));
    TestEqual(TEXT("Every concurrent writer reports success or failure"),
        SuccessfulWrites + FailedWrites, WriterPayloads.Num());
    TestTrue(TEXT("At least one concurrent cache writer commits"), SuccessfulWrites > 0);

    TArray<uint8> ConcurrentResult;
    TestTrue(TEXT("Concurrent writer result loads"),
        FHiroGeoElevationFetcher::TryLoadCachedTile(Zoom, TileX, TileY, ConcurrentResult));
    bool bMatchesCompleteWriter = false;
    for (const TArray<uint8>& Payload : WriterPayloads)
    {
        bMatchesCompleteWriter |= (ConcurrentResult == Payload);
    }
    TestTrue(TEXT("Concurrent result is one complete writer payload"), bMatchesCompleteWriter);

    PF.DeleteFile(*FinalPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHiroGeoSavedLercFixtureTest,
    "HiroGeo.Elevation.SavedRealLercFixture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHiroGeoSavedLercFixtureTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HiroGeo"));
    if (!TestTrue(TEXT("HiroGeo plugin is discoverable"), Plugin.IsValid())) return false;
    const FString FixturePath = Plugin->GetBaseDir() / TEXT("Tests/Fixtures/sample.lerc");
    TArray<uint8> Bytes;
    if (!TestTrue(TEXT("Plugin LERC fixture loads"),
        FFileHelper::LoadFileToArray(Bytes, *FixturePath))) return false;
    TArray<float> Heights;
    TArray<uint8> ValidMask;
    int32 Width = 0;
    int32 Height = 0;
    TestTrue(TEXT("Plugin LERC fixture decodes"),
        FHiroGeoLercDecoder::DecodeSingleBandFloat(
            Bytes.GetData(), Bytes.Num(), Heights, Width, Height, &ValidMask));
    TestTrue(TEXT("Plugin LERC fixture has usable tile dimensions"), Width >= 257 && Height >= 257);
    TestEqual(TEXT("Plugin LERC fixture height count"), Heights.Num(), Width * Height);
    return true;
}
#endif
