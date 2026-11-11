// Copyright Hiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Array.h"
#include "Math/Vector2D.h"
#include "Math/Color.h"

// ============================================================================
// FDoublePoint2D - Double-precision 2D point for WGS84 lon/lat storage
// X = Longitude, Y = Latitude
// ============================================================================
struct FDoublePoint2D
{
        double X;
        double Y;

        FDoublePoint2D() : X(0.0), Y(0.0) {}
        FDoublePoint2D(double InX, double InY) : X(InX), Y(InY) {}

        FDoublePoint2D operator-(const FDoublePoint2D& Other) const
        {
                return FDoublePoint2D(X - Other.X, Y - Other.Y);
        }

        FDoublePoint2D operator+(const FDoublePoint2D& Other) const
        {
                return FDoublePoint2D(X + Other.X, Y + Other.Y);
        }
};

// ============================================================================
// ERoofShape - Enum for roof shape types (from OSM roof:shape tag)
// ============================================================================
enum class ERoofShape
{
        Flat,
        Gabled,
        Saltbox,
        QuadrupleSaltbox,
        Hipped,
        HalfHipped,     // Dutch hip / clipped gable
        Pyramidal,
        Skillion,
        Mansard,
        Gambrel,
        Dome,
        Onion,
        Round,          // Barrel vault
        CrossGabled,    // Perpendicular ridges for L/T footprints
};

// ============================================================================
// FGeoBuilding - Building data parsed from GeoJSON/OSM
// All color fields come from OSM tags (building:colour, roof:colour, etc.)
// Default colors are provided when OSM data doesn't specify a color.
// ============================================================================
struct FGeoBuilding
{
        TArray<FVector2D> Nodes;        // Building footprint OUTER ring (local meters)
        TArray<FDoublePoint2D> RawLonLat; // Raw WGS84 lon/lat for precision
        // Inner rings = courtyards / holes (multipolygon "inner" members).
        // Holes are in the same coord space as Nodes (local meters); HolesRaw
        // keeps the WGS84 source so they convert alongside Nodes.
        TArray<TArray<FVector2D>>      Holes;
        TArray<TArray<FDoublePoint2D>> HolesRaw;
        double Height;                  // Building height in meters
        double MinHeight;               // Minimum height / base elevation in meters
        int32 Levels;                   // Number of building levels (building:levels)
        int32 MinLevel;                 // Minimum level (building:min_level)
        double RoofHeight;              // Explicit roof height in meters (> 0.0 means specified)
        int32 RoofLevels;               // Roof levels count (roof:levels)
        ERoofShape RoofShape;           // Roof shape enum value
        double RoofAngle;               // Roof angle in degrees (roof:angle)
        double RoofDirection;           // Roof direction in degrees (roof:direction)
        FString RoofOrientation;        // Roof orientation (roof:orientation: across/along)
        FString RoofMaterial;           // Roof material string (roof:material tag)
        FString Name;                   // Building name (name tag)
        int32 BuildingType;             // 0=residential, 1=commercial, 2=industrial, 3=other
        FString BuildingTypeStr;        // Raw building type string from OSM (e.g. "residential", "yes")
        bool bIsBuildingPart;           // True if this is a building:part
        bool bIsTowerPart;              // True if building:part=tower (a free-standing tower, not a roof turret)
        bool bSuppressOutline;          // True if this is an outline covered by building:parts (don't render in 3D)
        bool bSuppressRoof;             // True if this part's flat roof is covered by another part on top (skip roof cap)
        FString BuildingPartId;         // OSM ID for building:part grouping
        bool bHasExplicitBuildingColour;// True if building:colour was explicitly set in OSM
        bool bHasExplicitRoofColor;     // True if roof:colour was explicitly set in OSM

        // Colors from OSM tags
        FLinearColor WallColor;         // From building:colour (default: warm beige)
        FLinearColor RoofColor;         // From roof:colour (default: gray-brown)
        FLinearColor BuildingColour;    // Parsed building colour (from GeoJSON building:colour)
        FLinearColor Color;             // Alias: same as WallColor for OSM compatibility
        int32 FacadeMaterialType;       // 0=plaster, 1=brick, 2=block/dark_stone, 3=light_stone, 4=wood, 5=glass
        FVector2D Center;               // Computed center of the building footprint (local meters)

        FGeoBuilding()
                : Height(0.0)
                , MinHeight(0.0)
                , Levels(0)
                , MinLevel(0)
                , RoofHeight(0.0)
                , RoofLevels(0)
                , RoofShape(ERoofShape::Flat)
                , RoofAngle(0.0)
                , RoofDirection(-1.0)   // -1 = not specified; 0..360 = explicit OSM value
                , BuildingType(0)
                , bIsBuildingPart(false)
                , bIsTowerPart(false)
                , bSuppressOutline(false)
                , bSuppressRoof(false)
                , bHasExplicitBuildingColour(false)
                , bHasExplicitRoofColor(false)
                , WallColor(FLinearColor(0.71f, 0.67f, 0.63f))    // Default: warm beige (180,170,160)
                , RoofColor(FLinearColor(0.45f, 0.38f, 0.33f))     // Default: gray-brown
                , BuildingColour(FLinearColor(0.71f, 0.67f, 0.63f))
                , Color(WallColor)                                   // Alias for WallColor
                , FacadeMaterialType(0)
                , Center(FVector2D::ZeroVector)
        {}
};

