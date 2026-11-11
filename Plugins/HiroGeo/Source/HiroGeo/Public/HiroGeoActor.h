// Copyright Hiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "HiroGeo.h"
#include "HiroGeoActor.generated.h"

struct FGeoModelInstance;
struct FGeoBuilding;
struct FGeoRoad;
struct FGeoWater;
struct FGeoVegetation;
struct FGeoTree;
struct FGeoRailway;
struct FGeoFence;
struct FGeoPowerLine;
struct FHiroGeoHeightmap;
class FHiroGeoElevationFetcher;

UCLASS(hidecategories = (Rendering, Physics, Collision, Navigation, Lighting,
                          AssetUserData, Activation, Instances, Input, Mobile))
class HIROGEO_API AHiroGeoActor : public AActor
{
        GENERATED_BODY()

public:
        AHiroGeoActor();

        // ====================================================================
        // Actions
        // ====================================================================
        UFUNCTION(CallInEditor, Category = "HiroGeo", meta = (DisplayPriority = "0"))
        void Generate();

#if WITH_DEV_AUTOMATION_TESTS
        /** Injects a deterministic DEM completion for actor-level automation.
         *  Inline mode models a full cache hit; deferred mode is completed by
         *  CompleteAutomationElevation() after Generate() returns. */
        void SetAutomationElevation(TSharedPtr<FHiroGeoHeightmap> Heightmap, bool bCompleteInline);
        bool CompleteAutomationElevation();
#endif

        UFUNCTION(CallInEditor, Category = "HiroGeo", meta = (DisplayPriority = "0"))
        void Clear();

