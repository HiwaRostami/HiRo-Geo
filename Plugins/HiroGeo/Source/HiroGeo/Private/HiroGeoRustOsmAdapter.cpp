#include "HiroGeoRustOsmAdapter.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "OSMLoader.h"
#include "Templates/UnrealTemplate.h"
#include "../../HiRoGeoRust/include/hirogeo_osm.h"

namespace
{
    FString Decode(HiroOsmBytes Bytes)
    {
        const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.ptr), static_cast<int32>(Bytes.len));
        return FString(Text.Length(), Text.Get());
    }

    bool CopyRing(const HiroOsmRing& Ring, bool bClosed, TArray<FDoublePoint2D>& Points)
    {
        if (!Ring.points || Ring.count > MAX_int32 || Ring.count < (bClosed ? 4u : 2u))
        {
            return false;
        }
        const int32 Count = static_cast<int32>(Ring.count) - (bClosed ? 1 : 0);
        Points.Reserve(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Points.Emplace(Ring.points[Index].lon, Ring.points[Index].lat);
        }
        return true;
    }
}

TUniquePtr<FOSMLoader> FHiroGeoRustOsmAdapter::LoadXml(const FString& DllPath, const TArray<uint8>& Source,
    const FOSMLoader& ReferenceBounds, FString& Error)
{
    void* Dll = FPlatformProcess::GetDllHandle(*FPaths::ConvertRelativePathToFull(DllPath));
    if (!Dll)
    {
        Error = TEXT("Cannot load the experimental Rust OSM library.");
        return nullptr;
    }
    ON_SCOPE_EXIT { FPlatformProcess::FreeDllHandle(Dll); };

#define HIRO_BIND(Name) auto Api##Name = reinterpret_cast<decltype(&::Name)>(FPlatformProcess::GetDllExport(Dll, TEXT(#Name)))
    HIRO_BIND(hiro_osm_abi_version);
    HIRO_BIND(hiro_osm_parse_xml);
    HIRO_BIND(hiro_osm_release);
    HIRO_BIND(hiro_osm_counts);
    HIRO_BIND(hiro_osm_feature);
    HIRO_BIND(hiro_osm_unreal_includes);
    HIRO_BIND(hiro_osm_ring);
    HIRO_BIND(hiro_osm_tag);
#undef HIRO_BIND
    if (!Apihiro_osm_abi_version || !Apihiro_osm_parse_xml || !Apihiro_osm_release || !Apihiro_osm_counts ||
        !Apihiro_osm_feature || !Apihiro_osm_unreal_includes || !Apihiro_osm_ring || !Apihiro_osm_tag ||
        Apihiro_osm_abi_version() != 1)
    {
        Error = TEXT("The experimental Rust OSM library is missing ABI v1 exports.");
        return nullptr;
    }

    HiroOsmResult* Result = nullptr;
    ON_SCOPE_EXIT { Apihiro_osm_release(&Result); };
    uint64_t FeatureCount = 0, Skipped = 0;
    if (Apihiro_osm_parse_xml(Source.GetData(), Source.Num(), &Result) != HIRO_OSM_OK ||
        Apihiro_osm_counts(Result, &FeatureCount, &Skipped) != HIRO_OSM_OK)
    {
        Error = TEXT("The experimental Rust OSM parser could not read the XML.");
        return nullptr;
    }

    auto Loader = MakeUnique<FOSMLoader>();
    Loader->MinLat = ReferenceBounds.GetMinLat();
    Loader->MaxLat = ReferenceBounds.GetMaxLat();
    Loader->MinLon = ReferenceBounds.GetMinLon();
    Loader->MaxLon = ReferenceBounds.GetMaxLon();
    Loader->CalculateCenterPoint();

    Error = TEXT("The experimental Rust OSM result contains invalid feature data.");
    for (uint64_t Index = 0; Index < FeatureCount; ++Index)
    {
        uint32_t Included = 0;
        if (Apihiro_osm_unreal_includes(Result, Index, &Included) != HIRO_OSM_OK) return nullptr;
        if (!Included) continue;
        HiroOsmFeature Feature{};
        if (Apihiro_osm_feature(Result, Index, &Feature) != HIRO_OSM_OK) return nullptr;

        TMap<FString, FString> Tags;
        for (uint64_t TagIndex = 0; ; ++TagIndex)
        {
            HiroOsmBytes Key{}, Value{};
            const uint32_t Status = Apihiro_osm_tag(Result, Feature.source, Feature.id, TagIndex, &Key, &Value);
            if (Status == HIRO_OSM_OUT_OF_RANGE) break;
            if (Status != HIRO_OSM_OK || Key.len > MAX_int32 || Value.len > MAX_int32) return nullptr;
            Tags.Add(Decode(Key), Decode(Value));
        }

        TMap<uint32, TArray<FDoublePoint2D>> Outers;
        TMap<uint32, TArray<TArray<FDoublePoint2D>>> Holes;
        const bool bClosed = Feature.kind != HIRO_OSM_ROAD;
        for (uint64_t RingIndex = 0; RingIndex < Feature.ring_count; ++RingIndex)
        {
            HiroOsmRing Ring{};
            if (Apihiro_osm_ring(Result, Index, RingIndex, &Ring) != HIRO_OSM_OK) return nullptr;
            auto& Points = Ring.inner ? Holes.FindOrAdd(Ring.polygon).AddDefaulted_GetRef()
                : Outers.FindOrAdd(Ring.polygon);
            if (!CopyRing(Ring, bClosed, Points)) return nullptr;
        }
        for (auto& Polygon : Outers)
        {
            if (Feature.kind == HIRO_OSM_ROAD)
            {
                auto& Road = Loader->Roads.AddDefaulted_GetRef();
                Road.RawLonLat = MoveTemp(Polygon.Value);
                Loader->ApplyRoadTags(Road, Tags);
            }
            else if (Feature.kind == HIRO_OSM_WATER)
            {
                auto& Water = Loader->Waters.AddDefaulted_GetRef();
                Water.RawLonLat = MoveTemp(Polygon.Value);
                Water.HolesRaw = MoveTemp(Holes.FindOrAdd(Polygon.Key));
                Water.Name = Tags.FindRef(TEXT("name"));
            }
            else
            {
                auto& Building = Loader->Buildings.AddDefaulted_GetRef();
                Building.RawLonLat = MoveTemp(Polygon.Value);
                float CenterX = 0, CenterY = 0;
                for (const FDoublePoint2D& Point : Building.RawLonLat)
                {
                    Building.Nodes.Emplace(static_cast<float>(Point.X), static_cast<float>(Point.Y));
                    CenterX += static_cast<float>(Point.X);
                    CenterY += static_cast<float>(Point.Y);
                }
                Building.Center = FVector2D(CenterX / Building.Nodes.Num(), CenterY / Building.Nodes.Num());
                Building.HolesRaw = MoveTemp(Holes.FindOrAdd(Polygon.Key));
                Building.bIsBuildingPart = Feature.kind == HIRO_OSM_BUILDING_PART;
                Building.bIsTowerPart = Tags.FindRef(TEXT("building:part")).Equals(TEXT("tower"), ESearchCase::IgnoreCase);
                if (Building.bIsBuildingPart)
                    Building.BuildingPartId = FString::Printf(TEXT("%lld"), static_cast<long long>(Feature.id));
                Loader->ApplyBuildingTags(Building, Tags);
            }
        }
    }
    Loader->AssembleBuildingParts();
    Loader->ConvertAllCoordsToLocal();
    Loader->bIsLoaded = true;
    Error.Reset();
    return Loader;
}

#endif