// ============================================================================
// ERoadSide - sidewalk / cycleway side relative to way direction (streets-gl)
// ============================================================================
enum class ERoadSide : uint8
{
        None = 0,   // no tagged side feature
        Left,
        Right,
        Both
};

// ============================================================================
// FGeoRoad - Road data parsed from GeoJSON/OSM
// Colors from OSM tags or sensible defaults
// ============================================================================
struct FGeoRoad
{
        TArray<FVector2D> Points;       // Road centerline points (local meters)
        TArray<FDoublePoint2D> RawLonLat; // Raw WGS84 lon/lat for precision
        float Width;                    // Road width in meters (0 = default)
        FString Name;                   // Road name (name tag)
        int32 RoadType;                 // 0=residential, 1=primary, 2=secondary, 3=tertiary, 4=service, 5=other
        FString RoadTypeStr;            // Raw highway type string from OSM (e.g. "motorway", "residential")
        bool bIsHighway;                // True if this is a highway (major road)

        // OSM "layer" tag (bridge=+N above ground, tunnel=-N below ground, 0 =
        // ground level). Used to vertically separate grade-separated ramps at a
        // multi-level interchange so they don't render flattened onto one plane
        // and visually cross/overlap in the middle of their curves.
        int32 Layer;

        // streets-gl sidewalk=* / sidewalk:left|right|both — only emit flanking
        // sidewalk ribbons when tagged (null/None = no sidewalk).
        ERoadSide SidewalkSide;

        // streets-gl cycleway:left|right|both=lane — emit flanking cycle ribbons.
        ERoadSide CyclewaySide;

        // Colors from OSM tags
        FLinearColor RoadColor;         // Road surface color (default: dark gray)
        FLinearColor SidewalkColor;     // Sidewalk surface color (default: light gray)
        FLinearColor Color;             // Alias: same as RoadColor for OSM compatibility

        FGeoRoad()
                : Width(0.f)
                , RoadType(0)
                , bIsHighway(false)
                , Layer(0)
                , SidewalkSide(ERoadSide::None)
                , CyclewaySide(ERoadSide::None)
                , RoadColor(FLinearColor(0.24f, 0.24f, 0.24f))
                , SidewalkColor(FLinearColor(0.71f, 0.69f, 0.67f))
                , Color(RoadColor)
        {}
};

// ============================================================================
// EFenceKind - barrier type (streets-gl fence / wall / hedge)
// ============================================================================
enum class EFenceKind : uint8
{
        Fence = 0,
        Wall,
        Hedge
};

// ============================================================================
// FGeoFence - barrier=fence|wall|hedge polyline (streets-gl FenceBuilder / wall extrusion)
// ============================================================================
struct FGeoFence
{
        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawLonLat;
        EFenceKind Kind;
        FString Material;       // wood, chain_link, metal, concrete, stone, hedge, ...
        float Height;           // meters (top relative to min height)
        float MinHeight;        // meters above ground
        float Thickness;        // meters — walls ~0.8, fences thin

        FGeoFence()
                : Kind(EFenceKind::Fence)
                , Height(2.f)
                , MinHeight(0.f)
                , Thickness(0.05f)
        {}
};

// ============================================================================
// FGeoPowerLine - power=line / minor_line centerline (wires between poles/towers)
// ============================================================================
struct FGeoPowerLine
{
        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawLonLat;
        bool bIsMinor;          // power=minor_line → lower wire height

        FGeoPowerLine()
                : bIsMinor(false)
        {}
};

// ============================================================================
// FGeoWater - Water body data parsed from GeoJSON/OSM
// ============================================================================
struct FGeoWater
{
        TArray<FVector2D> Points;       // Water body polygon/polyline points
        TArray<FDoublePoint2D> RawLonLat; // Raw WGS84 lon/lat for precision
        TArray<TArray<FVector2D>> Holes;
        TArray<TArray<FDoublePoint2D>> HolesRaw;
        FString Name;                   // Water body name

        // true  = closed, filled polygon (lake / pond / sea / reservoir) — original behaviour.
        // false = open polyline (river / stream / canal / drain); GenerateWaterMeshes turns
        //         this into a width-buffered ribbon instead of a filled polygon.
        bool bIsClosed = true;
        float Width = 0.f;              // meters — only used when bIsClosed == false.

        // Colors from OSM tags
        FLinearColor WaterColor;        // Water surface color (default: blue)
        FLinearColor Color;             // Alias: same as WaterColor for OSM compatibility

