// Copyright Hiro. All Rights Reserved.


#include "OSMLoader.h"
#include "XmlParser.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FOSMLoader::FOSMLoader()
        : CenterLatitude(0.0), CenterLongitude(0.0)
        , MinLat(DBL_MAX), MaxLat(-DBL_MAX), MinLon(DBL_MAX), MaxLon(-DBL_MAX)
        , bIsLoaded(false)
{}

FOSMLoader::~FOSMLoader() {}

void FOSMLoader::Reset()
{
        bIsLoaded = false;
        LastError.Reset();
        Buildings.Reset(); Roads.Reset(); Waters.Reset(); Vegetations.Reset();
        POIs.Reset(); Trees.Reset(); Railways.Reset(); Fences.Reset(); PowerLines.Reset();
        ModelInstances.Reset(); ElevationSamples.Reset();
        NodeCoords.Reset(); NodeRawCoords.Reset(); NodeTags.Reset(); WayMap.Reset();
        CenterLatitude = 0.0; CenterLongitude = 0.0;
        MinLat = DBL_MAX; MaxLat = -DBL_MAX; MinLon = DBL_MAX; MaxLon = -DBL_MAX;
}

// ============================================================================
// LoadOSMFile
// ============================================================================
bool FOSMLoader::LoadOSMFile(const FString& FilePath)
{
        Reset();
        FString FullPath = FPaths::IsRelative(FilePath) ? FPaths::ProjectDir() + FilePath : FilePath;
        if (!FPaths::FileExists(*FullPath))
        {
                LastError = TEXT("OSM file does not exist");
                return false;
        }

        FString FileContent;
        if (!FFileHelper::LoadFileToString(FileContent, *FullPath))
        {
                LastError = TEXT("Failed to read OSM file");
                return false;
        }

        return LoadOSMFromString(FileContent);
}

// ============================================================================
// LoadOSMFromString
// ============================================================================
bool FOSMLoader::LoadOSMFromString(const FString& OSMContent)
{
        Reset();

        // FXmlFile construction from an in-memory string buffer.
        // EConstructMethod has only two members in UE5: ConstructFromFile and
        // ConstructFromBuffer. Since OSMContent is the XML text (not a path),
        // we must use ConstructFromBuffer.
        FXmlFile XmlDocument(OSMContent, EConstructMethod::ConstructFromBuffer);
        if (!XmlDocument.IsValid())
        {
                LastError = TEXT("Failed to parse OSM XML");
                UE_LOG(LogTemp, Error, TEXT("OSMLoader: %s"), *LastError);
                return false;
        }

        FXmlNode* RootNode = XmlDocument.GetRootNode();
        if (!RootNode || RootNode->GetTag() != TEXT("osm"))
        {
                LastError = TEXT("Document root is not <osm>");
                return false;
        }

        // Phase 1: Parse all <node> elements (coordinates + tags)
        ParseNodes(RootNode);
        if (NodeRawCoords.Num() == 0 || MinLat == DBL_MAX || MinLon == DBL_MAX)
        {
                LastError = TEXT("OSM document contains no valid nodes");
                return false;
        }

        // Phase 2: Parse all <way> elements (roads, buildings, etc.)
        ParseWays(RootNode);

        // Phase 3: Parse all <relation> elements (multipolygon buildings, etc.)
        ParseRelations(RootNode);

        // Phase 3b: many rivers/lakes are mapped BOTH as an areal outline
        // (natural=water / a water multipolygon) AND a separate waterway=river
        // centerline for routing purposes. Rendering both produces a visible
        // double-water layer (a darker strip on top of the correct polygon).
        // Drop centerlines that are already covered by a closed water polygon.
        RemoveRedundantWaterwayCenterlines();

        // Phase 4: Assemble building:parts into composite buildings
        AssembleBuildingParts();

        const int32 FeatureCount = Buildings.Num() + Roads.Num() + Waters.Num() + Vegetations.Num() +
                POIs.Num() + Trees.Num() + Railways.Num() + Fences.Num() + PowerLines.Num() +
                ModelInstances.Num() + ElevationSamples.Num();
        if (FeatureCount == 0)
        {
                LastError = TEXT("OSM document contains no supported features");
                return false;
        }

        // Phase 5: Calculate center and convert coordinates
        CalculateCenterPoint();
        ConvertAllCoordsToLocal();

        bIsLoaded = true;
        UE_LOG(LogTemp, Log, TEXT("OSMLoader: Loaded B=%d R=%d W=%d V=%d Trees=%d Rail=%d"),
                Buildings.Num(), Roads.Num(), Waters.Num(), Vegetations.Num(),
                Trees.Num(), Railways.Num());

        return true;
}

// ============================================================================
// ParseNodes - Extract all <node> elements
// ============================================================================
void FOSMLoader::ParseNodes(const FXmlNode* RootNode)
{
        if (!RootNode) return;

        for (const FXmlNode* Child : RootNode->GetChildrenNodes())
        {
                if (Child->GetTag() != TEXT("node")) continue;

                // Extract lat/lon attributes
                const FString IdText = Child->GetAttribute(TEXT("id"));
                const FString LatText = Child->GetAttribute(TEXT("lat"));
                const FString LonText = Child->GetAttribute(TEXT("lon"));
                if (!IdText.IsNumeric() || !LatText.IsNumeric() || !LonText.IsNumeric()) continue;
                int64 NodeId = FCString::Atoi64(*IdText);
                double Lat = FCString::Atod(*LatText);
                double Lon = FCString::Atod(*LonText);
                if (!FMath::IsFinite(Lat) || !FMath::IsFinite(Lon) ||
                        Lat < -90.0 || Lat > 90.0 || Lon < -180.0 || Lon > 180.0 ||
                        NodeRawCoords.Contains(NodeId))
                        continue;

                // Update bounds
                if (Lat < MinLat) MinLat = Lat;
                if (Lat > MaxLat) MaxLat = Lat;
                if (Lon < MinLon) MinLon = Lon;
                if (Lon > MaxLon) MaxLon = Lon;

                // Store raw coordinates for later conversion
                NodeRawCoords.Add(NodeId, FDoublePoint2D(Lon, Lat));

                // Extract tags from this node (for POIs, trees, etc.)
                TMap<FString, FString> Tags = ExtractTags(Child);
                if (Tags.Num() > 0)
                {
                        NodeTags.Add(NodeId, Tags);

                        // OSM ele tag — used for terrain deformation (streets.gl terrain parity).
                        const FString* EleStr = Tags.Find(TEXT("ele"));
                        if (EleStr && EleStr->IsNumeric())
                        {
                                FElevationSample Sample;
                                Sample.RawLonLat = FDoublePoint2D(Lon, Lat);
                                Sample.Elevation = FCString::Atod(**EleStr);
                                ElevationSamples.Add(Sample);
                        }
                }
        }
}

