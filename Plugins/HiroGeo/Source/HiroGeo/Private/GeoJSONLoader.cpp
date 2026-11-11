// Copyright Hiro. All Rights Reserved.

#include "GeoJSONLoader.h"
#include "JsonUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	bool TryGetScalarString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& Out)
	{
		if (!Object.IsValid()) return false;
		if (Object->TryGetStringField(Field, Out)) return true;
		double Number = 0.0;
		if (Object->TryGetNumberField(Field, Number) && FMath::IsFinite(Number))
		{
			Out = FString::SanitizeFloat(Number);
			return true;
		}
		return false;
	}

	bool TryGetFiniteNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& Out)
	{
		if (!Object.IsValid()) return false;
		if (Object->TryGetNumberField(Field, Out)) return FMath::IsFinite(Out);
		FString Text;
		if (!Object->TryGetStringField(Field, Text) || !Text.IsNumeric()) return false;
		Out = FCString::Atod(*Text);
		return FMath::IsFinite(Out);
	}

	bool IsValidLonLat(double Lon, double Lat)
	{
		return FMath::IsFinite(Lon) && FMath::IsFinite(Lat) &&
			Lon >= -180.0 && Lon <= 180.0 && Lat >= -90.0 && Lat <= 90.0;
	}

	bool TryGetMeters(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double& Out)
	{
		FString Text;
		if (!TryGetScalarString(Object, Field, Text)) return false;
		Out = FOSMTagHelpers::ParseMeters(Text, 0.0);
		return FMath::IsFinite(Out);
	}
}

FGeoJSONLoader::FGeoJSONLoader()
	: bIsLoaded(false), CenterLatitude(0.0), CenterLongitude(0.0)
	, MinLat(DBL_MAX), MaxLat(-DBL_MAX), MinLon(DBL_MAX), MaxLon(-DBL_MAX) {}

FGeoJSONLoader::~FGeoJSONLoader() {}

void FGeoJSONLoader::Reset()
{
	Buildings.Reset(); Roads.Reset(); Waters.Reset(); Vegetations.Reset(); POIs.Reset();
	Trees.Reset(); Railways.Reset(); ElevationSamples.Reset(); ModelInstances.Reset();
	MinLat = DBL_MAX; MaxLat = -DBL_MAX; MinLon = DBL_MAX; MaxLon = -DBL_MAX;
	CenterLatitude = 0.0; CenterLongitude = 0.0;
	bIsLoaded = false;
}

bool FGeoJSONLoader::LoadGeoJSON(const FString& FilePath)
{
	Reset();
	FString FullPath = FPaths::IsRelative(FilePath) ? FPaths::ProjectDir() + FilePath : FilePath;
	if (!FPaths::FileExists(*FullPath)) return false;

	FString FileContent;
	if (!FFileHelper::LoadFileToString(FileContent, *FullPath)) return false;

	return LoadGeoJSONFromString(FileContent);
}

bool FGeoJSONLoader::LoadGeoJSONFromString(const FString& JsonString)
{
	Reset();

	// Parse JSON on game thread (UE JSON system is not thread-safe)
	TSharedPtr<FJsonObject> RootObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid()) return false;

	FString Type;
	if (!RootObject->TryGetStringField(TEXT("type"), Type)) return false;
	if (Type != TEXT("FeatureCollection") && Type != TEXT("Feature")) return false;

	// First pass: collect all coordinates as double to preserve precision
	ParseGeoJSON(RootObject);
	if (MinLat == DBL_MAX || MinLon == DBL_MAX) return false;
	const int32 FeatureCount = Buildings.Num() + Roads.Num() + Waters.Num() + Vegetations.Num() +
		POIs.Num() + Trees.Num() + Railways.Num() + ElevationSamples.Num();
	if (FeatureCount == 0) return false;
	CalculateCenterPoint();
	ConvertAllCoordsToLocal();

	bIsLoaded = true;
	return bIsLoaded;
}