        FGeoWater()
                : WaterColor(FLinearColor(0.12f, 0.39f, 0.78f))        // Default: blue (30,100,200)
                , Color(WaterColor)                                        // Alias for WaterColor
        {}
};

// ============================================================================
// FGeoVegetation - Vegetation area data parsed from GeoJSON/OSM
// ============================================================================
struct FGeoVegetation
{
        TArray<FVector2D> Points;       // Vegetation area polygon points
        TArray<FDoublePoint2D> RawLonLat; // Raw WGS84 lon/lat for precision
        TArray<TArray<FVector2D>> Holes;
        TArray<TArray<FDoublePoint2D>> HolesRaw;
        FString Name;                   // Vegetation area name
        FString VegType;                // Vegetation type string (grass, forest, park, etc.)

        // Colors from OSM tags
        FLinearColor VegetationColor;   // Vegetation surface color (default: green)
        FLinearColor Color;             // Alias: same as VegetationColor for OSM compatibility

        FGeoVegetation()
                : VegType(TEXT("grass"))
                , VegetationColor(FLinearColor(0.20f, 0.55f, 0.16f))   // Default: green (50,140,40)
                , Color(VegetationColor)                                   // Alias for VegetationColor
        {}
};

// ============================================================================
// FGeoPOI - Point of Interest data parsed from OSM
// ============================================================================
struct FGeoPOI
{
        FVector2D Location;             // POI location (local meters)
        FDoublePoint2D RawLonLat;       // Raw WGS84 lon/lat
        FString Name;                   // POI name
        FString POIType;                // POI type (amenity, shop, tourism, etc.)
        FString POISubType;             // POI sub-type (restaurant, cafe, hotel, etc.)
        FString Type;                   // Alias for POI type (OSM compatibility)
        FLinearColor Color;             // POI color (if specified in OSM)

        FGeoPOI()
                : Location(FVector2D::ZeroVector)
                , RawLonLat(0.0, 0.0)
                , Color(FLinearColor::White)
        {}
};

// ============================================================================
// FGeoRailway - Railway data parsed from OSM
// ============================================================================
struct FGeoRailway
{
        TArray<FVector2D> Points;       // Railway centerline points (local meters)
        TArray<FDoublePoint2D> RawLonLat; // Raw WGS84 lon/lat for precision
        FString Name;                   // Railway name
        FString RailwayType;            // rail, light_rail, subway, tram, etc.
        float Gauge;                    // Track gauge in mm (default: 1435 = standard)
        int32 Tracks;                   // Number of tracks (default: 1)

        FGeoRailway()
                : Gauge(1435.0f)
                , Tracks(1)
        {}
};

// ============================================================================
// FGeoTree - Individual tree data parsed from OSM (natural=tree nodes)
// ============================================================================
struct FGeoTree
{
        FVector2D Location;             // Tree location (local meters)
        FDoublePoint2D RawLonLat;       // Raw WGS84 lon/lat
        float CrownRadius;              // Tree crown radius in meters (default: 3.0)
        float Height;                   // Tree height in meters (default: 10.0)
        FString Genus;                  // Tree genus (from species:genus tag)
        FString Species;                // Tree species (from species tag)

        FGeoTree()
                : Location(FVector2D::ZeroVector)
                , RawLonLat(0.0, 0.0)
                , CrownRadius(3.0f)
                , Height(10.0f)
        {}
};

// ============================================================================
// FElevationSample - WGS84 elevation point (OSM ele tag or GeoJSON Z coord)
// Used to deform the ground mesh when enough samples are available.
// ============================================================================
struct FElevationSample
{
        FVector2D Location;             // local meters (after ConvertAllCoordsToLocal)
        FDoublePoint2D RawLonLat;       // raw WGS84 lon/lat
        double Elevation;               // meters above sea level

        FElevationSample()
                : Location(FVector2D::ZeroVector)
                , RawLonLat(0.0, 0.0)
                , Elevation(0.0)
        {}
};

// ============================================================================
// FGeoPOIHelpers - marker colours by amenity/shop/tourism (streets.gl style)
// ============================================================================
class FGeoPOIHelpers
{
public:
        static FLinearColor MarkerColorForPOI(const FGeoPOI& POI)
        {
                const FString Sub = POI.POISubType.ToLower();
                const FString Type = POI.POIType.ToLower();

                if (Sub.Contains(TEXT("restaurant")) || Sub.Contains(TEXT("cafe")) || Sub.Contains(TEXT("fast_food")))
                        return FLinearColor(0.90f, 0.35f, 0.20f);
                if (Sub.Contains(TEXT("hospital")) || Sub.Contains(TEXT("clinic")) || Sub.Contains(TEXT("doctors")))
                        return FLinearColor(0.95f, 0.95f, 0.95f);
                if (Sub.Contains(TEXT("school")) || Sub.Contains(TEXT("university")) || Sub.Contains(TEXT("college")))
                        return FLinearColor(0.95f, 0.85f, 0.20f);
                if (Sub.Contains(TEXT("fuel")) || Sub.Contains(TEXT("parking")))
                        return FLinearColor(0.55f, 0.55f, 0.55f);
                if (Type == TEXT("shop"))
                        return FLinearColor(0.20f, 0.45f, 0.90f);
                if (Type == TEXT("tourism"))
                        return FLinearColor(0.65f, 0.25f, 0.85f);
                if (Type == TEXT("amenity"))
                        return FLinearColor(0.85f, 0.25f, 0.25f);
                return POI.Color;
        }
};