// ============================================================================
// ParseWays - Extract all <way> elements
// ============================================================================
void FOSMLoader::ParseWays(const FXmlNode* RootNode)
{
        if (!RootNode) return;

        for (const FXmlNode* Child : RootNode->GetChildrenNodes())
        {
                if (Child->GetTag() != TEXT("way")) continue;

                int64 WayId = FCString::Atoi64(*Child->GetAttribute(TEXT("id")));
                TMap<FString, FString> Tags = ExtractTags(Child);

                // Collect node references
                TArray<int64> NodeRefs;
                for (const FXmlNode* NdChild : Child->GetChildrenNodes())
                {
                        if (NdChild->GetTag() == TEXT("nd"))
                        {
                                int64 Ref = FCString::Atoi64(*NdChild->GetAttribute(TEXT("ref")));
                                NodeRefs.Add(Ref);
                        }
                }

                // Store in WayMap for relation lookup
                FOSMWay& Way = WayMap.Add(WayId);
                Way.NodeRefs = NodeRefs;
                Way.Tags = Tags;

                if (NodeRefs.Num() < 2) continue;

                // Determine what kind of feature this way represents.
                // building=no/construction/proposed are NOT real buildings: they must
                // not be extruded (e.g. an "Alte Akademie" construction site that
                // streets.gl keeps flat). Such ways fall through to nothing.
                bool bHasBuildingTag = false;
                if (Tags.Contains(TEXT("building")))
                {
                        const FString BV = Tags[TEXT("building")].ToLower();
                        bHasBuildingTag = (BV != TEXT("no") && BV != TEXT("construction") &&
                                BV != TEXT("proposed") && BV != TEXT("demolished"));
                }
                bool bHasHighwayTag = Tags.Contains(TEXT("highway"));
                bool bHasRailwayTag = Tags.Contains(TEXT("railway"));
                bool bHasWaterTag = Tags.Contains(TEXT("water")) || Tags.Contains(TEXT("waterway")) ||
                        (Tags.Contains(TEXT("natural")) && Tags[TEXT("natural")] == TEXT("water")) ||
                        (Tags.Contains(TEXT("landuse")) &&
                                (Tags[TEXT("landuse")] == TEXT("reservoir") || Tags[TEXT("landuse")] == TEXT("basin")));
                bool bHasBuildingPartTag = Tags.Contains(TEXT("building:part"));
                bool bIsArea = Tags.Contains(TEXT("area")) && Tags[TEXT("area")] == TEXT("yes");

                // Check for natural/landuse/leisure (vegetation)
                bool bIsVegetation = false;
                if (Tags.Contains(TEXT("landuse")))
                {
                        FString Landuse = Tags[TEXT("landuse")];
                        if (Landuse == TEXT("grass") || Landuse == TEXT("forest") || Landuse == TEXT("orchard")
                                || Landuse == TEXT("vineyard") || Landuse == TEXT("meadow") || Landuse == TEXT("flowerbed")
                                || Landuse == TEXT("shrubs") || Landuse == TEXT("farmland") || Landuse == TEXT("farmyard")
                                // Bare ground surfaces — rendered as a flat AREA (brown, no
                                // grass/trees scattered) so a construction site reads as a dirt lot.
                                || Landuse == TEXT("construction") || Landuse == TEXT("brownfield")
                                || Landuse == TEXT("landfill"))
                                bIsVegetation = true;
                }
                if (!bIsVegetation && Tags.Contains(TEXT("leisure")))
                {
                        FString Leisure = Tags[TEXT("leisure")];
                        if (Leisure == TEXT("park") || Leisure == TEXT("garden") || Leisure == TEXT("pitch")
                                || Leisure == TEXT("playground") || Leisure == TEXT("golf_course"))
                                bIsVegetation = true;
                }
                if (!bIsVegetation && Tags.Contains(TEXT("natural")))
                {
                        FString Natural = Tags[TEXT("natural")];
                        if (Natural == TEXT("wood") || Natural == TEXT("scrub") || Natural == TEXT("heath")
                                || Natural == TEXT("sand") || Natural == TEXT("beach") || Natural == TEXT("rock")
                                || Natural == TEXT("bare_rock") || Natural == TEXT("scree"))
                                bIsVegetation = true;
                }

                const bool bIsTreeRow = Tags.Contains(TEXT("natural")) && Tags[TEXT("natural")] == TEXT("tree_row");
                const bool bIsFence =
                        Tags.Contains(TEXT("barrier")) &&
                        (Tags[TEXT("barrier")] == TEXT("fence") || Tags[TEXT("barrier")] == TEXT("wall") ||
                         Tags[TEXT("barrier")] == TEXT("hedge"));
                const bool bIsPowerLine =
                        Tags.Contains(TEXT("power")) &&
                        (Tags[TEXT("power")] == TEXT("line") || Tags[TEXT("power")] == TEXT("minor_line"));
                const bool bIsAeroway =
                        Tags.Contains(TEXT("aeroway")) &&
                        (Tags[TEXT("aeroway")] == TEXT("runway") || Tags[TEXT("aeroway")] == TEXT("taxiway"));

                // Route to appropriate processor
                if (bHasBuildingTag || bHasBuildingPartTag)
                {
                        ProcessBuildingWay(Child);
                }
                else if (bIsTreeRow)
                {
                        ProcessTreeRowWay(Child);
                }
                else if (bIsVegetation && NodeRefs.Num() >= 3)
                {
                        ProcessVegetationWay(Child);
                }
                else if (bHasWaterTag)
                {
                        ProcessWaterWay(Child);
                }
                else if (bIsFence)
                {
                        ProcessFenceWay(Child);
                }
                else if (bIsPowerLine)
                {
                        ProcessPowerLineWay(Child);
                }
                else if (bIsAeroway)
                {
                        ProcessAerowayWay(Child);
                }
                else if (bHasHighwayTag)
                {
                        ProcessRoadWay(Child);
                }
                else if (bHasRailwayTag)
                {
                        ProcessRailwayWay(Child);
                }
        }

        // Process POIs and trees from nodes
        for (const auto& Pair : NodeTags)
        {
                int64 NodeId = Pair.Key;
                const TMap<FString, FString>& Tags = Pair.Value;

                // Check for tree
                if (Tags.Contains(TEXT("natural")) && Tags[TEXT("natural")] == TEXT("tree"))
                {
                        const FDoublePoint2D* RawCoord = NodeRawCoords.Find(NodeId);
                        if (RawCoord)
                        {
                                FGeoTree Tree;
                                Tree.RawLonLat = *RawCoord;
                                Tree.Location = FVector2D(static_cast<float>(RawCoord->X), static_cast<float>(RawCoord->Y));
                                if (Tags.Contains(TEXT("species"))) Tree.Species = Tags[TEXT("species")];
                                if (Tags.Contains(TEXT("genus"))) Tree.Genus = Tags[TEXT("genus")];
                                if (Tags.Contains(TEXT("height")))
                                {
                                        Tree.Height = FCString::Atof(*Tags[TEXT("height")]);
                                        if (Tree.Height <= 0.f) Tree.Height = 8.f;
                                }
                                if (Tags.Contains(TEXT("crown_radius")))
                                {
                                        Tree.CrownRadius = FCString::Atof(*Tags[TEXT("crown_radius")]);
                                        if (Tree.CrownRadius <= 0.f) Tree.CrownRadius = 3.f;
                                }
                                Trees.Add(Tree);
                        }
                        continue;
                }

                // 3D model instances (street furniture, power, landmarks).
                // Mirrors streets-gl's OSMNodeQualifierFactory node->model mapping.
                {
                        auto Is = [&](const TCHAR* K, const TCHAR* V) -> bool
                        { const FString* P = Tags.Find(K); return P && (*P == V); };
                        auto ParseDir = [](const FString& S) -> float
                        {
                                if (S.IsNumeric()) return FCString::Atof(*S);
                                const FString U = S.ToUpper();
                                if (U == TEXT("N"))  return 0.f;   if (U == TEXT("NE")) return 45.f;
                                if (U == TEXT("E"))  return 90.f;  if (U == TEXT("SE")) return 135.f;
                                if (U == TEXT("S"))  return 180.f; if (U == TEXT("SW")) return 225.f;
                                if (U == TEXT("W"))  return 270.f; if (U == TEXT("NW")) return 315.f;
                                return -1.f;
                        };

                        FString ModelType;
                        float RotDeg = -1.f, Hgt = 0.f;

                        if (Is(TEXT("emergency"), TEXT("fire_hydrant")))
                        {
                                const FString* HT = Tags.Find(TEXT("fire_hydrant:type"));
                                if (!HT || *HT == TEXT("pillar")) ModelType = TEXT("hydrant");
                        }
                        else if (Is(TEXT("advertising"), TEXT("column"))) ModelType = TEXT("ad_column");
                        // Construction tower cranes (man_made=crane, usually crane:type=tower_crane).
                        else if (Is(TEXT("man_made"), TEXT("crane")))     ModelType = TEXT("tower_crane");
                        else if (Is(TEXT("power"), TEXT("tower")))        ModelType = TEXT("transmission_tower");
                        else if (Is(TEXT("power"), TEXT("pole")))         ModelType = TEXT("utility_pole");
                        else if (Is(TEXT("amenity"), TEXT("bench")))
                        {
                                ModelType = TEXT("bench");
                                const FString* Dir = Tags.Find(TEXT("direction"));
                                if (Dir) RotDeg = ParseDir(*Dir);
                        }
                        else if (Is(TEXT("leisure"), TEXT("picnic_table"))) ModelType = TEXT("picnic_table");
                        else if (Is(TEXT("highway"), TEXT("bus_stop")))     ModelType = TEXT("bus_stop");
                        else if (Is(TEXT("power"), TEXT("generator")) && Is(TEXT("generator:source"), TEXT("wind")))
                        {
                                ModelType = TEXT("wind_turbine");
                                const FString* H = Tags.Find(TEXT("height"));
                                if (H) Hgt = FCString::Atof(**H);
                        }
                        else if (Is(TEXT("historic"), TEXT("memorial")))
                        {
                                const FString* M = Tags.Find(TEXT("memorial"));
                                const FString MV = M ? M->ToLower() : FString();
                                if (MV.Contains(TEXT("statue")))         ModelType = TEXT("statue_0");
                                else if (MV.Contains(TEXT("sculpture"))) ModelType = TEXT("sculpture");
                                else                                     ModelType = TEXT("memorial");
                        }
                        else if (Is(TEXT("tourism"), TEXT("artwork")))
                        {
                                const FString* A = Tags.Find(TEXT("artwork_type"));
                                const FString AV = A ? A->ToLower() : FString();
                                if (AV.Contains(TEXT("statue")))         ModelType = TEXT("statue_1");
                                else if (AV.Contains(TEXT("sculpture"))) ModelType = TEXT("sculpture");
                        }

                        if (!ModelType.IsEmpty())
                        {
                                const FDoublePoint2D* RawCoord = NodeRawCoords.Find(NodeId);
                                if (RawCoord)
                                {
                                        FGeoModelInstance MI;
                                        MI.RawLonLat = *RawCoord;
                                        MI.Location = FVector2D((float)RawCoord->X, (float)RawCoord->Y);
                                        MI.ModelType = ModelType;
                                        MI.RotationDeg = RotDeg;
                                        MI.Height = Hgt;
                                        ModelInstances.Add(MI);
                                }
                                continue; // handled as a model; don't also add as a generic POI
                        }
                }

                // Other POIs
                if (Tags.Contains(TEXT("amenity")) || Tags.Contains(TEXT("shop")) ||
                        Tags.Contains(TEXT("tourism")) || Tags.Contains(TEXT("office")) ||
                        Tags.Contains(TEXT("public_transport")))
                {
                        const FDoublePoint2D* RawCoord = NodeRawCoords.Find(NodeId);
                        if (RawCoord)
                        {
                                FGeoPOI POI;
                                POI.RawLonLat = *RawCoord;
                                POI.Location = FVector2D(static_cast<float>(RawCoord->X), static_cast<float>(RawCoord->Y));
                                if (Tags.Contains(TEXT("name"))) POI.Name = Tags[TEXT("name")];
                                if (Tags.Contains(TEXT("amenity"))) { POI.POIType = TEXT("amenity"); POI.POISubType = Tags[TEXT("amenity")]; }
                                else if (Tags.Contains(TEXT("shop"))) { POI.POIType = TEXT("shop"); POI.POISubType = Tags[TEXT("shop")]; }
                                else if (Tags.Contains(TEXT("tourism"))) { POI.POIType = TEXT("tourism"); POI.POISubType = Tags[TEXT("tourism")]; }
                                POIs.Add(POI);
                        }
                }
        }
}

