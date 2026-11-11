// Copyright Hiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GeoJSONLoader.h"
#include "Containers/Map.h"

// ============================================================================
// EXmlFileStyle compatibility
// In UE5, EXmlFileStyle is defined in XmlParser.h. However, if the
// XmlParser module is not listed as a dependency in the project's Build.cs,
// the enum will not be visible. We provide a fallback definition here.
// If XmlParser is properly linked, its definition takes precedence.
// ============================================================================
#ifndef EXMLFILESTYLE_ALREADY_DEFINED
#define EXMLFILESTYLE_ALREADY_DEFINED
#endif

class HIROGEO_API FOSMLoader
{
public:
        FOSMLoader();
        ~FOSMLoader();

        bool LoadOSMFile(const FString& FilePath);
        bool LoadOSMFromString(const FString& OSMContent);

        const TArray<FGeoBuilding>& GetBuildings() const { return Buildings; }
        const TArray<FGeoRoad>& GetRoads() const { return Roads; }
        const TArray<FGeoWater>& GetWaters() const { return Waters; }
        const TArray<FGeoVegetation>& GetVegetations() const { return Vegetations; }
        const TArray<FGeoPOI>& GetPOIs() const { return POIs; }
        const TArray<FGeoTree>& GetTrees() const { return Trees; }
        const TArray<FGeoRailway>& GetRailways() const { return Railways; }
        const TArray<FGeoFence>& GetFences() const { return Fences; }
        const TArray<FGeoPowerLine>& GetPowerLines() const { return PowerLines; }
        const TArray<FGeoModelInstance>& GetModelInstances() const { return ModelInstances; }
        const TArray<FElevationSample>& GetElevationSamples() const { return ElevationSamples; }
        bool IsLoaded() const { return bIsLoaded; }
        const FString& GetLastError() const { return LastError; }

        double GetMinLat() const { return MinLat; }
        double GetMaxLat() const { return MaxLat; }
        double GetMinLon() const { return MinLon; }
        double GetMaxLon() const { return MaxLon; }
        double GetCenterLatitude() const { return CenterLatitude; }
        double GetCenterLongitude() const { return CenterLongitude; }

private:
        // --- Parsing phases ---
        void Reset();
        void ParseNodes(const class FXmlNode* RootNode);
        void ParseWays(const class FXmlNode* RootNode);
        void ParseRelations(const class FXmlNode* RootNode);
        void AssembleBuildingParts();
        void RemoveRedundantWaterwayCenterlines();
        void CalculateCenterPoint();
        void ConvertAllCoordsToLocal();

        // --- Feature processors ---
        void ProcessBuildingWay(const class FXmlNode* WayNode);
        void ProcessRoadWay(const class FXmlNode* WayNode);
        void ProcessRailwayWay(const class FXmlNode* WayNode);
        void ProcessWaterWay(const class FXmlNode* WayNode);
        void ProcessVegetationWay(const class FXmlNode* WayNode);
        void ProcessFenceWay(const class FXmlNode* WayNode);
        void ProcessPowerLineWay(const class FXmlNode* WayNode);
        void ProcessTreeRowWay(const class FXmlNode* WayNode);
        void ProcessAerowayWay(const class FXmlNode* WayNode);

        // --- Tag appliers ---
        void ApplyBuildingTags(FGeoBuilding& Building, const TMap<FString, FString>& Tags);
        void ApplyRoadTags(FGeoRoad& Road, const TMap<FString, FString>& Tags);

        // --- Tag extraction helpers ---
        TMap<FString, FString> ExtractTags(const class FXmlNode* Element);
        FString GetTag(const TMap<FString, FString>& Tags, const FString& Key, const FString& Default = TEXT(""));
        float GetTagFloat(const TMap<FString, FString>& Tags, const FString& Key, float Default = 0.f);
        int32 GetTagInt(const TMap<FString, FString>& Tags, const FString& Key, int32 Default = 0);

        // --- Geometry helpers ---
        FVector2D LatLonToLocal(double Lat, double Lon) const;
        bool ResolveNodeRefs(const TArray<int64>& NodeRefs, TArray<FVector2D>& OutPoints, TArray<FDoublePoint2D>& OutRawPoints) const;

        // --- Internal OSM data structures ---
        struct FOSMWay
        {
                int64 Id;
                TArray<int64> NodeRefs;
                TMap<FString, FString> Tags;
        };

        // --- Internal data storage ---
        TMap<int64, FVector2D> NodeCoords;           // Converted local coords
        TMap<int64, FDoublePoint2D> NodeRawCoords;    // Raw WGS84 lon/lat
        TMap<int64, TMap<FString, FString>> NodeTags; // Tags attached to nodes
        TMap<int64, FOSMWay> WayMap;                  // All ways for relation lookup

        TArray<FGeoBuilding> Buildings;
        TArray<FGeoRoad> Roads;
        TArray<FGeoWater> Waters;
        TArray<FGeoVegetation> Vegetations;
        TArray<FGeoPOI> POIs;
        TArray<FGeoTree> Trees;
        TArray<FGeoRailway> Railways;
        TArray<FGeoFence> Fences;
        TArray<FGeoPowerLine> PowerLines;
        TArray<FGeoModelInstance> ModelInstances;
        TArray<FElevationSample> ElevationSamples;

        double CenterLatitude, CenterLongitude;
        double MinLat, MaxLat, MinLon, MaxLon;
        bool bIsLoaded;
        FString LastError;
};