// ============================================================================
// FGeoModelInstance - a single placed 3D model from an OSM node (street
// furniture, power, landmarks, ...). ModelType matches a folder under
// /HiroGeo/Models/ (e.g. "bench", "hydrant", "transmission_tower"). Mirrors
// streets-gl's node->instance mapping (OSMNodeQualifierFactory).
// ============================================================================
struct FGeoModelInstance
{
        FVector2D Location;             // local meters (filled by ConvertAllCoordsToLocal)
        FDoublePoint2D RawLonLat;       // raw WGS84 lon/lat
        FString ModelType;              // folder name under /HiroGeo/Models/
        float RotationDeg;              // OSM 'direction' yaw; < 0 = random
        float Height;                   // meters (0 = use model's native size)

        FGeoModelInstance()
                : Location(FVector2D::ZeroVector)
                , RawLonLat(0.0, 0.0)
                , RotationDeg(-1.f)
                , Height(0.f)
        {}
};

// ============================================================================
// FOSMTagHelpers - Parse OSM measurement / direction strings (streets-gl parity)
// ============================================================================
class FOSMTagHelpers
{
public:
        /** Parse an OSM length/height string into meters.
         *  Supports bare numbers, "12m", "120cm", feet/inches ("12'6\"").
         *  Returns DefaultValue when parsing fails. */
        static double ParseMeters(const FString& Raw, double DefaultValue = 0.0)
        {
                FString S = Raw.TrimStartAndEnd().Replace(TEXT(","), TEXT(".")).Replace(TEXT(" "), TEXT(""));
                S = S.Replace(TEXT("feet"), TEXT("'")).Replace(TEXT("ft"), TEXT("'"));
                if (S.IsEmpty()) return DefaultValue;

                if (S.Contains(TEXT("cm")))
                {
                        const double V = FCString::Atod(*S.Replace(TEXT("cm"), TEXT("")));
                        return FMath::IsFinite(V) ? V * 0.01 : DefaultValue;
                }
                if (S.Contains(TEXT("m")) && !S.Contains(TEXT("cm")))
                {
                        // Strip trailing unit carefully: "12m" / "12.5m" but keep pure scientific notation rare in OSM.
                        FString Num = S;
                        Num.ReplaceInline(TEXT("m"), TEXT(""));
                        const double V = FCString::Atod(*Num);
                        return FMath::IsFinite(V) ? V : DefaultValue;
                }
                if (S.Contains(TEXT("'")))
                {
                        FString FeetStr, InchStr;
                        if (S.Split(TEXT("'"), &FeetStr, &InchStr))
                        {
                                const double Feet = FCString::Atod(*FeetStr);
                                InchStr.ReplaceInline(TEXT("\""), TEXT(""));
                                const double Inches = InchStr.IsEmpty() ? 0.0 : FCString::Atod(*InchStr);
                                return (Feet * 12.0 + Inches) * 0.0254;
                        }
                }
                if (S.Contains(TEXT("\"")))
                {
                        const double Inches = FCString::Atod(*S.Replace(TEXT("\""), TEXT("")));
                        return FMath::IsFinite(Inches) ? Inches * 0.0254 : DefaultValue;
                }

                const double V = FCString::Atod(*S);
                return FMath::IsFinite(V) ? V : DefaultValue;
        }

        /** streets-gl Simple 3D Buildings level height (meters). */
        static constexpr double LevelHeightMeters = 4.0;

        /** Parse sidewalk=* / sidewalk:left|right|both → ERoadSide (streets-gl). */
        static ERoadSide ParseSidewalkSide(const TMap<FString, FString>& Tags)
        {
                auto Val = [&](const FString& K) -> FString
                {
                        const FString* V = Tags.Find(K);
                        return V ? V->ToLower() : FString();
                };
                const bool bBoth = Val(TEXT("sidewalk:both")) == TEXT("yes") || Val(TEXT("sidewalk")) == TEXT("both");
                const bool bLeft = Val(TEXT("sidewalk:left")) == TEXT("yes") || Val(TEXT("sidewalk")) == TEXT("left") || bBoth;
                const bool bRight = Val(TEXT("sidewalk:right")) == TEXT("yes") || Val(TEXT("sidewalk")) == TEXT("right") || bBoth;
                if (bLeft && bRight) return ERoadSide::Both;
                if (bLeft) return ERoadSide::Left;
                if (bRight) return ERoadSide::Right;
                return ERoadSide::None;
        }