// ============================================================================
// ParseRelations - Extract <relation> elements (multipolygon buildings, etc.)
// ============================================================================
void FOSMLoader::ParseRelations(const FXmlNode* RootNode)
{
        if (!RootNode) return;

        for (const FXmlNode* Child : RootNode->GetChildrenNodes())
        {
                if (Child->GetTag() != TEXT("relation")) continue;

                TMap<FString, FString> Tags = ExtractTags(Child);
                if (Tags.Num() == 0) continue;

                // Check if this is a building relation (excluding non-building
                // building=no/construction/proposed, same as the way path).
                bool bBuildingVal = false;
                if (Tags.Contains(TEXT("building")))
                {
                        const FString BV = Tags[TEXT("building")].ToLower();
                        bBuildingVal = (BV != TEXT("no") && BV != TEXT("construction") &&
                                BV != TEXT("proposed") && BV != TEXT("demolished"));
                }
                bool bIsBuildingRelation = bBuildingVal ||
                        (Tags.Contains(TEXT("type")) && Tags[TEXT("type")] == TEXT("building"));

                // Water multipolygon relation (e.g. a wide river or lake split into
                // several outer/inner member ways — tags usually live on the relation
                // itself, not on the individual member ways, so ProcessWaterWay's
                // per-way tag check never sees them without this).
                const bool bIsWaterRelation =
                        Tags.Contains(TEXT("water")) ||
                        (Tags.Contains(TEXT("natural")) && Tags[TEXT("natural")] == TEXT("water")) ||
                        Tags.Contains(TEXT("waterway")) ||
                        (Tags.Contains(TEXT("landuse")) &&
                                (Tags[TEXT("landuse")] == TEXT("reservoir") || Tags[TEXT("landuse")] == TEXT("basin")));
                const bool bIsVegetationRelation =
                        (Tags.Contains(TEXT("landuse")) &&
                                (Tags[TEXT("landuse")] == TEXT("grass") || Tags[TEXT("landuse")] == TEXT("forest") ||
                                 Tags[TEXT("landuse")] == TEXT("orchard") || Tags[TEXT("landuse")] == TEXT("vineyard") ||
                                 Tags[TEXT("landuse")] == TEXT("meadow"))) ||
                        (Tags.Contains(TEXT("leisure")) &&
                                (Tags[TEXT("leisure")] == TEXT("park") || Tags[TEXT("leisure")] == TEXT("garden"))) ||
                        (Tags.Contains(TEXT("natural")) &&
                                (Tags[TEXT("natural")] == TEXT("wood") || Tags[TEXT("natural")] == TEXT("scrub") ||
                                 Tags[TEXT("natural")] == TEXT("heath")));

                if (!bIsBuildingRelation && !bIsWaterRelation && !bIsVegetationRelation) continue;

                // Collect outer and inner member ways
                TArray<int64> OuterWayIds, InnerWayIds;
                for (const FXmlNode* MemberNode : Child->GetChildrenNodes())
                {
                        if (MemberNode->GetTag() != TEXT("member")) continue;
                        FString Role = MemberNode->GetAttribute(TEXT("role"));
                        int64 Ref = FCString::Atoi64(*MemberNode->GetAttribute(TEXT("ref")));
                        if (Role == TEXT("outer") || Role == TEXT("outline"))
                                OuterWayIds.Add(Ref);
                        else if (Role == TEXT("inner"))
                                InnerWayIds.Add(Ref);
                }

                if (bIsWaterRelation || bIsVegetationRelation)
                {
                        // Chain outer member ways into one or more CLOSED rings by
                        // matching shared endpoint node IDs. A relation can contain
                        // more than one disjoint outer ring (e.g. the main channel
                        // plus an unconnected pond); naively concatenating all of
                        // them (old behaviour) bridges unrelated shapes with a bogus
                        // edge that cuts straight across the map. Each closed chain
                        // here becomes its own separate water polygon.
                        // (Inner rings — piers/islands — are still not cut out; the
                        // water renders underneath them, closer to correct than the
                        // water body being missing entirely.)
                        TArray<FOSMWay*> OuterWays;
                        for (int64 WayId : OuterWayIds)
                        {
                                FOSMWay* Way = WayMap.Find(WayId);
                                if (Way && Way->NodeRefs.Num() >= 2)
                                        OuterWays.Add(Way);
                        }

                        TArray<bool> Used;
                        Used.Init(false, OuterWays.Num());

                        TArray<TArray<FDoublePoint2D>> InnerRings;
                        for (int64 InnerId : InnerWayIds)
                        {
                                FOSMWay* InnerWay = WayMap.Find(InnerId);
                                if (!InnerWay) continue;
                                TArray<FVector2D> IgnorePoints;
                                TArray<FDoublePoint2D> InnerRaw;
                                if (!ResolveNodeRefs(InnerWay->NodeRefs, IgnorePoints, InnerRaw)) continue;
                                if (InnerRaw.Num() > 1 && InnerRaw[0].X == InnerRaw.Last().X && InnerRaw[0].Y == InnerRaw.Last().Y)
                                        InnerRaw.Pop();
                                if (InnerRaw.Num() >= 3) InnerRings.Add(MoveTemp(InnerRaw));
                        }

                        for (int32 StartIdx = 0; StartIdx < OuterWays.Num(); StartIdx++)
                        {
                                if (Used[StartIdx]) continue;

                                TArray<int64> Chain = OuterWays[StartIdx]->NodeRefs;
                                Used[StartIdx] = true;

                                bool bExtended = true;
                                while (bExtended && (Chain.Num() < 2 || Chain[0] != Chain.Last()))
                                {
                                        bExtended = false;
                                        for (int32 i = 0; i < OuterWays.Num(); i++)
                                        {
                                                if (Used[i]) continue;
                                                const TArray<int64>& WNodes = OuterWays[i]->NodeRefs;
                                                if (WNodes.Num() < 2) continue;

                                                if (WNodes[0] == Chain.Last())
                                                {
                                                        for (int32 k = 1; k < WNodes.Num(); k++) Chain.Add(WNodes[k]);
                                                        Used[i] = true; bExtended = true; break;
                                                }
                                                if (WNodes.Last() == Chain.Last())
                                                {
                                                        for (int32 k = WNodes.Num() - 2; k >= 0; k--) Chain.Add(WNodes[k]);
                                                        Used[i] = true; bExtended = true; break;
                                                }
                                                if (WNodes.Last() == Chain[0])
                                                {
                                                        TArray<int64> NewChain;
                                                        for (int32 k = 0; k < WNodes.Num() - 1; k++) NewChain.Add(WNodes[k]);
                                                        NewChain.Append(Chain);
                                                        Chain = MoveTemp(NewChain);
                                                        Used[i] = true; bExtended = true; break;
                                                }
                                                if (WNodes[0] == Chain[0])
                                                {
                                                        TArray<int64> NewChain;
                                                        for (int32 k = WNodes.Num() - 1; k >= 1; k--) NewChain.Add(WNodes[k]);
                                                        NewChain.Append(Chain);
                                                        Chain = MoveTemp(NewChain);
                                                        Used[i] = true; bExtended = true; break;
                                                }
                                        }
                                }

                                TArray<FVector2D> RingPoints;
                                TArray<FDoublePoint2D> RingRawPoints;
                                if (!ResolveNodeRefs(Chain, RingPoints, RingRawPoints)) continue;
                                if (RingPoints.Num() > 1 && RingPoints[0].Equals(RingPoints.Last(), 1e-6f))
                                {
                                        RingPoints.Pop();
                                        RingRawPoints.Pop();
                                }
                                if (RingPoints.Num() < 3) continue;

                                auto IsInsideOuter = [&RingRawPoints](const FDoublePoint2D& P) -> bool
                                {
                                        bool bInside = false;
                                        for (int32 i = 0, j = RingRawPoints.Num() - 1; i < RingRawPoints.Num(); j = i++)
                                        {
                                                const FDoublePoint2D& A = RingRawPoints[i];
                                                const FDoublePoint2D& B = RingRawPoints[j];
                                                if (((A.Y > P.Y) != (B.Y > P.Y)) &&
                                                        P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y + 1.e-12) + A.X)
                                                        bInside = !bInside;
                                        }
                                        return bInside;
                                };
                                TArray<TArray<FDoublePoint2D>> AssignedHoles;
                                for (const TArray<FDoublePoint2D>& Inner : InnerRings)
                                        if (Inner.Num() >= 3 && IsInsideOuter(Inner[0]))
                                                AssignedHoles.Add(Inner);

                                if (bIsWaterRelation)
                                {
                                        FGeoWater Water;
                                        Water.Points = RingPoints;
                                        Water.RawLonLat = RingRawPoints;
                                        Water.HolesRaw = AssignedHoles;
                                        Water.bIsClosed = true;
                                        if (Tags.Contains(TEXT("name"))) Water.Name = Tags[TEXT("name")];
                                        Waters.Add(MoveTemp(Water));
                                }
                                else
                                {
                                        FGeoVegetation Veg;
                                        Veg.Points = RingPoints;
                                        Veg.RawLonLat = RingRawPoints;
                                        Veg.HolesRaw = AssignedHoles;
                                        if (Tags.Contains(TEXT("name"))) Veg.Name = Tags[TEXT("name")];
                                        if (Tags.Contains(TEXT("landuse"))) Veg.VegType = Tags[TEXT("landuse")];
                                        else if (Tags.Contains(TEXT("leisure"))) Veg.VegType = Tags[TEXT("leisure")];
                                        else Veg.VegType = Tags.FindRef(TEXT("natural"));
                                        Veg.Color = (Veg.VegType == TEXT("forest") || Veg.VegType == TEXT("wood"))
                                                ? FLinearColor(0.1f, 0.4f, 0.1f, 1.f)
                                                : FLinearColor(0.2f, 0.55f, 0.15f, 1.f);
                                        Vegetations.Add(MoveTemp(Veg));
                                }
                        }
                        continue;
                }

                // Resolve outer rings into a single building
                if (OuterWayIds.Num() > 0)
                {
                        TArray<FVector2D> AllNodes;
                        TArray<FDoublePoint2D> AllRawNodes;

                        for (int64 WayId : OuterWayIds)
                        {
                                FOSMWay* Way = WayMap.Find(WayId);
                                if (!Way) continue;

                                TArray<FVector2D> WayPoints;
                                TArray<FDoublePoint2D> WayRawPoints;
                                if (ResolveNodeRefs(Way->NodeRefs, WayPoints, WayRawPoints))
                                {
                                        // Skip last point if it equals the first (closed way)
                                        if (WayPoints.Num() > 1 && WayPoints[0].Equals(WayPoints.Last(), 1e-6f))
                                        {
                                                WayPoints.Pop();
                                                WayRawPoints.Pop();
                                        }
                                        AllNodes.Append(WayPoints);
                                        AllRawNodes.Append(WayRawPoints);
                                }
                        }

                        if (AllNodes.Num() >= 3)
                        {
                                FGeoBuilding Building;
                                Building.Nodes = AllNodes;
                                Building.RawLonLat = AllRawNodes;

                                // Resolve inner rings (courtyards / holes).
                                for (int64 InnerId : InnerWayIds)
                                {
                                        FOSMWay* IW = WayMap.Find(InnerId);
                                        if (!IW) continue;
                                        TArray<FVector2D> HP;
                                        TArray<FDoublePoint2D> HRP;
                                        if (ResolveNodeRefs(IW->NodeRefs, HP, HRP))
                                        {
                                                if (HRP.Num() > 1 && HRP[0].X == HRP.Last().X && HRP[0].Y == HRP.Last().Y)
                                                        HRP.Pop();
                                                if (HRP.Num() >= 3)
                                                        Building.HolesRaw.Add(HRP);
                                        }
                                }

                                // Calculate center
                                float CX = 0.f, CY = 0.f;
                                for (const FVector2D& N : AllNodes) { CX += N.X; CY += N.Y; }
                                Building.Center = FVector2D(CX / AllNodes.Num(), CY / AllNodes.Num());

                                ApplyBuildingTags(Building, Tags);
                                Buildings.Add(Building);
                        }
                }
        }
}