void FGeoJSONLoader::ParseGeoJSON(const TSharedPtr<FJsonObject>& RootObject)
{
	if (!RootObject.IsValid()) return;
	FString Type = RootObject->GetStringField(TEXT("type"));

	if (Type == TEXT("FeatureCollection"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Features = nullptr;
		if (RootObject->TryGetArrayField(TEXT("features"), Features))
		{
			for (const TSharedPtr<FJsonValue>& FeatureValue : *Features)
			{
				if (FeatureValue.IsValid() && FeatureValue->AsObject().IsValid()) ParseFeature(FeatureValue->AsObject());
			}
		}
	}
	else if (Type == TEXT("Feature")) ParseFeature(RootObject);
}

void FGeoJSONLoader::ParseFeature(const TSharedPtr<FJsonObject>& Feature)
{
	if (!Feature.IsValid()) return;
	const TSharedPtr<FJsonObject>* Geometry = nullptr;
	const TSharedPtr<FJsonObject>* Properties = nullptr;

	TSharedPtr<FJsonObject> GeometryObj = Feature->TryGetObjectField(TEXT("geometry"), Geometry) ? *Geometry : nullptr;
	TSharedPtr<FJsonObject> PropertiesObj = Feature->TryGetObjectField(TEXT("properties"), Properties) ? *Properties : nullptr;

	if (GeometryObj.IsValid()) ParseGeometry(GeometryObj, PropertiesObj);
}

void FGeoJSONLoader::ParseGeometry(const TSharedPtr<FJsonObject>& Geometry, const TSharedPtr<FJsonObject>& Properties)
{
	FString GeomType;
	const TArray<TSharedPtr<FJsonValue>>* Coordinates = nullptr;
	if (!Geometry->TryGetStringField(TEXT("type"), GeomType) || !Geometry->TryGetArrayField(TEXT("coordinates"), Coordinates)) return;

	if (GeomType == TEXT("Point")) ParsePoint(*Coordinates, Properties);
	else if (GeomType == TEXT("LineString")) ParseLineString(*Coordinates, Properties);
	else if (GeomType == TEXT("Polygon")) ParsePolygon(*Coordinates, Properties);
	else if (GeomType == TEXT("MultiPolygon")) ParseMultiPolygon(*Coordinates, Properties);
	else if (GeomType == TEXT("MultiLineString"))
	{
		for (const TSharedPtr<FJsonValue>& LineValue : *Coordinates)
		{
			if (LineValue.IsValid() && LineValue->Type == EJson::Array) ParseLineString(LineValue->AsArray(), Properties);
		}
	}
}

void FGeoJSONLoader::ParsePoint(const TArray<TSharedPtr<FJsonValue>>& Coordinates, const TSharedPtr<FJsonObject>& Properties)
{
	if (Coordinates.Num() < 2 || !Coordinates[0].IsValid() || !Coordinates[1].IsValid() || Coordinates[0]->Type != EJson::Number || Coordinates[1]->Type != EJson::Number) return;
	double Lon = Coordinates[0]->AsNumber();
	double Lat = Coordinates[1]->AsNumber();
	if (!IsValidLonLat(Lon, Lat)) return;

	MinLat = FMath::Min(MinLat, Lat); MaxLat = FMath::Max(MaxLat, Lat);
	MinLon = FMath::Min(MinLon, Lon); MaxLon = FMath::Max(MaxLon, Lon);

	double Ele = 0.0;
	bool bHasEle = false;
	if (Coordinates.Num() >= 3 && Coordinates[2].IsValid() && Coordinates[2]->Type == EJson::Number)
	{
		Ele = Coordinates[2]->AsNumber();
		bHasEle = true;
	}
	if (Properties.IsValid() && Properties->HasField(TEXT("ele")))
	{
		bHasEle = TryGetFiniteNumber(Properties, TEXT("ele"), Ele);
	}
	if (bHasEle)
	{
		FElevationSample Sample;
		Sample.RawLonLat = FDoublePoint2D(Lon, Lat);
		Sample.Elevation = Ele;
		ElevationSamples.Add(Sample);
	}

	// Check if this point is a tree (natural=tree)
	if (Properties.IsValid() && Properties->HasField(TEXT("natural")))
	{
		FString Natural = Properties->GetStringField(TEXT("natural"));
		if (Natural == TEXT("tree"))
		{
			FGeoTree Tree;
			Tree.Location = FVector2D(static_cast<float>(Lon), static_cast<float>(Lat));
			Tree.RawLonLat = FDoublePoint2D(Lon, Lat);
			if (Properties->HasField(TEXT("species"))) Tree.Species = Properties->GetStringField(TEXT("species"));
			if (Properties->HasField(TEXT("genus"))) Tree.Genus = Properties->GetStringField(TEXT("genus"));
			double Parsed = 0.0;
			if (TryGetFiniteNumber(Properties, TEXT("height"), Parsed))
			{
				Tree.Height = static_cast<float>(Parsed);
				if (Tree.Height <= 0.0f) Tree.Height = 8.0f;
			}
			if (TryGetFiniteNumber(Properties, TEXT("crown_radius"), Parsed))
			{
				Tree.CrownRadius = static_cast<float>(Parsed);
				if (Tree.CrownRadius <= 0.0f) Tree.CrownRadius = 3.0f;
			}
			Trees.Add(Tree);
			return;
		}
	}

	// Store placeholder FVector2D (will be overwritten by ConvertAllCoordsToLocal)
	// and raw double-precision coordinate for accurate conversion
	ProcessPOIFeature(FVector2D(static_cast<float>(Lon), static_cast<float>(Lat)),
		FDoublePoint2D(Lon, Lat), Properties);
}

void FGeoJSONLoader::ParseLineString(const TArray<TSharedPtr<FJsonValue>>& Coordinates, const TSharedPtr<FJsonObject>& Properties)
{
	if (Coordinates.Num() < 2) return;
	TArray<FVector2D> Points;
	TArray<FDoublePoint2D> RawPoints;
	TArray<FElevationSample> LineElevationSamples;
	bool bInvalidCoordinate = false;

	for (const TSharedPtr<FJsonValue>& CoordValue : Coordinates)
	{
		if (!CoordValue.IsValid() || CoordValue->Type != EJson::Array)
		{
			bInvalidCoordinate = true;
			break;
		}
		const TArray<TSharedPtr<FJsonValue>>& Coord = CoordValue->AsArray();
		if (Coord.Num() < 2 || !Coord[0].IsValid() || !Coord[1].IsValid() ||
			Coord[0]->Type != EJson::Number || Coord[1]->Type != EJson::Number)
		{
			bInvalidCoordinate = true;
			break;
		}
		double Lon = Coord[0]->AsNumber();
		double Lat = Coord[1]->AsNumber();
		if (!IsValidLonLat(Lon, Lat)) { bInvalidCoordinate = true; break; }
		Points.Add(FVector2D(static_cast<float>(Lon), static_cast<float>(Lat)));
		RawPoints.Add(FDoublePoint2D(Lon, Lat));

		if (Coord.Num() >= 3 && Coord[2].IsValid() && Coord[2]->Type == EJson::Number)
		{
			const double Elevation = Coord[2]->AsNumber();
			if (!FMath::IsFinite(Elevation)) { bInvalidCoordinate = true; break; }
			FElevationSample Sample;
			Sample.RawLonLat = FDoublePoint2D(Lon, Lat);
			Sample.Elevation = Elevation;
			LineElevationSamples.Add(Sample);
		}
	}
	if (bInvalidCoordinate || Points.Num() < 2) return;
	for (const FDoublePoint2D& Point : RawPoints)
	{
		MinLat = FMath::Min(MinLat, Point.Y); MaxLat = FMath::Max(MaxLat, Point.Y);
		MinLon = FMath::Min(MinLon, Point.X); MaxLon = FMath::Max(MaxLon, Point.X);
	}
	ElevationSamples.Append(MoveTemp(LineElevationSamples));

	// Check if this is a railway
	if (Properties.IsValid() && Properties->HasField(TEXT("railway")))
	{
		ProcessRailwayFeature(Points, RawPoints, Properties);
	}
	else if (Properties.IsValid() && Properties->HasField(TEXT("waterway")))
	{
		// River/stream/canal represented as an open LineString — must NOT become
		// a road. Flagged bIsClosed=false so GenerateWaterMeshes builds a ribbon.
		ProcessWaterFeature(Points, RawPoints, Properties, /*bIsClosed*/ false);
	}
	else if (Properties.IsValid() && Properties->HasField(TEXT("highway")))
	{
		ProcessRoadFeature(Points, RawPoints, Properties);
	}
}

void FGeoJSONLoader::ParsePolygon(const TArray<TSharedPtr<FJsonValue>>& Coordinates, const TSharedPtr<FJsonObject>& Properties)
{
	if (Coordinates.Num() < 1 || Coordinates.Num() > 10000) return;

	auto ParseRing = [this](const TSharedPtr<FJsonValue>& RingValue, TArray<FDoublePoint2D>& Out) -> bool
	{
		Out.Reset();
		if (!RingValue.IsValid() || RingValue->Type != EJson::Array) return false;
		const TArray<TSharedPtr<FJsonValue>>& Ring = RingValue->AsArray();
		if (Ring.Num() < 3 || Ring.Num() > 1000000) return false;
		Out.Reserve(Ring.Num());
		for (const TSharedPtr<FJsonValue>& Point : Ring)
		{
			if (!Point.IsValid() || Point->Type != EJson::Array) return false;
			const TArray<TSharedPtr<FJsonValue>>& Pair = Point->AsArray();
			if (Pair.Num() < 2 || !Pair[0].IsValid() || !Pair[1].IsValid() ||
				Pair[0]->Type != EJson::Number || Pair[1]->Type != EJson::Number) return false;
			const double Lon = Pair[0]->AsNumber();
			const double Lat = Pair[1]->AsNumber();
			if (!IsValidLonLat(Lon, Lat)) return false;
			Out.Add(FDoublePoint2D(Lon, Lat));
		}
		if (Out.Num() > 1 && Out[0].X == Out.Last().X && Out[0].Y == Out.Last().Y)
			Out.Pop();
		return Out.Num() >= 3;
	};

	TArray<FDoublePoint2D> RawNodes;
	if (!ParseRing(Coordinates[0], RawNodes)) return;
	TArray<TArray<FDoublePoint2D>> RawHoles;
	for (int32 RingIndex = 1; RingIndex < Coordinates.Num(); ++RingIndex)
	{
		TArray<FDoublePoint2D> Ring;
		if (!ParseRing(Coordinates[RingIndex], Ring)) return; // malformed geometry is rejected as a whole
		RawHoles.Add(MoveTemp(Ring));
	}

	TArray<FVector2D> Nodes;
	Nodes.Reserve(RawNodes.Num());
	for (const FDoublePoint2D& Point : RawNodes)
	{
		MinLat = FMath::Min(MinLat, Point.Y); MaxLat = FMath::Max(MaxLat, Point.Y);
		MinLon = FMath::Min(MinLon, Point.X); MaxLon = FMath::Max(MaxLon, Point.X);
		Nodes.Add(FVector2D(static_cast<float>(Point.X), static_cast<float>(Point.Y)));
	}

	FString BuildingTag, BuildingPart, Landuse, Leisure, Natural, WaterTag, Waterway;
	TryGetScalarString(Properties, TEXT("building"), BuildingTag);
	TryGetScalarString(Properties, TEXT("building:part"), BuildingPart);
	TryGetScalarString(Properties, TEXT("landuse"), Landuse);
	TryGetScalarString(Properties, TEXT("leisure"), Leisure);
	TryGetScalarString(Properties, TEXT("natural"), Natural);
	TryGetScalarString(Properties, TEXT("water"), WaterTag);
	TryGetScalarString(Properties, TEXT("waterway"), Waterway);
	BuildingTag = BuildingTag.ToLower();
	Landuse = Landuse.ToLower(); Leisure = Leisure.ToLower(); Natural = Natural.ToLower();

	const bool bIsVegetation =
		Landuse == TEXT("grass") || Landuse == TEXT("forest") || Landuse == TEXT("orchard") ||
		Landuse == TEXT("vineyard") || Landuse == TEXT("meadow") ||
		Leisure == TEXT("park") || Leisure == TEXT("garden") ||
		Natural == TEXT("wood") || Natural == TEXT("scrub") || Natural == TEXT("heath");
	const bool bIsWater = !WaterTag.IsEmpty() || !Waterway.IsEmpty() || Natural == TEXT("water") ||
		Landuse == TEXT("reservoir") || Landuse == TEXT("basin");
	const bool bIsBuilding = !BuildingPart.IsEmpty() ||
		(!BuildingTag.IsEmpty() && BuildingTag != TEXT("no") && BuildingTag != TEXT("construction") &&
		 BuildingTag != TEXT("proposed") && BuildingTag != TEXT("demolished"));

	if (bIsVegetation)
	{
		FGeoVegetation Veg;
		Veg.Points = Nodes; Veg.RawLonLat = RawNodes; Veg.HolesRaw = RawHoles;
		TryGetScalarString(Properties, TEXT("name"), Veg.Name);
		Veg.VegType = !Landuse.IsEmpty() ? Landuse : (!Leisure.IsEmpty() ? Leisure : Natural);
		Veg.Color = Veg.VegType == TEXT("forest") || Veg.VegType == TEXT("wood")
			? FLinearColor(0.1f, 0.4f, 0.1f, 1.0f) : FLinearColor(0.2f, 0.55f, 0.15f, 1.0f);
		Vegetations.Add(MoveTemp(Veg));
	}
	else if (bIsWater)
	{
		ProcessWaterFeature(Nodes, RawNodes, Properties);
		Waters.Last().HolesRaw = MoveTemp(RawHoles);
	}
	else if (bIsBuilding)
	{
		ProcessBuildingFeature(Nodes, RawNodes, Properties);
		Buildings.Last().HolesRaw = MoveTemp(RawHoles);
	}
}

void FGeoJSONLoader::ParseMultiPolygon(const TArray<TSharedPtr<FJsonValue>>& Coordinates, const TSharedPtr<FJsonObject>& Properties)
{
	for (const TSharedPtr<FJsonValue>& PolygonValue : Coordinates)
	{
		if (PolygonValue.IsValid() && PolygonValue->Type == EJson::Array) ParsePolygon(PolygonValue->AsArray(), Properties);
	}
}

void FGeoJSONLoader::ApplyBuildingProperties(FGeoBuilding& Building, const TSharedPtr<FJsonObject>& Properties)
{
	if (!Properties.IsValid()) return;
	FString Value;
	double Number = 0.0;

	// Building colour
	if (TryGetScalarString(Properties, TEXT("building:colour"), Value))
	{
		Building.BuildingColour = FOSMColorParser::Parse(Value, Building.Color);
		Building.Color = Building.BuildingColour;
		Building.bHasExplicitBuildingColour = true;
	}

	// Building material (affects facade type)
	if (TryGetScalarString(Properties, TEXT("building:material"), Value))
		Building.FacadeMaterialType = FOSMBuildingMaterialParser::Parse(Value);

	// Roof shape
	if (TryGetScalarString(Properties, TEXT("roof:shape"), Value) ||
		TryGetScalarString(Properties, TEXT("building:roof:shape"), Value))
		Building.RoofShape = ParseRoofShape(Value);

	// Roof colour
	if (TryGetScalarString(Properties, TEXT("roof:colour"), Value) ||
		TryGetScalarString(Properties, TEXT("building:roof:colour"), Value))
	{
		Building.RoofColor = FOSMColorParser::Parse(Value, Building.RoofColor);
		Building.bHasExplicitRoofColor = true;
	}

	// Roof height
	if (TryGetMeters(Properties, TEXT("roof:height"), Number))
		Building.RoofHeight = Number;

	// Roof levels
	if (TryGetFiniteNumber(Properties, TEXT("roof:levels"), Number))
		Building.RoofLevels = FMath::Max(0, FMath::RoundToInt(Number));

	// Roof angle (used to estimate roof height if roof:height not set)
	if (TryGetFiniteNumber(Properties, TEXT("roof:angle"), Number))
		Building.RoofAngle = Number;

	// Roof direction (degrees: 0=North, 90=East, etc.)
	if (TryGetFiniteNumber(Properties, TEXT("roof:direction"), Number))
		Building.RoofDirection = Number;

	// Roof orientation
	TryGetScalarString(Properties, TEXT("roof:orientation"), Building.RoofOrientation);

	// Roof material
	TryGetScalarString(Properties, TEXT("roof:material"), Building.RoofMaterial);

	// Min height
	if (TryGetMeters(Properties, TEXT("min_height"), Number))
		Building.MinHeight = Number;

	// Estimate roof height from angle if not explicitly set
	if (Building.RoofHeight <= 0.0 && Building.RoofAngle > 0.0 && Building.Nodes.Num() > 0)
	{
		// Compute building half-width for roof height estimation
		float MinX = FLT_MAX, MaxX = -FLT_MAX, MinY = FLT_MAX, MaxY = -FLT_MAX;
		for (const FVector2D& N : Building.Nodes)
		{
			if (N.X < MinX) MinX = N.X;
			if (N.X > MaxX) MaxX = N.X;
			if (N.Y < MinY) MinY = N.Y;
			if (N.Y > MaxY) MaxY = N.Y;
		}
		float HalfWidth = FMath::Min(MaxX - MinX, MaxY - MinY) * 0.5f;
		Building.RoofHeight = HalfWidth * FMath::Tan(FMath::DegreesToRadians((float)Building.RoofAngle));
	}

	// Default roof height for non-flat roofs if still not set
	if (Building.RoofHeight <= 0.0 && Building.RoofShape != ERoofShape::Flat)
	{
		Building.RoofHeight = 3.0; // Default 3m roof for non-flat shapes
	}
}

void FGeoJSONLoader::ProcessBuildingFeature(const TArray<FVector2D>& Nodes, const TArray<FDoublePoint2D>& RawCoords, const TSharedPtr<FJsonObject>& Properties)
{
	FGeoBuilding Building; Building.Nodes = Nodes; Building.RawLonLat = RawCoords;
	float CenterX = 0.f, CenterY = 0.f;
	for (const FVector2D& N : Nodes) { CenterX += N.X; CenterY += N.Y; }
	if (Nodes.Num() > 0) Building.Center = FVector2D(CenterX / Nodes.Num(), CenterY / Nodes.Num());

	if (Properties.IsValid())
	{
		TryGetScalarString(Properties, TEXT("name"), Building.Name);
		double Levels = 0.0;
		if (TryGetFiniteNumber(Properties, TEXT("building:levels"), Levels))
			Building.Levels = FMath::Max(0, FMath::RoundToInt(Levels));
		double NumericHeight = 0.0;
		FString HeightText;
		if (Properties->TryGetNumberField(TEXT("height"), NumericHeight) || Properties->TryGetNumberField(TEXT("est_height"), NumericHeight))
			Building.Height = NumericHeight;
		else if (Properties->TryGetStringField(TEXT("height"), HeightText) || Properties->TryGetStringField(TEXT("est_height"), HeightText))
			Building.Height = FOSMTagHelpers::ParseMeters(HeightText, 0.0);
		if (Building.Height <= 0.0)
		{
			const int32 Lv = Building.Levels > 0 ? Building.Levels : 1;
			if (Building.Levels <= 0) Building.Levels = 1;
			Building.Height = Lv * FOSMTagHelpers::LevelHeightMeters;
		}
		if (TryGetScalarString(Properties, TEXT("building"), Building.BuildingTypeStr))
		{
			// Map common building types to numeric codes
			FString BLower = Building.BuildingTypeStr.ToLower();
			if (BLower == TEXT("residential") || BLower == TEXT("house") || BLower == TEXT("apartments"))
				Building.BuildingType = 0;
			else if (BLower == TEXT("commercial") || BLower == TEXT("office") || BLower == TEXT("retail"))
				Building.BuildingType = 1;
			else if (BLower == TEXT("industrial") || BLower == TEXT("warehouse"))
				Building.BuildingType = 2;
			else
				Building.BuildingType = 3; // other
		}

		// Apply roof and building colour properties
		ApplyBuildingProperties(Building, Properties);
	}
	Building.Color = GetColorFromType(Building.BuildingTypeStr, TEXT("building"));

	// If explicit building colour was set, override type-based color
	if (Building.bHasExplicitBuildingColour)
		Building.Color = Building.BuildingColour;

	Buildings.Add(Building);
}

void FGeoJSONLoader::ProcessRoadFeature(const TArray<FVector2D>& Points, const TArray<FDoublePoint2D>& RawCoords, const TSharedPtr<FJsonObject>& Properties)
{
	FGeoRoad Road; Road.Points = Points; Road.RawLonLat = RawCoords;
	if (Properties.IsValid() && TryGetScalarString(Properties, TEXT("highway"), Road.RoadTypeStr))
	{
		// Map road types to numeric codes
		FString RLower = Road.RoadTypeStr.ToLower();
		if (RLower == TEXT("residential") || RLower == TEXT("living_street")) Road.RoadType = 0;
		else if (RLower == TEXT("primary") || RLower == TEXT("motorway") || RLower == TEXT("trunk")) Road.RoadType = 1;
		else if (RLower == TEXT("secondary")) Road.RoadType = 2;
		else if (RLower == TEXT("tertiary")) Road.RoadType = 3;
		else if (RLower == TEXT("service") || RLower == TEXT("unclassified")) Road.RoadType = 4;
		else Road.RoadType = 5;

		// Road widths in meters
		if (Road.RoadTypeStr == TEXT("motorway"))            Road.Width = 24.0f;
		else if (Road.RoadTypeStr == TEXT("motorway_link"))   Road.Width = 12.0f;
		else if (Road.RoadTypeStr == TEXT("trunk"))           Road.Width = 20.0f;
		else if (Road.RoadTypeStr == TEXT("trunk_link"))      Road.Width = 12.0f;
		else if (Road.RoadTypeStr == TEXT("primary"))         Road.Width = 14.0f;
		else if (Road.RoadTypeStr == TEXT("primary_link"))    Road.Width = 10.0f;
		else if (Road.RoadTypeStr == TEXT("secondary"))       Road.Width = 12.0f;
		else if (Road.RoadTypeStr == TEXT("secondary_link"))  Road.Width = 10.0f;
		else if (Road.RoadTypeStr == TEXT("tertiary"))        Road.Width = 10.0f;
		else if (Road.RoadTypeStr == TEXT("residential"))     Road.Width = 8.0f;
		else if (Road.RoadTypeStr == TEXT("service"))         Road.Width = 5.0f;
		else                                                Road.Width = 8.0f;

		Road.bIsHighway = (Road.RoadTypeStr == TEXT("motorway") || Road.RoadTypeStr == TEXT("trunk") ||
				   Road.RoadTypeStr == TEXT("primary") || Road.RoadTypeStr == TEXT("secondary") ||
				   Road.RoadTypeStr == TEXT("tertiary") ||
				   Road.RoadTypeStr == TEXT("motorway_link") || Road.RoadTypeStr == TEXT("trunk_link") ||
				   Road.RoadTypeStr == TEXT("primary_link") || Road.RoadTypeStr == TEXT("secondary_link"));
	}
	TryGetScalarString(Properties, TEXT("name"), Road.Name);
	FString Width;
	if (TryGetScalarString(Properties, TEXT("width"), Width))
	{
		const float ParsedWidth = static_cast<float>(FOSMTagHelpers::ParseMeters(Width, 0.0));
		if (ParsedWidth > 0.0f) Road.Width = ParsedWidth;
	}

	Road.Color = GetColorFromType(Road.RoadTypeStr, TEXT("road"));
	Roads.Add(Road);
}

void FGeoJSONLoader::ProcessWaterFeature(const TArray<FVector2D>& Points, const TArray<FDoublePoint2D>& RawCoords, const TSharedPtr<FJsonObject>& Properties, bool bIsClosed)
{
	FGeoWater Water; Water.Points = Points; Water.RawLonLat = RawCoords;
	TryGetScalarString(Properties, TEXT("name"), Water.Name);
	Water.Color = FLinearColor::Blue;
	Water.bIsClosed = bIsClosed;
	if (!bIsClosed)
	{
		// River/stream/canal LineString — needs a real width so GenerateWaterMeshes
		// can build a ribbon instead of trying to fill an open line as a polygon.
		float Width = 4.f;
		FString WidthText;
		if (TryGetScalarString(Properties, TEXT("width"), WidthText))
		{
			Width = static_cast<float>(FOSMTagHelpers::ParseMeters(WidthText, Width));
		}
		else if (Properties.IsValid())
		{
			FString WW;
			if (TryGetScalarString(Properties, TEXT("waterway"), WW))
				Width = (WW == TEXT("river")) ? 8.f : (WW == TEXT("canal")) ? 6.f : 3.f;
		}
		Water.Width = FMath::Max(Width, 0.5f);
	}
	Waters.Add(Water);
}

void FGeoJSONLoader::ProcessPOIFeature(const FVector2D& Location, const FDoublePoint2D& RawCoord, const TSharedPtr<FJsonObject>& Properties)
{
	FGeoPOI POI; POI.Location = Location; POI.RawLonLat = RawCoord;
	TryGetScalarString(Properties, TEXT("name"), POI.Name);
	POIs.Add(POI);
}

void FGeoJSONLoader::ProcessRailwayFeature(const TArray<FVector2D>& Points, const TArray<FDoublePoint2D>& RawCoords, const TSharedPtr<FJsonObject>& Properties)
{
	FGeoRailway Railway; Railway.Points = Points; Railway.RawLonLat = RawCoords;
	if (Properties.IsValid())
	{
		TryGetScalarString(Properties, TEXT("name"), Railway.Name);
		TryGetScalarString(Properties, TEXT("railway"), Railway.RailwayType);
		double Value = 0.0;
		if (TryGetFiniteNumber(Properties, TEXT("gauge"), Value) && Value > 0.0) Railway.Gauge = static_cast<float>(Value);
		if (TryGetFiniteNumber(Properties, TEXT("tracks"), Value) && Value > 0.0) Railway.Tracks = FMath::RoundToInt(Value);
	}
	Railways.Add(Railway);
}

void FGeoJSONLoader::CalculateCenterPoint()
{
	CenterLatitude = (MinLat + MaxLat) / 2.0; CenterLongitude = (MinLon + MaxLon) / 2.0;
}

void FGeoJSONLoader::ConvertAllCoordsToLocal()
{
	// Use double-precision RawLonLat when available (avoids float precision loss),
	// otherwise fall back to float FVector2D (shouldn't happen for GeoJSON data).
	for (FGeoBuilding& B : Buildings)
	{
		if (B.RawLonLat.Num() > 0)
		{
			B.Nodes.Empty();
			for (const FDoublePoint2D& RL : B.RawLonLat)
				B.Nodes.Add(LatLonToLocalCoords(RL.Y, RL.X, CenterLatitude, CenterLongitude));
		}
		else
		{
			for (FVector2D& N : B.Nodes) N = LatLonToLocalCoords(N.Y, N.X, CenterLatitude, CenterLongitude);
		}
		for (const TArray<FDoublePoint2D>& RawRing : B.HolesRaw)
		{
			TArray<FVector2D>& Ring = B.Holes.AddDefaulted_GetRef();
			for (const FDoublePoint2D& Point : RawRing)
				Ring.Add(LatLonToLocalCoords(Point.Y, Point.X, CenterLatitude, CenterLongitude));
		}
		// Recalculate Center from the now-converted local nodes
		if (B.Nodes.Num() > 0)
		{
			float CX = 0.0f, CY = 0.0f;
			for (const FVector2D& N : B.Nodes) { CX += N.X; CY += N.Y; }
			B.Center = FVector2D(CX / B.Nodes.Num(), CY / B.Nodes.Num());
		}
	}
	for (FGeoRoad& R : Roads)
	{
		if (R.RawLonLat.Num() > 0)
		{
			R.Points.Empty();
			for (const FDoublePoint2D& RL : R.RawLonLat)
				R.Points.Add(LatLonToLocalCoords(RL.Y, RL.X, CenterLatitude, CenterLongitude));
		}
		else
		{
			for (FVector2D& P : R.Points) P = LatLonToLocalCoords(P.Y, P.X, CenterLatitude, CenterLongitude);
		}
	}
	for (FGeoWater& W : Waters)
	{
		if (W.RawLonLat.Num() > 0)
		{
			W.Points.Empty();
			for (const FDoublePoint2D& RL : W.RawLonLat)
				W.Points.Add(LatLonToLocalCoords(RL.Y, RL.X, CenterLatitude, CenterLongitude));
		}
		else
		{
			for (FVector2D& P : W.Points) P = LatLonToLocalCoords(P.Y, P.X, CenterLatitude, CenterLongitude);
		}
		W.Holes.Reset();
		for (const TArray<FDoublePoint2D>& RawRing : W.HolesRaw)
		{
			TArray<FVector2D>& Ring = W.Holes.AddDefaulted_GetRef();
			for (const FDoublePoint2D& Point : RawRing)
				Ring.Add(LatLonToLocalCoords(Point.Y, Point.X, CenterLatitude, CenterLongitude));
		}
	}
	for (FGeoVegetation& V : Vegetations)
	{
		if (V.RawLonLat.Num() > 0)
		{
			V.Points.Empty();
			for (const FDoublePoint2D& RL : V.RawLonLat)
				V.Points.Add(LatLonToLocalCoords(RL.Y, RL.X, CenterLatitude, CenterLongitude));
		}
		else
		{
			for (FVector2D& P : V.Points) P = LatLonToLocalCoords(P.Y, P.X, CenterLatitude, CenterLongitude);
		}
		V.Holes.Reset();
		for (const TArray<FDoublePoint2D>& RawRing : V.HolesRaw)
		{
			TArray<FVector2D>& Ring = V.Holes.AddDefaulted_GetRef();
			for (const FDoublePoint2D& Point : RawRing)
				Ring.Add(LatLonToLocalCoords(Point.Y, Point.X, CenterLatitude, CenterLongitude));
		}
	}
	for (FGeoPOI& P : POIs)
	{
		P.Location = LatLonToLocalCoords(P.RawLonLat.Y, P.RawLonLat.X, CenterLatitude, CenterLongitude);
	}
	for (FGeoTree& T : Trees)
	{
		T.Location = LatLonToLocalCoords(T.RawLonLat.Y, T.RawLonLat.X, CenterLatitude, CenterLongitude);
	}
	for (FGeoRailway& R : Railways)
	{
		if (R.RawLonLat.Num() > 0)
		{
			R.Points.Empty();
			for (const FDoublePoint2D& RL : R.RawLonLat)
				R.Points.Add(LatLonToLocalCoords(RL.Y, RL.X, CenterLatitude, CenterLongitude));
		}
		else
		{
			for (FVector2D& P : R.Points) P = LatLonToLocalCoords(P.Y, P.X, CenterLatitude, CenterLongitude);
		}
	}
	for (FElevationSample& E : ElevationSamples)
	{
		E.Location = LatLonToLocalCoords(E.RawLonLat.Y, E.RawLonLat.X, CenterLatitude, CenterLongitude);
	}
}

FVector2D FGeoJSONLoader::LatLonToLocalCoords(double Lat, double Lon, double CenterLat, double CenterLon)
{
	const double EarthRadiusM = 6371000.0;
	double X = EarthRadiusM * ((Lon - CenterLon) * UE_PI / 180.0) * FMath::Cos(CenterLat * UE_PI / 180.0);
	double Y = EarthRadiusM * ((Lat - CenterLat) * UE_PI / 180.0);
	return FVector2D(static_cast<float>(X), static_cast<float>(Y));
}

FLinearColor FGeoJSONLoader::GetColorFromType(const FString& Type, const FString& Category)
{
	if (Category == TEXT("building"))
	{
		if (Type == TEXT("residential") || Type == TEXT("apartments"))
			return FLinearColor(0.85f, 0.82f, 0.78f, 1.0f);
		else if (Type == TEXT("commercial") || Type == TEXT("office"))
			return FLinearColor(0.65f, 0.68f, 0.75f, 1.0f);
		else if (Type == TEXT("industrial"))
			return FLinearColor(0.6f, 0.6f, 0.55f, 1.0f);
		else if (Type == TEXT("retail") || Type == TEXT("shop"))
			return FLinearColor(0.8f, 0.75f, 0.7f, 1.0f);
		else
			return FLinearColor(0.75f, 0.75f, 0.75f, 1.0f);
	}
	else if (Category == TEXT("road"))
	{
		if (Type == TEXT("motorway") || Type == TEXT("trunk"))
			return FLinearColor(0.3f, 0.3f, 0.35f, 1.0f);
		else if (Type == TEXT("primary") || Type == TEXT("secondary"))
			return FLinearColor(0.35f, 0.35f, 0.35f, 1.0f);
		else
			return FLinearColor(0.4f, 0.4f, 0.4f, 1.0f);
	}

	return FLinearColor::Gray;
}