        /** Parse cycleway:*=lane → ERoadSide (streets-gl). */
        static ERoadSide ParseCyclewaySide(const TMap<FString, FString>& Tags)
        {
                auto Val = [&](const FString& K) -> FString
                {
                        const FString* V = Tags.Find(K);
                        return V ? V->ToLower() : FString();
                };
                const bool bBoth = Val(TEXT("cycleway:both")) == TEXT("lane") || Val(TEXT("cycleway")) == TEXT("lane");
                const bool bLeft = Val(TEXT("cycleway:left")) == TEXT("lane") || bBoth;
                const bool bRight = Val(TEXT("cycleway:right")) == TEXT("lane") || bBoth;
                if (bLeft && bRight) return ERoadSide::Both;
                if (bLeft) return ERoadSide::Left;
                if (bRight) return ERoadSide::Right;
                return ERoadSide::None;
        }
};

// ============================================================================
// FOSMColorParser - Utility for parsing OSM color strings to FLinearColor
// Handles hex colors (#RGB, #RRGGBB), CSS named colors, and fallback defaults.
// OSM tags like building:colour, roof:colour use these formats.
// ============================================================================
class FOSMColorParser
{
public:
        /** Parse an OSM color string to FLinearColor (alias for ParseColor).
         *  Used by OSMLoader.cpp as FOSMColorParser::Parse() */
        static FLinearColor Parse(const FString& ColorStr, const FLinearColor& DefaultColor = FLinearColor::White)
        {
                return ParseColor(ColorStr, DefaultColor);
        }

        /** Parse an OSM color string to FLinearColor.
         *  Supports:
         *  - Hex: #RGB, #RRGGBB, #RRGGBBAA
         *  - Named: white, black, red, green, blue, yellow, grey/gray, brown,
         *           cream, orange, pink, purple, cyan, darkgrey/darkgray, etc.
         *  - Returns DefaultColor if parsing fails. */
        static FLinearColor ParseColor(const FString& ColorStr, const FLinearColor& DefaultColor = FLinearColor::White)
        {
                FString Trimmed = ColorStr.TrimStartAndEnd();

                // Hex color: #RGB or #RRGGBB or #RRGGBBAA
                if (Trimmed.StartsWith(TEXT("#")))
                {
                        return ParseHexColor(Trimmed, DefaultColor);
                }

                // Named color (case-insensitive)
                return ParseNamedColor(Trimmed, DefaultColor);
        }

        /** British spelling alias for ParseHexColor (public for backward compatibility) */
        static FLinearColor ParseHexColour(const FString& HexStr, const FLinearColor& DefaultColor)
        {
                return ParseHexColor(HexStr, DefaultColor);
        }

private:
        static FLinearColor ParseHexColor(const FString& HexStr, const FLinearColor& DefaultColor)
        {
                // Remove the # prefix
                FString Digits = HexStr.RightChop(1);

                // #RGB shorthand -> #RRGGBB
                if (Digits.Len() == 3)
                {
                        FString Expanded;
                        Expanded += Digits[0]; Expanded += Digits[0];
                        Expanded += Digits[1]; Expanded += Digits[1];
                        Expanded += Digits[2]; Expanded += Digits[2];
                        Digits = Expanded;
                }

                if (Digits.Len() == 6 || Digits.Len() == 8)
                {
                        int32 R = 0, G = 0, B = 0, A = 255;
                        if (!HexToInt(Digits.Mid(0, 2), R)) return DefaultColor;
                        if (!HexToInt(Digits.Mid(2, 2), G)) return DefaultColor;
                        if (!HexToInt(Digits.Mid(4, 2), B)) return DefaultColor;
                        if (Digits.Len() == 8)
                        {
                                if (!HexToInt(Digits.Mid(6, 2), A)) A = 255;
                        }
                        return FLinearColor(R / 255.f, G / 255.f, B / 255.f, A / 255.f);
                }

                return DefaultColor;
        }

        static bool HexToInt(const FString& Hex, int32& OutValue)
        {
                OutValue = 0;
                for (int32 i = 0; i < Hex.Len(); i++)
                {
                        TCHAR C = Hex[i];
                        OutValue *= 16;
                        if (C >= '0' && C <= '9') OutValue += (C - '0');
                        else if (C >= 'A' && C <= 'F') OutValue += (C - 'A' + 10);
                        else if (C >= 'a' && C <= 'f') OutValue += (C - 'a' + 10);
                        else return false;
                }
                return true;
        }