// ============================================================================
// RemoveRedundantWaterwayCenterlines
//
// Many rivers/lakes are mapped BOTH as a closed areal outline (natural=water,
// or a water multipolygon) AND a separate open waterway=river/stream centerline
// running through the middle of that same area for routing purposes. Rendering
// both produces a visible double-water layer. A centerline whose points mostly
// fall inside an already-collected closed water polygon is redundant and dropped.
// ============================================================================
static bool FOSMLoader_PointInPolygon(const FVector2D& P, const TArray<FVector2D>& Poly)
{
        bool bInside = false;
        const int32 N = Poly.Num();
        for (int32 i = 0, j = N - 1; i < N; j = i++)
        {
                const FVector2D& A = Poly[i];
                const FVector2D& B = Poly[j];
                if (((A.Y > P.Y) != (B.Y > P.Y)) &&
                        (P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y + KINDA_SMALL_NUMBER) + A.X))
                {
                        bInside = !bInside;
                }
        }
        return bInside;
}

void FOSMLoader::RemoveRedundantWaterwayCenterlines()
{
        TArray<const FGeoWater*> ClosedWaters;
        for (const FGeoWater& W : Waters)
                if (W.bIsClosed && W.Points.Num() >= 3)
                        ClosedWaters.Add(&W);

        if (ClosedWaters.Num() == 0) return;

        TArray<FGeoWater> Kept;
        Kept.Reserve(Waters.Num());
        for (const FGeoWater& W : Waters)
        {
                if (W.bIsClosed || W.Points.Num() < 2)
                {
                        Kept.Add(W);
                        continue;
                }

                // Sample densely along the centerline (up to 12 evenly spaced
                // points, every point for short lines) and test each against
                // ALL closed water polygons combined (the real river may be
                // split across several polygon pieces/relations). If a third
                // or more of the samples land inside some closed water body,
                // this centerline is treated as a redundant duplicate of it.
                const int32 NP = W.Points.Num();
                const int32 NumSamples = FMath::Clamp(NP, 2, 12);
                int32 InsideCount = 0;
                for (int32 s = 0; s < NumSamples; s++)
                {
                        const int32 Idx = (NumSamples == 1) ? 0 : FMath::RoundToInt(
                                (float)s * (float)(NP - 1) / (float)(NumSamples - 1));
                        const FVector2D& Sample = W.Points[FMath::Clamp(Idx, 0, NP - 1)];

                        for (const FGeoWater* Closed : ClosedWaters)
                        {
                                if (FOSMLoader_PointInPolygon(Sample, Closed->Points))
                                {
                                        InsideCount++;
                                        break;
                                }
                        }
                }

                const bool bRedundant = (float)InsideCount / (float)NumSamples >= 0.34f;
                if (!bRedundant)
                        Kept.Add(W);
        }

        if (Kept.Num() != Waters.Num())
        {
                UE_LOG(LogTemp, Log, TEXT("OSMLoader: dropped %d redundant waterway centerline(s) already covered by a water polygon"),
                        Waters.Num() - Kept.Num());
        }
        Waters = MoveTemp(Kept);
}

// ============================================================================
// ProcessBuildingWay
// ============================================================================
void FOSMLoader::ProcessBuildingWay(const FXmlNode* WayNode)
{
        if (!WayNode) return;

        TMap<FString, FString> Tags = ExtractTags(WayNode);

        // Collect node references
        TArray<int64> NodeRefs;
        for (const FXmlNode* NdChild : WayNode->GetChildrenNodes())
        {
                if (NdChild->GetTag() == TEXT("nd"))
                        NodeRefs.Add(FCString::Atoi64(*NdChild->GetAttribute(TEXT("ref"))));
        }

        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawPoints;
        if (!ResolveNodeRefs(NodeRefs, Points, RawPoints)) return;

        // Remove closing point if it duplicates the first
        if (Points.Num() > 1 && Points[0].Equals(Points.Last(), 1e-6f))
        {
                Points.Pop();
                RawPoints.Pop();
        }

        if (Points.Num() < 3) return;

        FGeoBuilding Building;
        Building.Nodes = Points;
        Building.RawLonLat = RawPoints;

        // Check if this is a building:part
        if (Tags.Contains(TEXT("building:part")))
        {
                Building.bIsBuildingPart = true;
                Building.BuildingPartId = WayNode->GetAttribute(TEXT("id"));
                // building:part=tower is a free-standing tower (e.g. Alter Peter), NOT a
                // small roof turret. Towers must never have their column dropped to a
                // neighbour's eave nor their skillion cap stretched toward an adjacent dome.
                if (Tags[TEXT("building:part")].ToLower() == TEXT("tower"))
                        Building.bIsTowerPart = true;
        }

        // Calculate center
        float CX = 0.f, CY = 0.f;
        for (const FVector2D& N : Points) { CX += N.X; CY += N.Y; }
        Building.Center = FVector2D(CX / Points.Num(), CY / Points.Num());

        ApplyBuildingTags(Building, Tags);
        Buildings.Add(Building);
}

// ============================================================================
// ProcessRoadWay
// ============================================================================
void FOSMLoader::ProcessRoadWay(const FXmlNode* WayNode)
{
        if (!WayNode) return;

        TMap<FString, FString> Tags = ExtractTags(WayNode);
        // streets-gl isUnderground — skip tunnels / underground / negative level.
        {
                const FString Tunnel = Tags.FindRef(TEXT("tunnel")).ToLower();
                const FString Location = Tags.FindRef(TEXT("location")).ToLower();
                const float Level = Tags.Contains(TEXT("level")) ? FCString::Atof(*Tags[TEXT("level")]) : 0.f;
                if (Location == TEXT("underground") || (Tunnel.Len() > 0 && Tunnel != TEXT("no")) || Level < 0.f)
                        return;
        }

        TArray<int64> NodeRefs;
        for (const FXmlNode* NdChild : WayNode->GetChildrenNodes())
        {
                if (NdChild->GetTag() == TEXT("nd"))
                        NodeRefs.Add(FCString::Atoi64(*NdChild->GetAttribute(TEXT("ref"))));
        }

        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawPoints;
        if (!ResolveNodeRefs(NodeRefs, Points, RawPoints)) return;
        if (Points.Num() < 2) return;

        FGeoRoad Road;
        Road.Points = Points;
        Road.RawLonLat = RawPoints;
        ApplyRoadTags(Road, Tags);
        Roads.Add(Road);
}