        // ====================================================================
        // Data Source
        // ====================================================================
        /** Path to GeoJSON file (used when bUseGeoJSON = true) */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "1"))
        FString GeoJSONFilePath;

        /** Path to OSM (.osm) file (used when bUseGeoJSON = false) */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "1"))
        FString OSMFilePath;

        /** True = load GeoJSON, False = load OSM */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "1"))
        bool bUseGeoJSON;

        /** Meters -> UE units conversion. Default 100 (1 m = 100 cm). */
        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (ClampMin = "1.0", ClampMax = "1000.0", DisplayPriority = "1"))
        float ScaleFactor;

        /** Z offset applied to the entire ground mesh. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "1"))
        float GroundOffset;

        /** Use vertex color material for rendering (default: true) */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "1"))
        bool bUseVertexColorMaterial;

        /** When true: windows rendered as real 3D glass geometry using M_Glass (Section 1).
         *  When false (default): windows are a flat facade atlas texture slice (Section 6)
         *  — cheaper and better for city-scale views. Toggle without re-generating. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "1"))
        bool b3DWindow;

        /** Deform ground mesh from OSM ele tags / GeoJSON Z coordinates (streets.gl terrain). */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "1"))
        bool bUseTerrainElevation;

        /** Grid resolution for terrain mesh when elevation data is available. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (ClampMin = "8", ClampMax = "256", DisplayPriority = "1"))
        int32 TerrainGridSubdivisions;

        /** Fetch real-world terrain elevation from Esri's World Elevation 3D service
         *  (elevation3d.arcgis.com — the same dataset streets.gl uses) for the loaded
         *  area, and use it to deform the ground mesh instead of the sparse OSM
         *  "ele" tag interpolation. Always fetches live over the network — there is
         *  no local tile cache. Requires internet access at Generate() time. Falls
         *  back to the existing "ele"-tag behaviour if the fetch fails or finds no
         *  data. Works for both OSM and GeoJSON paths (both provide WGS84 bounds). */
        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (DisplayPriority = "1", EditCondition = "bUseTerrainElevation"))
        bool bFetchOnlineElevation = true;

        /** Web-Mercator zoom level used for the online elevation fetch. Higher values
         *  give finer terrain detail but request more tiles (resolution is roughly
         *  156543 / 2^Zoom meters/pixel at the equator, less at higher latitudes).
         *  14-16 is a good range for city-block-scale terrain; lower it for very
         *  large OSM extracts to keep the tile count (and request count) down. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (ClampMin = "10", ClampMax = "18", DisplayPriority = "1", EditCondition = "bFetchOnlineElevation"))
        int32 ElevationZoomLevel = 15;

        /** Generate railway ballast + rails from OSM railway ways. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "1"))
        bool bShowRailways;

        /** Generate coloured POI pin markers for amenity/shop/tourism nodes. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "1"))
        bool bShowPOIMarkers;

        /** Bind streets-gl normal/roughness textures when the PBR material supports them. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "1"))
        bool bUsePBRMaterials;

        /** When true (default), Esri LERC elevation tiles are cached under
         *  Saved/HiroGeoElevationCache/ — UE equivalent of streets-gl tile reuse. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (DisplayPriority = "1", EditCondition = "bFetchOnlineElevation"))
        bool bUseElevationDiskCache = true;

        // Presentation FX: streets-gl deferred stack is not ported. Prefer UE post-process
        // (AO / contact shadows) and material roughness for mesh readability.

        /** Distance-based detail cull (UE LOD equivalent of streets-gl tile LOD).
         *  When enabled, POI markers, fence wires, and vegetation instances hide
         *  beyond LODCullDistanceCm from the player/editor camera. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo|LOD", meta = (DisplayPriority = "4"))
        bool bEnableDistanceLOD = true;

        UPROPERTY(EditAnywhere, Category = "HiroGeo|LOD",
                meta = (ClampMin = "5000.0", ClampMax = "500000.0", DisplayPriority = "4", EditCondition = "bEnableDistanceLOD"))
        float LODCullDistanceCm = 25000.f;

        /** POI marker base radius in UE units (cm). */
        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (ClampMin = "20.0", ClampMax = "500.0", DisplayPriority = "1", EditCondition = "bShowPOIMarkers"))
        float POIMarkerRadius;

        /** POI marker height in UE units (cm). */
        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (ClampMin = "50.0", ClampMax = "2000.0", DisplayPriority = "1", EditCondition = "bShowPOIMarkers"))
        float POIMarkerHeight;

        // ====================================================================
        // Material Override (optional - if null, OSM colors are used)
        // ====================================================================
        /** Optional override material for building walls.
         *  If null, a dynamic material is created from OSM building:colour tag. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo|Material Override", meta = (DisplayPriority = "2"))
        UMaterialInterface* WallMaterialOverride;

        /** Optional override material for roofs.
         *  If null, a dynamic material is created from OSM roof:colour tag. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo|Material Override", meta = (DisplayPriority = "2"))
        UMaterialInterface* RoofMaterialOverride;

        /** Optional override material for roads. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo|Material Override", meta = (DisplayPriority = "2"))
        UMaterialInterface* RoadMaterialOverride;

        /** Optional override material for water. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo|Material Override", meta = (DisplayPriority = "2"))
        UMaterialInterface* WaterMaterialOverride;

        /** Optional override material for ground/vegetation. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo|Material Override", meta = (DisplayPriority = "2"))
        UMaterialInterface* GroundMaterialOverride;

        /** Optional override for railway ballast bed. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo|Material Override", meta = (DisplayPriority = "2"))
        UMaterialInterface* RailwayMaterialOverride;

        /** Optional override for POI markers (falls back to vertex colour). */
        UPROPERTY(EditAnywhere, Category = "HiroGeo|Material Override", meta = (DisplayPriority = "2"))
        UMaterialInterface* POIMaterialOverride;

        // ====================================================================
        // Vegetation
        // ====================================================================
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "3"))
        UStaticMesh* TreeMesh;

        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "3"))
        UStaticMesh* GrassMesh;

        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (ClampMin = "0.0", ClampMax = "10.0", DisplayPriority = "3"))
        float TreeDensityScale;

        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (ClampMin = "0.0", ClampMax = "10.0", DisplayPriority = "3"))
        float GrassDensityScale;

        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (ClampMin = "10", ClampMax = "10000", DisplayPriority = "3"))
        int32 MaxTreeInstances;

        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (ClampMin = "10", ClampMax = "50000", DisplayPriority = "3"))
        int32 MaxGrassInstances;

        /** Uniform scale applied to placed OSM models (hydrants, benches, towers, …).
         *  Increase if the imported models appear too small for the world scale. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo",
                meta = (ClampMin = "0.01", ClampMax = "1000.0", DisplayPriority = "3"))
        float ModelScale;

        /** Base rotation applied to every placed model BEFORE its heading yaw.
         *  Fixes models whose up-axis differs from UE (Y-up GLB imports lie flat —
         *  Roll = 90 stands them upright). Tweak if models face the wrong way. */
        UPROPERTY(EditAnywhere, Category = "HiroGeo", meta = (DisplayPriority = "3"))
        FRotator ModelBaseRotation;

        // ====================================================================
        // Components (read-only)
        // ====================================================================
        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        USceneComponent* DefaultRoot;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UProceduralMeshComponent* BuildingMeshComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UProceduralMeshComponent* RoadMeshComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UProceduralMeshComponent* SidewalkMeshComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UProceduralMeshComponent* WaterMeshComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UProceduralMeshComponent* GroundMeshComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UProceduralMeshComponent* RoofMeshComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UProceduralMeshComponent* VegetationMeshComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UProceduralMeshComponent* RailwayMeshComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UProceduralMeshComponent* FenceMeshComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UProceduralMeshComponent* POIMeshComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UInstancedStaticMeshComponent* TreeInstancedComp;

        UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "HiroGeo|Components",
                meta = (DisplayPriority = "10"))
        UInstancedStaticMeshComponent* GrassInstancedComp;

        // ====================================================================
        // Grid Cell Tracking (نکته ۱: جلوگیری از ساخت مجدد Grid)
        // ====================================================================
        /** CellKey of the grid cell that generated this actor.
         *  Empty if generated from Free selection or local file. */
        UPROPERTY(VisibleAnywhere, Category = "HiroGeo|Grid")
        FString GeneratedCellKey;

        /** Returns the CellKey assigned to this actor. */
        FString GetGeneratedCellKey() const { return GeneratedCellKey; }

        /** Sets the CellKey for this actor (called by importer before Generate). */
        void SetGeneratedCellKey(const FString& InKey) { GeneratedCellKey = InKey; }

        // ====================================================================
        // Grid Bounds (نکته ۲: مش‌ها باید فقط به اندازه Grid ساخته بشن)
        // ====================================================================
        /** Geographic bounding box of the grid cell (in degrees).
         *  When set, the Generate() method will clip/filter all mesh geometry
         *  to stay within these bounds, preventing overlap with neighbouring cells. */
        UPROPERTY(VisibleAnywhere, Category = "HiroGeo|Grid")
        double GridMinLat = 0.0;

        UPROPERTY(VisibleAnywhere, Category = "HiroGeo|Grid")
        double GridMinLon = 0.0;

        UPROPERTY(VisibleAnywhere, Category = "HiroGeo|Grid")
        double GridMaxLat = 0.0;

        UPROPERTY(VisibleAnywhere, Category = "HiroGeo|Grid")
        double GridMaxLon = 0.0;

        /** Returns true if grid bounds have been set (i.e. this actor belongs to a grid cell). */
        bool HasGridBounds() const
        {
                return GridMinLat != 0.0 || GridMinLon != 0.0 || GridMaxLat != 0.0 || GridMaxLon != 0.0;
        }

        /** Sets the grid bounding box (called by importer before Generate). */
        void SetGridBounds(double InMinLat, double InMinLon, double InMaxLat, double InMaxLon)
        {
                GridMinLat = InMinLat;
                GridMinLon = InMinLon;
                GridMaxLat = InMaxLat;
                GridMaxLon = InMaxLon;
        }

        // ====================================================================
        // Mesh Local Origin (برای چینش صحیح کاشی‌ها در دنیا)
        // ====================================================================
        /** Geographic coordinate (WGS84) that the generated mesh's local origin
         *  (0,0,0 in actor space) corresponds to. This is the actual centroid of
         *  the loaded/clipped OSM data (computed inside Generate()), which is
         *  NOT necessarily the same point as the requested grid cell's bounding
         *  box centre — the importer must use THIS value (not the bbox centre)
         *  to position this actor relative to other tiles, or adjacent grid
         *  cells will not line up correctly. Populated only for the OSM data
         *  path; (0,0) if unset. */
        UPROPERTY(VisibleAnywhere, Category = "HiroGeo|Grid")
        double MeshOriginLatitude = 0.0;

        UPROPERTY(VisibleAnywhere, Category = "HiroGeo|Grid")
        double MeshOriginLongitude = 0.0;

        double GetMeshOriginLatitude() const { return MeshOriginLatitude; }
        double GetMeshOriginLongitude() const { return MeshOriginLongitude; }

        /** Returns true if Generate() has populated a valid mesh-local origin. */
        bool HasMeshOrigin() const { return MeshOriginLatitude != 0.0 || MeshOriginLongitude != 0.0; }

        // ====================================================================
        // Ground-plane bounds for the selected grid cell (Grid mode only).
        // Computed once in Generate() — in the SAME centered local-meter space
        // as every other generator's MinX/MaxX/MinY/MaxY (i.e. already shifted
        // by -OX/-OY) — and re-used by RebuildMeshesWithHeightmap() later, so
        // the ground plane is exactly the requested cell size (e.g. 500x500m)
        // instead of being sized off the OSM data's bounding box + padding.
        // ====================================================================
        bool bHasGridGroundBounds = false;
        float GridGroundMinX = 0.f, GridGroundMaxX = 0.f;
        float GridGroundMinY = 0.f, GridGroundMaxY = 0.f;

        // ====================================================================
        // Cross-tile vertical (Z) alignment.
        // The first tile ever generated in a map becomes the world's elevation
        // baseline (persisted the same way SHiroGeoImporterWindow persists
        // OriginLatLon, under the same GConfig section, key "OriginElevation").
        // Every subsequent tile offsets its own actor Z by the real-world
        // elevation delta between its own DEM-sampled origin height and that
        // baseline, once its async DEM fetch resolves — see
        // RebuildMeshesWithHeightmap(). Without this, every tile sits at
        // world Z=0 regardless of its true ground elevation, so adjacent
        // tiles whose terrain genuinely differs in height end up visibly
        // mismatched at the seam.
        // ====================================================================
        double MeshOriginElevation = 0.0;
        bool bHasMeshOriginElevation = false;
        double GetMeshOriginElevation() const { return MeshOriginElevation; }
        bool HasMeshOriginElevation() const { return bHasMeshOriginElevation; }