        static FLinearColor ParseNamedColor(const FString& Name, const FLinearColor& DefaultColor)
        {
                // Convert to lowercase for comparison
                FString Lower = Name.ToLower();

                // Common OSM color names
                if (Lower == TEXT("white") || Lower == TEXT("#ffffff"))
                        return FLinearColor(1.f, 1.f, 1.f);
                if (Lower == TEXT("black") || Lower == TEXT("#000000"))
                        return FLinearColor(0.f, 0.f, 0.f);
                if (Lower == TEXT("red"))
                        return FLinearColor(0.85f, 0.15f, 0.15f);
                if (Lower == TEXT("green"))
                        return FLinearColor(0.15f, 0.65f, 0.15f);
                if (Lower == TEXT("blue"))
                        return FLinearColor(0.15f, 0.25f, 0.85f);
                if (Lower == TEXT("yellow"))
                        return FLinearColor(0.95f, 0.90f, 0.20f);
                if (Lower == TEXT("orange"))
                        return FLinearColor(0.95f, 0.55f, 0.10f);
                if (Lower == TEXT("pink"))
                        return FLinearColor(0.95f, 0.65f, 0.70f);
                if (Lower == TEXT("purple"))
                        return FLinearColor(0.55f, 0.15f, 0.65f);
                if (Lower == TEXT("cyan"))
                        return FLinearColor(0.15f, 0.80f, 0.85f);
                if (Lower == TEXT("brown"))
                        return FLinearColor(0.55f, 0.35f, 0.20f);

                // Grey variants (very common in OSM)
                if (Lower == TEXT("grey") || Lower == TEXT("gray"))
                        return FLinearColor(0.55f, 0.55f, 0.55f);
                if (Lower == TEXT("darkgrey") || Lower == TEXT("darkgray") || Lower == TEXT("dark_grey"))
                        return FLinearColor(0.35f, 0.35f, 0.35f);
                if (Lower == TEXT("lightgrey") || Lower == TEXT("lightgray") || Lower == TEXT("light_grey"))
                        return FLinearColor(0.78f, 0.78f, 0.78f);

                // Building-specific named colors from OSM
                if (Lower == TEXT("cream") || Lower == TEXT("beige"))
                        return FLinearColor(0.95f, 0.90f, 0.68f);
                if (Lower == TEXT("sand"))
                        return FLinearColor(0.85f, 0.78f, 0.55f);
                if (Lower == TEXT("dark") || Lower == TEXT("darkred"))
                        return FLinearColor(0.45f, 0.10f, 0.10f);
                if (Lower == TEXT("lightblue") || Lower == TEXT("light_blue"))
                        return FLinearColor(0.65f, 0.80f, 0.95f);
                if (Lower == TEXT("lightgreen") || Lower == TEXT("light_green"))
                        return FLinearColor(0.65f, 0.88f, 0.65f);
                if (Lower == TEXT("olive"))
                        return FLinearColor(0.55f, 0.55f, 0.20f);
                if (Lower == TEXT("maroon"))
                        return FLinearColor(0.45f, 0.15f, 0.15f);
                if (Lower == TEXT("navy"))
                        return FLinearColor(0.10f, 0.10f, 0.45f);
                if (Lower == TEXT("teal"))
                        return FLinearColor(0.10f, 0.55f, 0.55f);
                if (Lower == TEXT("silver"))
                        return FLinearColor(0.75f, 0.75f, 0.75f);
                if (Lower == TEXT("gold"))
                        return FLinearColor(0.90f, 0.78f, 0.25f);
                if (Lower == TEXT("copper"))
                        return FLinearColor(0.72f, 0.45f, 0.20f);
                if (Lower == TEXT("rust"))
                        return FLinearColor(0.65f, 0.28f, 0.15f);
                if (Lower == TEXT("terracotta") || Lower == TEXT("terra_cotta"))
                        return FLinearColor(0.80f, 0.42f, 0.30f);
                if (Lower == TEXT("khaki"))
                        return FLinearColor(0.80f, 0.78f, 0.55f);
                if (Lower == TEXT("ivory"))
                        return FLinearColor(0.98f, 0.97f, 0.90f);

                // Roof-specific colors from OSM
                if (Lower == TEXT("roof_red") || Lower == TEXT("roofred") || Lower == TEXT("tile"))
                        return FLinearColor(0.70f, 0.30f, 0.22f);
                if (Lower == TEXT("roof_green") || Lower == TEXT("roofgreen"))
                        return FLinearColor(0.30f, 0.55f, 0.30f);
                if (Lower == TEXT("roof_blue") || Lower == TEXT("roofblue"))
                        return FLinearColor(0.25f, 0.35f, 0.65f);
                if (Lower == TEXT("slate") || Lower == TEXT("slate_grey"))
                        return FLinearColor(0.40f, 0.42f, 0.48f);

                // Unknown color name - return default
                UE_LOG(LogTemp, Verbose, TEXT("OSM: Unknown color name '%s', using default"), *Name);
                return DefaultColor;
        }
};