// ============================================================================
// ProcessFenceWay - barrier=fence|wall|hedge (streets-gl FenceBuilder / wall)
// ============================================================================
void FOSMLoader::ProcessFenceWay(const FXmlNode* WayNode)
{
        if (!WayNode) return;
        TMap<FString, FString> Tags = ExtractTags(WayNode);
        if (Tags.Contains(TEXT("area")) && Tags[TEXT("area")] == TEXT("yes")) return;

        TArray<int64> NodeRefs;
        for (const FXmlNode* NdChild : WayNode->GetChildrenNodes())
        {
                if (NdChild->GetTag() == TEXT("nd"))
                        NodeRefs.Add(FCString::Atoi64(*NdChild->GetAttribute(TEXT("ref"))));
        }

        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawPoints;
        if (!ResolveNodeRefs(NodeRefs, Points, RawPoints) || Points.Num() < 2) return;

        const FString Barrier = Tags.FindRef(TEXT("barrier")).ToLower();
        FGeoFence Fence;
        Fence.Points = Points;
        Fence.RawLonLat = RawPoints;
        Fence.MinHeight = (float)FOSMTagHelpers::ParseMeters(Tags.FindRef(TEXT("min_height")), 0.0);

        if (Barrier == TEXT("hedge"))
        {
                Fence.Kind = EFenceKind::Hedge;
                Fence.Material = TEXT("hedge");
                Fence.Height = (float)FOSMTagHelpers::ParseMeters(Tags.FindRef(TEXT("height")), 1.0) - Fence.MinHeight;
                Fence.Thickness = 0.6f;
        }
        else if (Barrier == TEXT("wall"))
        {
                Fence.Kind = EFenceKind::Wall;
                const FString WallType = Tags.FindRef(TEXT("wall")).ToLower();
                if (WallType == TEXT("dry_stone") || WallType == TEXT("stone") || WallType == TEXT("brick"))
                        Fence.Material = TEXT("stone");
                else
                        Fence.Material = TEXT("concrete");
                const float DefaultH = 3.f;
                Fence.Height = (float)FOSMTagHelpers::ParseMeters(Tags.FindRef(TEXT("height")), DefaultH) - Fence.MinHeight;
                Fence.Thickness = 0.8f;
        }
        else // fence
        {
                Fence.Kind = EFenceKind::Fence;
                const FString FenceType = Tags.FindRef(TEXT("fence_type")).ToLower();
                float DefaultH = 1.5f;
                if (FenceType == TEXT("wood")) { Fence.Material = TEXT("wood"); DefaultH = 2.f; }
                else if (FenceType == TEXT("chain_link") || FenceType == TEXT("wire")) { Fence.Material = TEXT("chain_link"); DefaultH = 3.f; }
                else if (FenceType == TEXT("concrete")) { Fence.Material = TEXT("concrete"); DefaultH = 2.5f; }
                else { Fence.Material = TEXT("metal"); DefaultH = 1.5f; }
                Fence.Height = (float)FOSMTagHelpers::ParseMeters(Tags.FindRef(TEXT("height")), DefaultH) - Fence.MinHeight;
                Fence.Thickness = 0.05f;
        }

        if (Fence.Height <= 0.f) return;
        Fences.Add(Fence);
}

// ============================================================================
// ProcessPowerLineWay - power=line|minor_line
// ============================================================================
void FOSMLoader::ProcessPowerLineWay(const FXmlNode* WayNode)
{
        if (!WayNode) return;
        TMap<FString, FString> Tags = ExtractTags(WayNode);

        TArray<int64> NodeRefs;
        for (const FXmlNode* NdChild : WayNode->GetChildrenNodes())
        {
                if (NdChild->GetTag() == TEXT("nd"))
                        NodeRefs.Add(FCString::Atoi64(*NdChild->GetAttribute(TEXT("ref"))));
        }

        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawPoints;
        if (!ResolveNodeRefs(NodeRefs, Points, RawPoints) || Points.Num() < 2) return;

        FGeoPowerLine PL;
        PL.Points = Points;
        PL.RawLonLat = RawPoints;
        PL.bIsMinor = Tags.FindRef(TEXT("power")).ToLower() == TEXT("minor_line");
        PowerLines.Add(PL);
}

// ============================================================================
// ProcessTreeRowWay - natural=tree_row → spaced tree instances (streets-gl NodeRow)
// ============================================================================
void FOSMLoader::ProcessTreeRowWay(const FXmlNode* WayNode)
{
        if (!WayNode) return;
        TMap<FString, FString> Tags = ExtractTags(WayNode);

        TArray<int64> NodeRefs;
        for (const FXmlNode* NdChild : WayNode->GetChildrenNodes())
        {
                if (NdChild->GetTag() == TEXT("nd"))
                        NodeRefs.Add(FCString::Atoi64(*NdChild->GetAttribute(TEXT("ref"))));
        }

        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawPoints;
        if (!ResolveNodeRefs(NodeRefs, Points, RawPoints) || Points.Num() < 2) return;

        const float SpacingM = 10.f;
        const float TreeH = (float)FOSMTagHelpers::ParseMeters(Tags.FindRef(TEXT("height")), 10.0);

        float Acc = 0.f;
        auto EmitAt = [&](const FDoublePoint2D& Raw)
        {
                FGeoTree T;
                T.RawLonLat = Raw;
                T.Location = FVector2D((float)Raw.X, (float)Raw.Y);
                T.Height = TreeH > 0.f ? TreeH : 10.f;
                T.CrownRadius = FMath::Clamp(T.Height * 0.3f, 2.f, 6.f);
                Trees.Add(T);
        };

        EmitAt(RawPoints[0]);
        for (int32 i = 0; i + 1 < RawPoints.Num(); i++)
        {
                // Approximate segment length in meters via lat/lon (pre-local conversion).
                const double Lat0 = RawPoints[i].Y, Lon0 = RawPoints[i].X;
                const double Lat1 = RawPoints[i + 1].Y, Lon1 = RawPoints[i + 1].X;
                const double MidLat = (Lat0 + Lat1) * 0.5 * UE_PI / 180.0;
                const double Dx = (Lon1 - Lon0) * UE_PI / 180.0 * 6371000.0 * FMath::Cos(MidLat);
                const double Dy = (Lat1 - Lat0) * UE_PI / 180.0 * 6371000.0;
                const float SegLen = (float)FMath::Sqrt(Dx * Dx + Dy * Dy);
                if (SegLen < 0.01f) continue;

                float Dist = SpacingM - Acc;
                while (Dist < SegLen)
                {
                        const float T = Dist / SegLen;
                        FDoublePoint2D Raw(
                                Lon0 + (Lon1 - Lon0) * T,
                                Lat0 + (Lat1 - Lat0) * T);
                        EmitAt(Raw);
                        Dist += SpacingM;
                }
                Acc = SegLen - (Dist - SpacingM);
                if (Acc < 0.f) Acc = 0.f;
        }
}

// ============================================================================
// ProcessAerowayWay - runway / taxiway as wide road ribbons
// ============================================================================
void FOSMLoader::ProcessAerowayWay(const FXmlNode* WayNode)
{
        if (!WayNode) return;
        TMap<FString, FString> Tags = ExtractTags(WayNode);

        TArray<int64> NodeRefs;
        for (const FXmlNode* NdChild : WayNode->GetChildrenNodes())
        {
                if (NdChild->GetTag() == TEXT("nd"))
                        NodeRefs.Add(FCString::Atoi64(*NdChild->GetAttribute(TEXT("ref"))));
        }

        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawPoints;
        if (!ResolveNodeRefs(NodeRefs, Points, RawPoints) || Points.Num() < 2) return;

        FGeoRoad Road;
        Road.Points = Points;
        Road.RawLonLat = RawPoints;
        const FString Aero = Tags.FindRef(TEXT("aeroway")).ToLower();
        Road.RoadTypeStr = Aero;
        Road.Width = (float)FOSMTagHelpers::ParseMeters(Tags.FindRef(TEXT("width")),
                Aero == TEXT("runway") ? 45.0 : 20.0);
        Road.RoadColor = FLinearColor(0.35f, 0.35f, 0.38f);
        Road.Color = Road.RoadColor;
        Road.SidewalkSide = ERoadSide::None;
        Road.CyclewaySide = ERoadSide::None;
        Roads.Add(Road);
}

// ============================================================================
// ProcessRailwayWay
// ============================================================================
void FOSMLoader::ProcessRailwayWay(const FXmlNode* WayNode)
{
        if (!WayNode) return;

        TMap<FString, FString> Tags = ExtractTags(WayNode);

        TArray<int64> NodeRefs;
        for (const FXmlNode* NdChild : WayNode->GetChildrenNodes())
        {
                if (NdChild->GetTag() == TEXT("nd"))
                        NodeRefs.Add(FCString::Atoi64(*NdChild->GetAttribute(TEXT("ref"))));
        }

        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawPoints;
        if (!ResolveNodeRefs(NodeRefs, Points, RawPoints)) return;
        if (Points.Num() < 2) return;

        FGeoRailway Railway;
        Railway.Points = Points;
        Railway.RawLonLat = RawPoints;
        if (Tags.Contains(TEXT("name"))) Railway.Name = Tags[TEXT("name")];
        if (Tags.Contains(TEXT("railway"))) Railway.RailwayType = Tags[TEXT("railway")];
        if (Tags.Contains(TEXT("gauge"))) Railway.Gauge = FCString::Atof(*Tags[TEXT("gauge")]);
        if (Tags.Contains(TEXT("tracks"))) Railway.Tracks = FCString::Atoi(*Tags[TEXT("tracks")]);
        Railways.Add(Railway);
}