protected:
        virtual void BeginPlay() override;
        virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
        virtual void Tick(float DeltaSeconds) override;

        /** Snap to world origin (0,0,0) the moment the actor is dropped into a
         *  viewport, so the generated city — which is centred on (0,0,0) — lines up
         *  without any manual transform tweaking. Saved/loaded actors keep their
         *  stored transform, so it can still be moved later if desired. */
        virtual void PostActorCreated() override;

private:
        void UpdateDistanceLOD();
        // ---- Internal material references (loaded in constructor or LoadDefaultAssets) ----
        UMaterialInterface* BuildingWallMaterial;
        UMaterialInterface* BuildingGlassMaterial;
        UMaterialInterface* BrickFacadeMaterial;
        UMaterialInterface* DarkStoneFacadeMaterial;
        UMaterialInterface* LightStoneFacadeMaterial;
        UMaterialInterface* RoofMaterial;
        UMaterialInterface* RoadMaterial;
        UMaterialInterface* SidewalkMaterial;
        UMaterialInterface* WaterMaterial;
        UMaterialInterface* GroundGrassMaterial;
        UMaterialInterface* VegetationMaterial;
        UMaterialInterface* RailwayBallastMaterial;
        UMaterialInterface* RailwayRailMaterial;
        UMaterialInterface* POIMaterial;

        // ---- Material creation helpers ----

        /** Load all default material assets from project content */
        void LoadDefaultAssets();

        /** Create a solid-color parent material (M_SolidColor) */
        UMaterialInterface* CreateSolidColorMaterial();

        /** Create a vertex-color material that displays per-vertex colors. */
        UMaterialInterface* CreateVertexColorMaterial();

        /** Create a glass material */
        UMaterialInterface* CreateGlassMaterial();

        /** Create a dynamic material instance from a parent and color */
        UMaterialInstanceDynamic* CreateColoredMID(UMaterialInterface* ParentMat, FLinearColor Color);

        /** Apply materials to all mesh sections */
        void ApplyMaterials();

        /** Attach streets-gl PBR normal/roughness maps when available. */
        UMaterialInterface* ResolvePBRMaterial(UMaterialInterface* BaseMat);

        // ---- Vegetation ----
        void SpawnVegetation(const TArray<FTransform>& TreeTransforms,
                              const TArray<FTransform>& GrassTransforms);
        void RemoveVegetation();

        // ---- OSM 3D models (street furniture, power, landmarks) ----
        /** Spawn one instanced-static-mesh component per model type and place an
         *  instance at every OSM node, using the same centre offset (OX,OY) and
         *  ScaleFactor as the generated meshes. */
        /** Place OSM model instances. Locations are expected in the same
         *  centered local-meter space as the rest of the mesh geometry
         *  (already shifted by -OX/-OY). Optional DEM elevates each instance
         *  onto terrain the same way trees/roads are. */
        void SpawnModels(const TArray<FGeoModelInstance>& Models,
                const FHiroGeoHeightmap* DemHeightmap = nullptr,
                double OriginLatitude = 0.0, double OriginLongitude = 0.0);
        void RemoveModels();

        /** Dynamically created instanced components for placed models (one per type). */
        UPROPERTY(Transient)
        TArray<UInstancedStaticMeshComponent*> ModelInstancedComps;

        // ---- Online elevation (Esri World Elevation 3D) ----
        /** Kept alive while an async elevation fetch is in flight; released on completion. */
        TSharedPtr<FHiroGeoElevationFetcher> ElevationFetcher;

        /** Bumped on every Generate() call. Captured by the async elevation
         *  completion callback so a stale fetch from a previous Generate() can't
         *  overwrite the ground mesh of a newer one (e.g. user mashes the button). */
        uint32 CurrentGenerationId = 0;