// ============================================================================
// ERoofShape parser - Convert OSM roof:shape tag to ERoofShape enum
// ============================================================================
namespace FOSMRoofShapeParser
{
        inline ERoofShape Parse(const FString& RoofShapeStr, ERoofShape Default = ERoofShape::Flat)
        {
                FString Lower = RoofShapeStr.ToLower();

                if (Lower == TEXT("flat"))       return ERoofShape::Flat;
                if (Lower == TEXT("gabled") ||
                    Lower == TEXT("gable") ||
                    Lower == TEXT("pitched"))    return ERoofShape::Gabled;
                if (Lower == TEXT("hipped"))     return ERoofShape::Hipped;
                if (Lower == TEXT("pyramidal"))  return ERoofShape::Pyramidal;
                if (Lower == TEXT("skillion") ||
                    Lower == TEXT("lean_to") ||
                    Lower == TEXT("single_pitched")) return ERoofShape::Skillion;
                if (Lower == TEXT("mansard"))    return ERoofShape::Mansard;
                if (Lower == TEXT("gambrel"))    return ERoofShape::Gambrel;
                if (Lower == TEXT("dome"))       return ERoofShape::Dome;
                if (Lower == TEXT("onion"))      return ERoofShape::Onion;
                if (Lower == TEXT("saltbox"))    return ERoofShape::Saltbox;
                if (Lower == TEXT("quadruple_saltbox") ||
                    Lower == TEXT("quadruple saltbox")) return ERoofShape::QuadrupleSaltbox;
                if (Lower == TEXT("half-hipped") || Lower == TEXT("half_hipped") ||
                    Lower == TEXT("dutch_gable") || Lower == TEXT("clipped_gable"))
                    return ERoofShape::HalfHipped;
                if (Lower == TEXT("round") || Lower == TEXT("barrel_vault") ||
                    Lower == TEXT("barrel vault"))
                    return ERoofShape::Round;
                if (Lower == TEXT("cross_gabled") || Lower == TEXT("cross gabled") ||
                    Lower == TEXT("cross-gabled"))
                    return ERoofShape::CrossGabled;

                return Default;
        }
};

// ============================================================================
// FOSMBuildingMaterialParser - Map OSM building:material to FacadeMaterialType
// ============================================================================
namespace FOSMBuildingMaterialParser
{
        inline int32 Parse(const FString& MaterialStr)
        {
                FString Lower = MaterialStr.ToLower();

                if (Lower == TEXT("brick"))             return 1;
                if (Lower == TEXT("dark_stone") ||
                    Lower == TEXT("darkstone") ||
                    Lower == TEXT("basalt") ||
                    Lower == TEXT("granite"))           return 2;
                if (Lower == TEXT("light_stone") ||
                    Lower == TEXT("lightstone") ||
                    Lower == TEXT("limestone") ||
                    Lower == TEXT("sandstone") ||
                    Lower == TEXT("marble"))            return 3;
                if (Lower == TEXT("wood") || Lower == TEXT("timber")) return 4; // FacadeWood*
                if (Lower == TEXT("glass") || Lower == TEXT("mirror")) return 5; // curtain glass
                if (Lower == TEXT("cement_block") || Lower == TEXT("cementblock") ||
                    Lower == TEXT("block"))             return 2; // FacadeBlock* (streets-gl cementBlock)
                if (Lower == TEXT("plaster") ||
                    Lower == TEXT("plastered") ||
                    Lower == TEXT("concrete") ||
                    Lower == TEXT("stucco") ||
                    Lower == TEXT("hard"))              return 0;

                return 0;  // default
        }
};

// ============================================================================
// FGeoJSONLoader - Loads GeoJSON and OSM XML files.
// Instance-based class that stores parsed data and provides accessors.
// Color tags are automatically parsed from OSM data.
// ============================================================================
class FGeoJSONLoader
{
public:
        FGeoJSONLoader();
        ~FGeoJSONLoader();

        // ---- Static utility methods (backward compatibility aliases) ----
        /** Parse an OSM hex color string. Alias for FOSMColorParser::ParseHexColour */
        static FLinearColor ParseHexColour(const FString& HexStr, const FLinearColor& DefaultColor = FLinearColor::White)
        {
                return FOSMColorParser::ParseHexColour(HexStr, DefaultColor);
        }

        /** Parse an OSM roof:shape tag. Alias for FOSMRoofShapeParser::Parse */
        static ERoofShape ParseRoofShape(const FString& RoofShapeStr, ERoofShape Default = ERoofShape::Flat)
        {
                return FOSMRoofShapeParser::Parse(RoofShapeStr, Default);
        }

        /** Load a GeoJSON file and populate data arrays */
        bool LoadGeoJSON(const FString& FilePath);

        /** Load a GeoJSON from a string */
        bool LoadGeoJSONFromString(const FString& GeoJSONContent);