// ============================================================================
// ProcessWaterWay
// ============================================================================
void FOSMLoader::ProcessWaterWay(const FXmlNode* WayNode)
{
        if (!WayNode) return;

        TMap<FString, FString> Tags = ExtractTags(WayNode);

        TArray<int64> NodeRefs;
        for (const FXmlNode* NdChild : WayNode->GetChildrenNodes())
        {
                if (NdChild->GetTag() == TEXT("nd"))
                        NodeRefs.Add(FCString::Atoi64(*NdChild->GetAttribute(TEXT("ref"))));
        }

        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawPoints;
        if (!ResolveNodeRefs(NodeRefs, Points, RawPoints)) return;

        // ---------------------------------------------------------------------
        // A "waterway" of type river/stream/canal/drain/ditch is always a LINE
        // (a channel), never a closed area — forcing it into a filled polygon
        // (old behaviour below) produces garbled/self-intersecting geometry once
        // triangulated, and breaks Sutherland-Hodgman tile-boundary clipping
        // (which assumes a closed ring). Only genuinely closed water bodies
        // (natural=water, landuse=reservoir/basin, closed ways with no waterway
        // tag, etc.) stay as filled polygons.
        // ---------------------------------------------------------------------
        const bool bIsClosedWay = Points.Num() > 1 && Points[0].Equals(Points.Last(), 1e-6f);
        const FString WaterwayTag = Tags.Contains(TEXT("waterway")) ? Tags[TEXT("waterway")] : FString();
        static const TSet<FString> LinearWaterwayTypes = {
                TEXT("river"), TEXT("stream"), TEXT("canal"), TEXT("drain"), TEXT("ditch"), TEXT("flowline") };
        const bool bIsLinearWaterway = LinearWaterwayTypes.Contains(WaterwayTag) && !bIsClosedWay;

        FGeoWater Water;
        if (bIsLinearWaterway)
        {
                // Open river/stream line — do NOT close/fill it. GenerateWaterMeshes
                // turns this into a width-buffered ribbon instead of a filled polygon.
                if (Points.Num() < 2) return;
                Water.bIsClosed = false;
                Water.Width = GetTagFloat(Tags, TEXT("width"),
                        WaterwayTag == TEXT("river") ? 8.f : WaterwayTag == TEXT("canal") ? 6.f : 3.f);
        }
        else
        {
                // Lake / pond / sea / reservoir — closed, filled polygon (original behaviour).
                if (bIsClosedWay)
                {
                        Points.Pop();
                        RawPoints.Pop();
                }
                if (Points.Num() < 3) return;
                Water.bIsClosed = true;
        }

        Water.Points = Points;
        Water.RawLonLat = RawPoints;
        if (Tags.Contains(TEXT("name"))) Water.Name = Tags[TEXT("name")];
        Waters.Add(Water);

        UE_LOG(LogTemp, Log, TEXT("HiroGeo[ZDiag] Water way id=%s closed=%d name=\"%s\" natural=%s water=%s waterway=%s landuse=%s pts=%d"),
                *WayNode->GetAttribute(TEXT("id")),
                Water.bIsClosed ? 1 : 0,
                *Water.Name,
                Tags.Contains(TEXT("natural")) ? *Tags[TEXT("natural")] : TEXT("-"),
                Tags.Contains(TEXT("water")) ? *Tags[TEXT("water")] : TEXT("-"),
                Tags.Contains(TEXT("waterway")) ? *Tags[TEXT("waterway")] : TEXT("-"),
                Tags.Contains(TEXT("landuse")) ? *Tags[TEXT("landuse")] : TEXT("-"),
                Water.Points.Num());
}

// ============================================================================
// ProcessVegetationWay
// ============================================================================
void FOSMLoader::ProcessVegetationWay(const FXmlNode* WayNode)
{
        if (!WayNode) return;

        TMap<FString, FString> Tags = ExtractTags(WayNode);

        TArray<int64> NodeRefs;
        for (const FXmlNode* NdChild : WayNode->GetChildrenNodes())
        {
                if (NdChild->GetTag() == TEXT("nd"))
                        NodeRefs.Add(FCString::Atoi64(*NdChild->GetAttribute(TEXT("ref"))));
        }

        TArray<FVector2D> Points;
        TArray<FDoublePoint2D> RawPoints;
        if (!ResolveNodeRefs(NodeRefs, Points, RawPoints)) return;

        // Remove closing point
        if (Points.Num() > 1 && Points[0].Equals(Points.Last(), 1e-6f))
        {
                Points.Pop();
                RawPoints.Pop();
        }

        if (Points.Num() < 3) return;

        FGeoVegetation Veg;
        Veg.Points = Points;
        Veg.RawLonLat = RawPoints;

        if (Tags.Contains(TEXT("name"))) Veg.Name = Tags[TEXT("name")];

        // Determine vegetation type
        if (Tags.Contains(TEXT("landuse")))
        {
                FString Landuse = Tags[TEXT("landuse")];
                Veg.VegType = Landuse;
                if (Landuse == TEXT("forest")) Veg.Color = FLinearColor(0.1f, 0.4f, 0.1f, 1.f);
                else if (Landuse == TEXT("grass") || Landuse == TEXT("meadow")) Veg.Color = FLinearColor(0.25f, 0.6f, 0.2f, 1.f);
                else if (Landuse == TEXT("flowerbed")) Veg.Color = FLinearColor(0.6f, 0.4f, 0.3f, 1.f);
                else if (Landuse == TEXT("farmland") || Landuse == TEXT("farmyard"))
                        Veg.Color = FLinearColor(0.55f, 0.50f, 0.28f, 1.f);
                // Bare construction/brownfield/landfill ground: dirt brown, no greenery.
                else if (Landuse == TEXT("construction") || Landuse == TEXT("brownfield") || Landuse == TEXT("landfill"))
                        Veg.Color = FLinearColor(0.42f, 0.33f, 0.24f, 1.f);
                else Veg.Color = FLinearColor(0.2f, 0.55f, 0.15f, 1.f);
        }
        else if (Tags.Contains(TEXT("leisure")))
        {
                FString Leisure = Tags[TEXT("leisure")];
                Veg.VegType = Leisure;
                if (Leisure == TEXT("pitch") || Leisure == TEXT("playground"))
                {
                        // streets-gl sport → pitchType (football/basketball/tennis/generic)
                        FString Sport = Tags.FindRef(TEXT("sport")).ToLower();
                        if (Sport.Contains(TEXT("soccer")) || Sport.Contains(TEXT("football")))
                                Veg.VegType = TEXT("pitch_football");
                        else if (Sport.Contains(TEXT("basketball")))
                                Veg.VegType = TEXT("pitch_basketball");
                        else if (Sport.Contains(TEXT("tennis")))
                                Veg.VegType = TEXT("pitch_tennis");
                        else
                                Veg.VegType = TEXT("pitch");
                        Veg.Color = FLinearColor(0.25f, 0.55f, 0.28f, 1.f);
                }
                else
                        Veg.Color = FLinearColor(0.2f, 0.55f, 0.15f, 1.f);
        }
        else if (Tags.Contains(TEXT("natural")))
        {
                FString Natural = Tags[TEXT("natural")];
                Veg.VegType = Natural;
                if (Natural == TEXT("sand") || Natural == TEXT("beach"))
                        Veg.Color = FLinearColor(0.82f, 0.75f, 0.55f, 1.f);
                else if (Natural == TEXT("rock") || Natural == TEXT("bare_rock") || Natural == TEXT("scree"))
                        Veg.Color = FLinearColor(0.55f, 0.55f, 0.52f, 1.f);
                else
                {
                        Veg.VegType = (Natural == TEXT("wood")) ? TEXT("wood") : TEXT("forest");
                        Veg.Color = FLinearColor(0.1f, 0.4f, 0.1f, 1.f);
                }
        }

        Vegetations.Add(Veg);
}