#if WITH_DEV_AUTOMATION_TESTS
        TSharedPtr<FHiroGeoHeightmap> AutomationElevationHeightmap;
        TFunction<void()> PendingAutomationElevation;
        bool bAutomationElevationInline = false;
#endif

        /** Re-runs the building/roof/road/sidewalk/water/vegetation/ground/tree
         *  steps using a freshly fetched DEM heightmap, and re-uploads the
         *  corresponding mesh sections + respawns vegetation instances. Called
         *  from the elevation fetch's completion callback. Buildings/roofs get
         *  one terrain-sampled Z offset per footprint; roads get one per
         *  polyline vertex (so they follow slopes); water/vegetation-area
         *  meshes get one per footprint centroid (flat); scattered trees/grass
         *  and individually-tagged OSM trees get one per instance. */
        void RebuildMeshesWithHeightmap(
                const TArray<FGeoBuilding>& Buildings,
                const TArray<FGeoRoad>& Roads,
                const TArray<FGeoWater>& WaterBodies,
                const TArray<FGeoVegetation>& Vegetations,
                const TArray<FGeoTree>& Trees,
                const TArray<FGeoRailway>& Railways,
                const TArray<FGeoFence>& Fences,
                const TArray<FGeoPowerLine>& PowerLines,
                const TArray<FGeoModelInstance>& Models,
                TSharedPtr<FHiroGeoHeightmap> Heightmap,
                double OriginLatitude,
                double OriginLongitude,
                uint32 ForGenerationId);
};