        /** Load an OSM XML file and populate data arrays.
         *  Color tags are automatically parsed:
         *  - building:colour -> FGeoBuilding.WallColor
         *  - roof:colour -> FGeoBuilding.RoofColor
         *  - building:material -> FGeoBuilding.FacadeMaterialType
         *  - roof:shape -> FGeoBuilding.RoofShape
         *  - surface:colour on highways -> FGeoRoad.RoadColor
         */
        bool LoadOSM(const FString& FilePath);

        /** Convert all loaded coordinates from WGS84 to local meters.
         *  Must be called after loading, before mesh generation. */
        void ConvertAllCoordsToLocal();

        /** Calculate center point from all loaded data bounds */
        void CalculateCenterPoint();

        /** Data accessors */
        const TArray<FGeoBuilding>& GetBuildings() const { return Buildings; }
        const TArray<FGeoRoad>& GetRoads() const { return Roads; }
        const TArray<FGeoWater>& GetWaters() const { return Waters; }
        const TArray<FGeoVegetation>& GetVegetations() const { return Vegetations; }
        const TArray<FGeoPOI>& GetPOIs() const { return POIs; }
        const TArray<FGeoRailway>& GetRailways() const { return Railways; }
        const TArray<FGeoTree>& GetTrees() const { return Trees; }
        const TArray<FGeoModelInstance>& GetModelInstances() const { return ModelInstances; }
        const TArray<FElevationSample>& GetElevationSamples() const { return ElevationSamples; }

        /** Check if data has been loaded */
        bool IsLoaded() const { return bIsLoaded; }

        // Bounds accessors
        double GetCenterLatitude() const { return CenterLatitude; }
        double GetCenterLongitude() const { return CenterLongitude; }
        double GetMinLat() const { return MinLat; }
        double GetMaxLat() const { return MaxLat; }
        double GetMinLon() const { return MinLon; }
        double GetMaxLon() const { return MaxLon; }

private:
        // Internal data storage
        TArray<FGeoBuilding> Buildings;
        TArray<FGeoRoad> Roads;
        TArray<FGeoWater> Waters;
        TArray<FGeoVegetation> Vegetations;
        TArray<FGeoPOI> POIs;
        TArray<FGeoRailway> Railways;
        TArray<FGeoTree> Trees;
        TArray<FGeoModelInstance> ModelInstances; // OSM model placements (GeoJSON: usually empty)
        TArray<FElevationSample> ElevationSamples;

        // State
        bool bIsLoaded;

        // Coordinate bounds (WGS84)
        double CenterLatitude;
        double CenterLongitude;
        double MinLat, MaxLat, MinLon, MaxLon;

        // ---- GeoJSON parsing helpers ----
        void Reset();
        void ParseGeoJSON(const TSharedPtr<FJsonObject>& RootObject);
        void ParseFeature(const class TSharedPtr<class FJsonObject>& FeatureObj);
        void ParseGeometry(const class TSharedPtr<class FJsonObject>& GeometryObj, const class TSharedPtr<class FJsonObject>& PropertiesObj);
        void ParsePolygon(const class TArray<class TSharedPtr<class FJsonValue>>& Coordinates, const class TSharedPtr<class FJsonObject>& Properties);
        void ParseMultiPolygon(const class TArray<class TSharedPtr<class FJsonValue>>& Coordinates, const class TSharedPtr<class FJsonObject>& Properties);
        void ParseLineString(const class TArray<class TSharedPtr<class FJsonValue>>& Coordinates, const class TSharedPtr<class FJsonObject>& Properties);
        void ParsePoint(const class TArray<class TSharedPtr<class FJsonValue>>& Coordinates, const class TSharedPtr<class FJsonObject>& Properties);

        // ---- GeoJSON feature processors ----
        void ApplyBuildingProperties(FGeoBuilding& Building, const TSharedPtr<class FJsonObject>& Properties);
        void ProcessBuildingFeature(const TArray<FVector2D>& Nodes, const TArray<FDoublePoint2D>& RawCoords, const TSharedPtr<class FJsonObject>& Properties);
        void ProcessRoadFeature(const TArray<FVector2D>& Points, const TArray<FDoublePoint2D>& RawCoords, const TSharedPtr<class FJsonObject>& Properties);
        void ProcessWaterFeature(const TArray<FVector2D>& Points, const TArray<FDoublePoint2D>& RawCoords, const TSharedPtr<class FJsonObject>& Properties, bool bIsClosed = true);
        void ProcessPOIFeature(const FVector2D& Location, const FDoublePoint2D& RawCoord, const TSharedPtr<class FJsonObject>& Properties);
        void ProcessRailwayFeature(const TArray<FVector2D>& Points, const TArray<FDoublePoint2D>& RawCoords, const TSharedPtr<class FJsonObject>& Properties);

        // ---- Coordinate helpers ----
        static FVector2D LatLonToLocalCoords(double Lat, double Lon, double CenterLat, double CenterLon);
        static FLinearColor GetColorFromType(const FString& Type, const FString& Category);
};