// ============================================================================
// ApplyBuildingTags - Map OSM tags to FGeoBuilding fields
// ============================================================================
void FOSMLoader::ApplyBuildingTags(FGeoBuilding& Building, const TMap<FString, FString>& Tags)
{
        // Building type string
        if (Tags.Contains(TEXT("building")))
        {
                Building.BuildingTypeStr = Tags[TEXT("building")];
                // Legacy int mapping for compatibility
                FString BType = Building.BuildingTypeStr.ToLower();
                if (BType == TEXT("residential") || BType == TEXT("apartments") || BType == TEXT("house")
                        || BType == TEXT("detached") || BType == TEXT("semidetached_house") || BType == TEXT("terrace"))
                        Building.BuildingType = 0;
                else if (BType == TEXT("commercial") || BType == TEXT("office") || BType == TEXT("retail"))
                        Building.BuildingType = 1;
                else if (BType == TEXT("industrial") || BType == TEXT("warehouse"))
                        Building.BuildingType = 2;
                else
                        Building.BuildingType = 3;
        }

        // Places of worship are frequently tagged building=yes + amenity=place_of_worship
        // (e.g. "Sankt Michael"), so the building= value alone misses them. Fold the
        // amenity/religion hint into the type string so IsColorOnlyBuilding treats them
        // as colour-only landmarks (and they never pull the generic 00-03 roof slices).
        if (Tags.Contains(TEXT("amenity")) && Tags[TEXT("amenity")].ToLower() == TEXT("place_of_worship"))
                Building.BuildingTypeStr += TEXT(" place_of_worship");
        if (Tags.Contains(TEXT("religion")))
                Building.BuildingTypeStr += TEXT(" religious");

        // Name
        if (Tags.Contains(TEXT("name"))) Building.Name = Tags[TEXT("name")];

        // Height — prefer explicit height, then est_height. Parse units ("12 m", "40ft")
        // the same way streets-gl's parseHeight / parseMeters helpers do.
        if (Tags.Contains(TEXT("height")))
                Building.Height = FOSMTagHelpers::ParseMeters(Tags[TEXT("height")], 0.0);
        else if (Tags.Contains(TEXT("est_height")))
                Building.Height = FOSMTagHelpers::ParseMeters(Tags[TEXT("est_height")], 0.0);

        // Levels
        if (Tags.Contains(TEXT("building:levels")))
                Building.Levels = FCString::Atoi(*Tags[TEXT("building:levels")]);

        // Min height — must be parsed before the level-based height estimate so that
        // building:parts with only building:levels get the correct absolute top height.
        if (Tags.Contains(TEXT("min_height")))
                Building.MinHeight = FOSMTagHelpers::ParseMeters(Tags[TEXT("min_height")], 0.0);

        // Roof height / levels early — needed for the streets-gl height formula
        // (levels * 4m + roofHeight) below.
        if (Tags.Contains(TEXT("roof:height")))
                Building.RoofHeight = FOSMTagHelpers::ParseMeters(Tags[TEXT("roof:height")], 0.0);
        if (Tags.Contains(TEXT("roof:levels")))
                Building.RoofLevels = FCString::Atoi(*Tags[TEXT("roof:levels")]);

        // Default height from levels (streets-gl: levelHeight = 4 m, plus roof).
        // For building:parts the OSM 'height' tag is the ABSOLUTE top from ground level.
        // When only building:levels is present, estimate the absolute top as
        // min_height + levels * floor_height (+ roof when not a part) so elevated
        // parts (towers, etc.) render at the correct altitude.
        const double LevelH = FOSMTagHelpers::LevelHeightMeters;
        double RoofHForEstimate = Building.RoofHeight;
        if (RoofHForEstimate <= 0.0 && Building.RoofLevels > 0)
                RoofHForEstimate = Building.RoofLevels * LevelH;

        if (Building.Height <= 0.0)
        {
                if (Building.Levels > 0)
                {
                        if (Building.bIsBuildingPart && Building.MinHeight > 0.0)
                                Building.Height = Building.MinHeight + Building.Levels * LevelH;
                        else
                                Building.Height = Building.Levels * LevelH + RoofHForEstimate;
                }
                else
                {
                        // Neither height nor levels — streets-gl fallback: 1 level + roof.
                        Building.Levels = 1;
                        Building.Height = LevelH + RoofHForEstimate;
                }
        }

        // Min level
        if (Tags.Contains(TEXT("building:min_level")))
                Building.MinLevel = FCString::Atoi(*Tags[TEXT("building:min_level")]);

        // Roof shape
        if (Tags.Contains(TEXT("roof:shape")))
                Building.RoofShape = FOSMRoofShapeParser::Parse(Tags[TEXT("roof:shape")]);
        else if (Tags.Contains(TEXT("building:roof:shape")))
                Building.RoofShape = FOSMRoofShapeParser::Parse(Tags[TEXT("building:roof:shape")]);

        // Roof angle
        if (Tags.Contains(TEXT("roof:angle")))
                Building.RoofAngle = FCString::Atod(*Tags[TEXT("roof:angle")]);

        // Roof direction (degrees: 0=North, 90=East, 180=South, 270=West)
        if (Tags.Contains(TEXT("roof:direction")))
                Building.RoofDirection = FCString::Atod(*Tags[TEXT("roof:direction")]);

        // Roof orientation (across / along)
        if (Tags.Contains(TEXT("roof:orientation")))
                Building.RoofOrientation = Tags[TEXT("roof:orientation")];

        // Roof material
        if (Tags.Contains(TEXT("roof:material")))
                Building.RoofMaterial = Tags[TEXT("roof:material")];

        // Roof colour
        if (Tags.Contains(TEXT("roof:colour")))
        {
                Building.RoofColor = FOSMColorParser::Parse(Tags[TEXT("roof:colour")], Building.RoofColor);
                Building.bHasExplicitRoofColor = true;
        }

        // Building colour
        if (Tags.Contains(TEXT("building:colour")))
        {
                Building.WallColor = FOSMColorParser::Parse(Tags[TEXT("building:colour")], Building.WallColor);
                Building.Color = Building.WallColor;
                Building.bHasExplicitBuildingColour = true;
        }

        // Building material
        if (Tags.Contains(TEXT("building:material")))
                Building.FacadeMaterialType = FOSMBuildingMaterialParser::Parse(Tags[TEXT("building:material")]);

        // Estimate roof height from angle if not set
        if (Building.RoofHeight <= 0.0 && Building.RoofAngle > 0.0 && Building.Nodes.Num() > 0)
        {
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

        // Default roof height for non-flat roofs.
        // Onion, Dome and Round are intentionally LEFT at 0 here. Their rise is
        // derived from the footprint width inside GeoMeshGenerator (where the
        // geometry is already in local units — at this parse stage Building.Nodes
        // are still raw lat/lon degrees, so a width-based calc here would be wrong).
        // A flat 3 m default also marks roof:height as explicit downstream, which
        // would block that width-proportional sizing.
        // Gambrel and Mansard are also LEFT at 0: they carry roof:levels which
        // GeoMeshGenerator converts to height (levels*300 UU). Forcing 3 m here
        // sets bRoofHeightExplicit=true and causes the roof:levels branch to be
        // skipped, producing a squashed gambrel on top of lowered walls.
        if (Building.RoofHeight <= 0.0 && Building.RoofShape != ERoofShape::Flat
                && Building.RoofShape != ERoofShape::Onion
                && Building.RoofShape != ERoofShape::Dome
                && Building.RoofShape != ERoofShape::Round
                && Building.RoofShape != ERoofShape::Gambrel
                && Building.RoofShape != ERoofShape::Mansard)
                Building.RoofHeight = 3.0;
}

// ============================================================================
// ApplyRoadTags
// ============================================================================
void FOSMLoader::ApplyRoadTags(FGeoRoad& Road, const TMap<FString, FString>& Tags)
{
        if (Tags.Contains(TEXT("highway")))
        {
                Road.RoadTypeStr = Tags[TEXT("highway")];
                FString HW = Road.RoadTypeStr.ToLower();

                if (HW == TEXT("motorway"))            Road.Width = 24.0f;
                else if (HW == TEXT("motorway_link"))   Road.Width = 12.0f;
                else if (HW == TEXT("trunk"))           Road.Width = 20.0f;
                else if (HW == TEXT("trunk_link"))      Road.Width = 12.0f;
                else if (HW == TEXT("primary"))         Road.Width = 14.0f;
                else if (HW == TEXT("primary_link"))    Road.Width = 10.0f;
                else if (HW == TEXT("secondary"))       Road.Width = 12.0f;
                else if (HW == TEXT("secondary_link"))  Road.Width = 10.0f;
                else if (HW == TEXT("tertiary"))        Road.Width = 10.0f;
                else if (HW == TEXT("residential"))     Road.Width = 8.0f;
                else if (HW == TEXT("service"))         Road.Width = 5.0f;
                else if (HW == TEXT("living_street"))   Road.Width = 6.0f;
                else if (HW == TEXT("pedestrian"))      Road.Width = 6.0f;
                else if (HW == TEXT("footway"))         Road.Width = 2.0f;
                else if (HW == TEXT("path"))            Road.Width = 1.5f;
                else if (HW == TEXT("cycleway"))        Road.Width = 2.0f;
                else if (HW == TEXT("steps"))           Road.Width = 2.0f;
                else if (HW == TEXT("track"))           Road.Width = 3.0f;
                else                                    Road.Width = 8.0f;

                Road.bIsHighway = (HW == TEXT("motorway") || HW == TEXT("trunk") ||
                        HW == TEXT("primary") || HW == TEXT("secondary") || HW == TEXT("tertiary") ||
                        HW == TEXT("motorway_link") || HW == TEXT("trunk_link") ||
                        HW == TEXT("primary_link") || HW == TEXT("secondary_link"));
        }

        if (Tags.Contains(TEXT("name"))) Road.Name = Tags[TEXT("name")];
        if (Tags.Contains(TEXT("width")))
        {
                float W = (float)FOSMTagHelpers::ParseMeters(Tags[TEXT("width")], 0.0);
                if (W > 0.f) Road.Width = W;
        }
        if (Tags.Contains(TEXT("surface:colour")))
                Road.RoadColor = FOSMColorParser::Parse(Tags[TEXT("surface:colour")], Road.RoadColor);

        // streets-gl: only emit sidewalk / cycleway side ribbons when tagged.
        Road.SidewalkSide = FOSMTagHelpers::ParseSidewalkSide(Tags);
        Road.CyclewaySide = FOSMTagHelpers::ParseCyclewaySide(Tags);

        // Grade separation: a flat multi-level interchange (bridges/ramps that
        // physically pass over/under each other) otherwise gets rendered onto one
        // single Z plane, so unrelated ramps visually cross/overlap in the middle
        // of their curves — not just at real intersections. Use the OSM "layer"
        // tag directly when present; otherwise infer a sensible default from
        // bridge=yes/tunnel=yes (which conventionally imply layer=1 / layer=-1
        // when "layer" itself isn't explicitly tagged).
        int32 Layer = 0;
        bool bHasExplicitLayer = false;
        if (Tags.Contains(TEXT("layer")))
        {
                Layer = FCString::Atoi(*Tags[TEXT("layer")]);
                bHasExplicitLayer = true;
        }
        if (!bHasExplicitLayer)
        {
                const bool bBridge = Tags.Contains(TEXT("bridge")) && Tags[TEXT("bridge")] != TEXT("no");
                const bool bTunnel = Tags.Contains(TEXT("tunnel")) && Tags[TEXT("tunnel")] != TEXT("no");
                if (bBridge) Layer = 1;
                else if (bTunnel) Layer = -1;
        }
        Road.Layer = Layer;
}

// ============================================================================
// LatLonToLocal
// ============================================================================
FVector2D FOSMLoader::LatLonToLocal(double Lat, double Lon) const
{
        const double EarthRadiusM = 6371000.0;
        double X = EarthRadiusM * ((Lon - CenterLongitude) * UE_PI / 180.0) * FMath::Cos(CenterLatitude * UE_PI / 180.0);
        double Y = EarthRadiusM * ((Lat - CenterLatitude) * UE_PI / 180.0);
        return FVector2D(static_cast<float>(X), static_cast<float>(Y));
}

// ============================================================================
// CalculateCenterPoint
// ============================================================================
void FOSMLoader::CalculateCenterPoint()
{
        CenterLatitude = (MinLat + MaxLat) / 2.0;
        CenterLongitude = (MinLon + MaxLon) / 2.0;
}

// ============================================================================
// ConvertAllCoordsToLocal
// ============================================================================
void FOSMLoader::ConvertAllCoordsToLocal()
{
        // Convert nodes (double-precision)
        NodeCoords.Empty();
        for (auto& Pair : NodeRawCoords)
        {
                NodeCoords.Add(Pair.Key, LatLonToLocal(Pair.Value.Y, Pair.Value.X));
        }

        // Buildings
        for (FGeoBuilding& B : Buildings)
        {
                if (B.RawLonLat.Num() > 0)
                {
                        B.Nodes.Empty();
                        for (const FDoublePoint2D& RL : B.RawLonLat)
                                B.Nodes.Add(LatLonToLocal(RL.Y, RL.X));
                }
                if (B.Nodes.Num() > 0)
                {
                        float CX = 0.f, CY = 0.f;
                        for (const FVector2D& N : B.Nodes) { CX += N.X; CY += N.Y; }
                        B.Center = FVector2D(CX / B.Nodes.Num(), CY / B.Nodes.Num());
                }

                // Convert inner rings (courtyards / holes) to local meters.
                B.Holes.Empty();
                for (const TArray<FDoublePoint2D>& HRaw : B.HolesRaw)
                {
                        TArray<FVector2D> H;
                        H.Reserve(HRaw.Num());
                        for (const FDoublePoint2D& RL : HRaw)
                                H.Add(LatLonToLocal(RL.Y, RL.X));
                        if (H.Num() >= 3)
                                B.Holes.Add(H);
                }
        }

        // Roads
        for (FGeoRoad& R : Roads)
        {
                if (R.RawLonLat.Num() > 0)
                {
                        R.Points.Empty();
                        for (const FDoublePoint2D& RL : R.RawLonLat)
                                R.Points.Add(LatLonToLocal(RL.Y, RL.X));
                }
        }

        // Water
        for (FGeoWater& W : Waters)
        {
                if (W.RawLonLat.Num() > 0)
                {
                        W.Points.Empty();
                        for (const FDoublePoint2D& RL : W.RawLonLat)
                                W.Points.Add(LatLonToLocal(RL.Y, RL.X));
                }
                W.Holes.Empty();
                for (const TArray<FDoublePoint2D>& HRaw : W.HolesRaw)
                {
                        TArray<FVector2D>& H = W.Holes.AddDefaulted_GetRef();
                        for (const FDoublePoint2D& RL : HRaw)
                                H.Add(LatLonToLocal(RL.Y, RL.X));
                }
        }

        // Vegetation
        for (FGeoVegetation& V : Vegetations)
        {
                if (V.RawLonLat.Num() > 0)
                {
                        V.Points.Empty();
                        for (const FDoublePoint2D& RL : V.RawLonLat)
                                V.Points.Add(LatLonToLocal(RL.Y, RL.X));
                }
                V.Holes.Empty();
                for (const TArray<FDoublePoint2D>& HRaw : V.HolesRaw)
                {
                        TArray<FVector2D>& H = V.Holes.AddDefaulted_GetRef();
                        for (const FDoublePoint2D& RL : HRaw)
                                H.Add(LatLonToLocal(RL.Y, RL.X));
                }
        }

        // POIs
        for (FGeoPOI& P : POIs)
        {
                P.Location = LatLonToLocal(P.RawLonLat.Y, P.RawLonLat.X);
        }

        // Trees
        for (FGeoTree& T : Trees)
        {
                T.Location = LatLonToLocal(T.RawLonLat.Y, T.RawLonLat.X);
        }

        // Model instances (street furniture, power, landmarks)
        for (FGeoModelInstance& M : ModelInstances)
        {
                M.Location = LatLonToLocal(M.RawLonLat.Y, M.RawLonLat.X);
        }

        // Railways
        for (FGeoRailway& R : Railways)
        {
                if (R.RawLonLat.Num() > 0)
                {
                        R.Points.Empty();
                        for (const FDoublePoint2D& RL : R.RawLonLat)
                                R.Points.Add(LatLonToLocal(RL.Y, RL.X));
                }
        }

        // Fences / walls / hedges
        for (FGeoFence& F : Fences)
        {
                if (F.RawLonLat.Num() > 0)
                {
                        F.Points.Empty();
                        for (const FDoublePoint2D& RL : F.RawLonLat)
                                F.Points.Add(LatLonToLocal(RL.Y, RL.X));
                }
        }

        // Power lines
        for (FGeoPowerLine& PL : PowerLines)
        {
                if (PL.RawLonLat.Num() > 0)
                {
                        PL.Points.Empty();
                        for (const FDoublePoint2D& RL : PL.RawLonLat)
                                PL.Points.Add(LatLonToLocal(RL.Y, RL.X));
                }
        }

        // Elevation samples (OSM ele nodes)
        for (FElevationSample& E : ElevationSamples)
        {
                E.Location = LatLonToLocal(E.RawLonLat.Y, E.RawLonLat.X);
        }
}

// ============================================================================
// AssembleBuildingParts - Group building:part entries with their parent buildings
// ============================================================================
void FOSMLoader::AssembleBuildingParts()
{
        // Ray-casting point-in-polygon. Works in any consistent 2D coord system,
        // so it is safe to call here (before WGS84->local conversion) because the
        // outline polygon and the part centroid are in the SAME system.
        auto PointInPoly = [](const FVector2D& P, const TArray<FVector2D>& Poly) -> bool
        {
                bool bInside = false;
                const int32 N = Poly.Num();
                for (int32 i = 0, j = N - 1; i < N; j = i++)
                {
                        const FVector2D& A = Poly[i];
                        const FVector2D& B = Poly[j];
                        if (((A.Y > P.Y) != (B.Y > P.Y)) &&
                                (P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X))
                                bInside = !bInside;
                }
                return bInside;
        };

        int32 PartCount = 0, SuppressCount = 0;
        for (const FGeoBuilding& B : Buildings)
                if (B.bIsBuildingPart) PartCount++;

        // Simple 3D Buildings rule (what streets.gl does): when a building outline
        // is covered by building:part polygons, the OUTLINE itself must NOT be
        // rendered in 3D — only the parts define the geometry. Otherwise the
        // outline's walls+roof (often a flat roof at a different height) sit on top
        // of the detailed parts and ruin the result (e.g. the flat slab that was
        // covering the Frauenkirche nave + towers).
        //
        // Detect such outlines by testing whether any part's centroid lies inside
        // them. (Parts are usually smaller than, and contained by, their outline.)
        if (PartCount > 0)
        {
                for (FGeoBuilding& Outline : Buildings)
                {
                        if (Outline.bIsBuildingPart) continue;
                        if (Outline.Nodes.Num() < 3) continue;

                        for (const FGeoBuilding& Part : Buildings)
                        {
                                if (!Part.bIsBuildingPart) continue;
                                if (&Part == &Outline) continue;
                                if (PointInPoly(Part.Center, Outline.Nodes))
                                {
                                        Outline.bSuppressOutline = true;
                                        SuppressCount++;
                                        break;
                                }
                        }
                }
        }

        // Suppress flat roofs of building:parts that have another part sitting directly
        // on top (its min_height ≈ this part's height AND its centroid is inside this
        // part's footprint). This eliminates the "phantom plane" visible between towers
        // and the nave of a church where the nave's flat cap bleeds through.
        int32 RoofSuppressCount = 0;
        for (FGeoBuilding& Base : Buildings)
        {
                if (!Base.bIsBuildingPart) continue;
                if (Base.bSuppressOutline) continue;
                if (Base.Height <= 0.0) continue;
                // Only FLAT caps create the "phantom plane" this pass removes. A part
                // carrying a real roof shape (onion, dome, gabled, ...) is a genuine
                // feature — e.g. an onion dome sitting under a finial/cross part — and
                // must never be suppressed, or it vanishes from the scene.
                if (Base.RoofShape != ERoofShape::Flat) continue;

                for (const FGeoBuilding& Upper : Buildings)
                {
                        if (!Upper.bIsBuildingPart) continue;
                        if (&Upper == &Base) continue;
                        // Upper part starts approximately where Base ends (within 0.5 m).
                        if (FMath::Abs((float)(Upper.MinHeight - Base.Height)) > 0.5f) continue;
                        if (PointInPoly(Upper.Center, Base.Nodes))
                        {
                                Base.bSuppressRoof = true;
                                RoofSuppressCount++;
                                break;
                        }
                }
        }

        UE_LOG(LogTemp, Log,
                TEXT("OSMLoader: %d building:parts; %d outline(s) suppressed; %d part roof(s) suppressed (covered by upper parts)"),
                PartCount, SuppressCount, RoofSuppressCount);
}

// ============================================================================
// Helper: ExtractTags
// ============================================================================
TMap<FString, FString> FOSMLoader::ExtractTags(const FXmlNode* Element)
{
        TMap<FString, FString> Tags;
        if (!Element) return Tags;

        for (const FXmlNode* Child : Element->GetChildrenNodes())
        {
                if (Child->GetTag() == TEXT("tag"))
                {
                        FString K = Child->GetAttribute(TEXT("k"));
                        FString V = Child->GetAttribute(TEXT("v"));
                        if (!K.IsEmpty())
                                Tags.Add(K, V);
                }
        }
        return Tags;
}

// ============================================================================
// Helper: GetTag
// ============================================================================
FString FOSMLoader::GetTag(const TMap<FString, FString>& Tags, const FString& Key, const FString& Default)
{
        const FString* Val = Tags.Find(Key);
        return Val ? *Val : Default;
}

// ============================================================================
// Helper: GetTagFloat
// ============================================================================
float FOSMLoader::GetTagFloat(const TMap<FString, FString>& Tags, const FString& Key, float Default)
{
        const FString* Val = Tags.Find(Key);
        return Val ? FCString::Atof(**Val) : Default;
}

// ============================================================================
// Helper: GetTagInt
// ============================================================================
int32 FOSMLoader::GetTagInt(const TMap<FString, FString>& Tags, const FString& Key, int32 Default)
{
        const FString* Val = Tags.Find(Key);
        return Val ? FCString::Atoi(**Val) : Default;
}

// ============================================================================
// Helper: ResolveNodeRefs
// ============================================================================
bool FOSMLoader::ResolveNodeRefs(const TArray<int64>& NodeRefs, TArray<FVector2D>& OutPoints, TArray<FDoublePoint2D>& OutRawPoints) const
{
        OutPoints.Empty();
        OutRawPoints.Empty();

        for (int64 Ref : NodeRefs)
        {
                const FDoublePoint2D* Raw = NodeRawCoords.Find(Ref);
                if (!Raw)
                {
                        OutPoints.Reset();
                        OutRawPoints.Reset();
                        return false;
                }
                OutRawPoints.Add(*Raw);
                // Temporary float coords (will be overwritten by ConvertAllCoordsToLocal)
                OutPoints.Add(FVector2D(static_cast<float>(Raw->X), static_cast<float>(Raw->Y)));
        }

        return OutPoints.Num() >= 2;
}