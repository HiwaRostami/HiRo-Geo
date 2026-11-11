// Copyright Hiro. All Rights Reserved.


#include "HiroGeoActor.h"
#include "GeoMeshGenerator.h"
#include "OSMLoader.h"
#include "GeoJSONLoader.h"
#include "HiroGeoElevationFetcher.h"
#include "HiroGeoGenerationGate.h"
#include "UObject/ConstructorHelpers.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture.h"
#include "EngineUtils.h"


// ============================================================================
// Constructor
// ============================================================================
AHiroGeoActor::AHiroGeoActor()
{
        PrimaryActorTick.bCanEverTick = true;
        PrimaryActorTick.bStartWithTickEnabled = true;

        ScaleFactor        = 100.0f;
        GroundOffset       = 0.0f;
        bUseGeoJSON        = true;
        bUseVertexColorMaterial = true;
        b3DWindow          = false;
        bUseTerrainElevation = true;
        TerrainGridSubdivisions = 48;
        bShowRailways      = true;
        bShowPOIMarkers    = true;
        bUsePBRMaterials   = true;
        bUseElevationDiskCache = true;
        bEnableDistanceLOD = true;
        LODCullDistanceCm  = 25000.f;
        POIMarkerRadius    = 80.f;
        POIMarkerHeight    = 250.f;
        TreeDensityScale   = 1.0f;
        GrassDensityScale  = 1.0f;
        MaxTreeInstances   = 2000;
        MaxGrassInstances  = 5000;
        ModelScale         = 1.0f;
        ModelBaseRotation  = FRotator(0.f, 0.f, 90.f); // stand up Y-up GLB models

        // ---------- Root ----------
        DefaultRoot = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultRoot"));
        RootComponent = DefaultRoot;

        // ---------- Procedural Mesh Components ----------
        auto MakePMC = [&](const TCHAR* Name) -> UProceduralMeshComponent*
        {
                UProceduralMeshComponent* C = CreateDefaultSubobject<UProceduralMeshComponent>(Name);
                C->SetupAttachment(RootComponent);
                // These are decorative procedurally-generated meshes, not meant to
                // contribute to the Recast navmesh; registering them with the nav
                // octree also triggers "Empty bounds" warnings whenever a section is
                // briefly cleared/rebuilt (e.g. ClearAllMeshSections during a rebuild).
                C->SetCanEverAffectNavigation(false);
                return C;
        };

        BuildingMeshComp   = MakePMC(TEXT("BuildingMesh"));
        RoadMeshComp       = MakePMC(TEXT("RoadMesh"));
        SidewalkMeshComp   = MakePMC(TEXT("SidewalkMesh"));
        WaterMeshComp      = MakePMC(TEXT("WaterMesh"));
        GroundMeshComp     = MakePMC(TEXT("GroundMesh"));
        RoofMeshComp       = MakePMC(TEXT("RoofMesh"));
        VegetationMeshComp = MakePMC(TEXT("VegetationMesh"));
        RailwayMeshComp    = MakePMC(TEXT("RailwayMesh"));
        FenceMeshComp      = MakePMC(TEXT("FenceMesh"));
        POIMeshComp        = MakePMC(TEXT("POIMesh"));

        // ---------- Instanced Static Mesh Components ----------
        TreeInstancedComp = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TreeInstanced"));
        TreeInstancedComp->SetupAttachment(RootComponent);
        TreeInstancedComp->SetMobility(EComponentMobility::Movable);

        GrassInstancedComp = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("GrassInstanced"));
        GrassInstancedComp->SetupAttachment(RootComponent);
        GrassInstancedComp->SetMobility(EComponentMobility::Movable);

        // ---------- Default Material Assets (imported from streets-gl) ----------
        // These live in the plugin Content folder under /HiroGeo/Materials/.
        BuildingWallMaterial    = nullptr;
        BuildingGlassMaterial   = nullptr;
        BrickFacadeMaterial     = nullptr;
        DarkStoneFacadeMaterial = nullptr;
        LightStoneFacadeMaterial= nullptr;
        RoofMaterial            = nullptr;
        RoadMaterial            = nullptr;
        SidewalkMaterial        = nullptr;
        WaterMaterial           = nullptr;
        GroundGrassMaterial     = nullptr;
        VegetationMaterial      = nullptr;
        RailwayBallastMaterial  = nullptr;
        RailwayRailMaterial     = nullptr;
        POIMaterial             = nullptr;

        static ConstructorHelpers::FObjectFinder<UMaterialInterface> BuildingMat(
                TEXT("/HiroGeo/Materials/M_Building.M_Building"));
        if (BuildingMat.Succeeded()) BuildingWallMaterial = BuildingMat.Object;

        static ConstructorHelpers::FObjectFinder<UMaterialInterface> RoofMat(
                TEXT("/HiroGeo/Materials/M_Roof.M_Roof"));
        if (RoofMat.Succeeded()) RoofMaterial = RoofMat.Object;

        static ConstructorHelpers::FObjectFinder<UMaterialInterface> RoadMat(
                TEXT("/HiroGeo/Materials/M_Road.M_Road"));
        if (RoadMat.Succeeded()) RoadMaterial = RoadMat.Object;

        // M_Area covers projected ground areas: sidewalks/pavement and vegetation.
        static ConstructorHelpers::FObjectFinder<UMaterialInterface> AreaMat(
                TEXT("/HiroGeo/Materials/M_Area.M_Area"));
        if (AreaMat.Succeeded()) { SidewalkMaterial = AreaMat.Object; VegetationMaterial = AreaMat.Object; }

        static ConstructorHelpers::FObjectFinder<UMaterialInterface> WaterMat(
                TEXT("/HiroGeo/Materials/M_Water.M_Water"));
        if (WaterMat.Succeeded()) WaterMaterial = WaterMat.Object;

        static ConstructorHelpers::FObjectFinder<UMaterialInterface> TerrainMat(
                TEXT("/HiroGeo/Materials/M_Terrain.M_Terrain"));
        if (TerrainMat.Succeeded()) GroundGrassMaterial = TerrainMat.Object;

        // No dedicated railway material is shipped; use road asphalt by default.
        // A missing FObjectFinder in a CDO constructor is logged as an engine error,
        // so do not probe a known-absent asset here.
        RailwayBallastMaterial = RoadMaterial;
        RailwayRailMaterial = RoadMaterial;

        static ConstructorHelpers::FObjectFinder<UStaticMesh> TreeMeshAsset(
                TEXT("StaticMesh'/HiroGeo/Models/Trees/PCG_Tree_02.PCG_Tree_02'"));
        if (TreeMeshAsset.Succeeded()) { TreeMesh = TreeMeshAsset.Object; TreeInstancedComp->SetStaticMesh(TreeMesh); }

        static ConstructorHelpers::FObjectFinder<UStaticMesh> GrassMeshAsset(
                TEXT("StaticMesh'/HiroGeo/Models/Grass/GRASS.GRASS'"));
        if (GrassMeshAsset.Succeeded()) { GrassMesh = GrassMeshAsset.Object; GrassInstancedComp->SetStaticMesh(GrassMesh); }
}

// ============================================================================
// BeginPlay
// ============================================================================
void AHiroGeoActor::BeginPlay()
{
        Super::BeginPlay();
        SetActorTickEnabled(bEnableDistanceLOD);
}

void AHiroGeoActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
        ++CurrentGenerationId;
#if WITH_DEV_AUTOMATION_TESTS
        PendingAutomationElevation = nullptr;
#endif
        if (ElevationFetcher.IsValid())
        {
                ElevationFetcher->Cancel();
                ElevationFetcher.Reset();
        }
        Super::EndPlay(EndPlayReason);
}

#if WITH_DEV_AUTOMATION_TESTS
void AHiroGeoActor::SetAutomationElevation(TSharedPtr<FHiroGeoHeightmap> Heightmap, bool bCompleteInline)
{
        AutomationElevationHeightmap = MoveTemp(Heightmap);
        bAutomationElevationInline = bCompleteInline;
        PendingAutomationElevation = nullptr;
}

bool AHiroGeoActor::CompleteAutomationElevation()
{
        if (!PendingAutomationElevation)
        {
                return false;
        }
        TFunction<void()> Completion = MoveTemp(PendingAutomationElevation);
        Completion();
        return true;
}
#endif

void AHiroGeoActor::Tick(float DeltaSeconds)
{
        Super::Tick(DeltaSeconds);
        if (bEnableDistanceLOD)
        {
                UpdateDistanceLOD();
        }
}

void AHiroGeoActor::UpdateDistanceLOD()
{
        // UE stand-in for streets-gl camera-distance tile LOD: cull decorative
        // detail beyond LODCullDistanceCm while keeping core city meshes.
        bool bHaveCam = false;
        FVector CamLoc = GetActorLocation();
        if (UWorld* World = GetWorld())
        {
                if (APlayerController* PC = World->GetFirstPlayerController())
                {
                        FVector ViewLoc; FRotator ViewRot;
                        PC->GetPlayerViewPoint(ViewLoc, ViewRot);
                        CamLoc = ViewLoc;
                        bHaveCam = true;
                }
        }
        // No gameplay camera (pure editor preview) → keep detail visible.
        if (!bHaveCam) return;

        const float DistSq = FVector::DistSquared(CamLoc, GetActorLocation());
        const float CullSq = LODCullDistanceCm * LODCullDistanceCm;
        const bool bNear = DistSq <= CullSq;

        auto SetVis = [bNear](UPrimitiveComponent* C)
        {
                if (C) C->SetVisibility(bNear, true);
        };
        SetVis(POIMeshComp);
        SetVis(FenceMeshComp);
        SetVis(VegetationMeshComp);
        SetVis(TreeInstancedComp);
        SetVis(GrassInstancedComp);
        for (UInstancedStaticMeshComponent* C : ModelInstancedComps)
                SetVis(C);
}

// ============================================================================
// PostActorCreated - force newly placed actors to the world origin
// ============================================================================
void AHiroGeoActor::PostActorCreated()
{
        Super::PostActorCreated();

        // Only when first created in the editor (drag-drop / placement). Actors
        // streamed in from a saved map go through PostLoad instead, so their saved
        // transform is preserved. Runtime game spawns are left untouched.
        if (UWorld* W = GetWorld())
        {
                if (!W->IsGameWorld())
                        SetActorTransform(FTransform::Identity);
        }
}

// ============================================================================
// LoadDefaultAssets
// ============================================================================
void AHiroGeoActor::LoadDefaultAssets()
{
        auto TryLoad = [](UMaterialInterface*& Mat, const TCHAR* Path)
        {
                if (!Mat) Mat = LoadObject<UMaterialInterface>(nullptr, Path);
        };

        // Imported streets-gl materials live under /HiroGeo/Materials/.
        TryLoad(BuildingWallMaterial,    TEXT("/HiroGeo/Materials/M_Building.M_Building"));
        TryLoad(RoofMaterial,            TEXT("/HiroGeo/Materials/M_Roof.M_Roof"));
        TryLoad(RoadMaterial,            TEXT("/HiroGeo/Materials/M_Road.M_Road"));
        TryLoad(SidewalkMaterial,        TEXT("/HiroGeo/Materials/M_Area.M_Area"));
        TryLoad(WaterMaterial,           TEXT("/HiroGeo/Materials/M_Water.M_Water"));
        TryLoad(GroundGrassMaterial,     TEXT("/HiroGeo/Materials/M_Terrain.M_Terrain"));
        TryLoad(VegetationMaterial,      TEXT("/HiroGeo/Materials/M_Area.M_Area"));
        TryLoad(BuildingGlassMaterial,   TEXT("/HiroGeo/Materials/M_Glass.M_Glass"));
        // No dedicated railway material in Content yet — fall back to road asphalt.
        if (!RailwayBallastMaterial) RailwayBallastMaterial = RoadMaterial;
        if (!RailwayRailMaterial)    RailwayRailMaterial = RoadMaterial;

        // Per-building atlas diagnostics.
        UE_LOG(LogTemp, Warning,
                TEXT("HiroGeo materials | Building=%s Roof=%s Road=%s Area=%s Water=%s Terrain=%s"),
                BuildingWallMaterial?TEXT("OK"):TEXT("MISSING"),
                RoofMaterial?TEXT("OK"):TEXT("MISSING"),
                RoadMaterial?TEXT("OK"):TEXT("MISSING"),
                SidewalkMaterial?TEXT("OK"):TEXT("MISSING"),
                WaterMaterial?TEXT("OK"):TEXT("MISSING"),
                GroundGrassMaterial?TEXT("OK"):TEXT("MISSING"));

        if (!TreeMesh)
        {
                TreeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("StaticMesh'/HiroGeo/Models/Trees/PCG_Tree_02.PCG_Tree_02'"));
                if (TreeMesh && TreeInstancedComp) TreeInstancedComp->SetStaticMesh(TreeMesh);
        }
        if (!GrassMesh)
        {
                GrassMesh = LoadObject<UStaticMesh>(nullptr, TEXT("StaticMesh'/HiroGeo/Models/Grass/GRASS.GRASS'"));
                if (GrassMesh && GrassInstancedComp) GrassInstancedComp->SetStaticMesh(GrassMesh);
        }
}

// ============================================================================
// Material creation helpers
// ============================================================================
UMaterialInterface* AHiroGeoActor::CreateSolidColorMaterial()
{
        UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr,
                TEXT("Material'/HiroGeo/Material/M_SolidColor.M_SolidColor'"));
        if (!M)
                UE_LOG(LogTemp, Warning, TEXT("HiroGeo: M_SolidColor not found. "
                        "Create: VectorParameter 'Color'→BaseColor, ScalarParameter 'Roughness'=0.7, "
                        "Opaque, Default Lit, Two Sided."));
        return M;
}

UMaterialInterface* AHiroGeoActor::CreateVertexColorMaterial()
{
        // Plain vertex-colour material that lives in the plugin Content folder.
        UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr,
                TEXT("/HiroGeo/Materials/M_VertexColor.M_VertexColor"));
        if (!M)
                M = LoadObject<UMaterialInterface>(nullptr,
                        TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
        return M;
}

UMaterialInterface* AHiroGeoActor::CreateGlassMaterial()
{
        return LoadObject<UMaterialInterface>(nullptr,
                TEXT("Material'/HiroGeo/Material/Glass/Glass.Glass'"));
}

UMaterialInstanceDynamic* AHiroGeoActor::CreateColoredMID(UMaterialInterface* ParentMat, FLinearColor Color)
{
        if (!ParentMat) return nullptr;
        UMaterialInterface* ResolvedParent = ResolvePBRMaterial(ParentMat);
        UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(ResolvedParent, this);
        if (MID)
        {
                MID->SetVectorParameterValue(FName("Color"),     Color);
                MID->SetVectorParameterValue(FName("BaseColor"), Color);
        }
        return MID;
}

// ============================================================================
// ResolvePBRMaterial — bind streets-gl normal/ORM maps when present
// ============================================================================
UMaterialInterface* AHiroGeoActor::ResolvePBRMaterial(UMaterialInterface* BaseMat)
{
        if (!bUsePBRMaterials || !BaseMat) return BaseMat;

        UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMat, this);
        if (!MID) return BaseMat;

        auto TryTex = [&](const TCHAR* Param, const TCHAR* Path)
        {
                if (UTexture* Tex = LoadObject<UTexture>(nullptr, Path))
                        MID->SetTextureParameterValue(FName(Param), Tex);
        };

        // streets-gl building facade PBR (parameter names match imported M_Building).
        // Atlas normals live under /HiroGeo/Atlas/ (TA_BuildingNormal), not /Textures/.
        TryTex(TEXT("NormalTexture"),   TEXT("/HiroGeo/Atlas/TA_BuildingNormal.TA_BuildingNormal"));
        TryTex(TEXT("NormalMap"),        TEXT("/HiroGeo/Atlas/TA_BuildingNormal.TA_BuildingNormal"));
        TryTex(TEXT("NormalTexture"),   TEXT("/HiroGeo/Textures/T_BuildingNormal.T_BuildingNormal"));
        TryTex(TEXT("RoughnessMap"),     TEXT("/HiroGeo/Textures/T_BuildingRoughness.T_BuildingRoughness"));
        TryTex(TEXT("ORMTexture"),       TEXT("/HiroGeo/Textures/T_BuildingORM.T_BuildingORM"));
        TryTex(TEXT("MetallicMap"),      TEXT("/HiroGeo/Textures/T_BuildingORM.T_BuildingORM"));

        return MID;
}

// ============================================================================
// ApplyMaterials
// ============================================================================
void AHiroGeoActor::ApplyMaterials()
{
    LoadDefaultAssets();
    
    UMaterialInterface* VCMat = CreateVertexColorMaterial();

    auto Resolve = [&](UMaterialInterface* Override, UMaterialInterface* Textured) -> UMaterialInterface*
    {
        if (Override) return Override;
        if (Textured) return Textured;
        return VCMat;
    };

    auto ApplyAll = [&](UProceduralMeshComponent* Comp, UMaterialInterface* Mat)
    {
        if (!Comp || !Mat) return;
        const int32 NumSec = Comp->GetNumSections();
        for (int32 s = 0; s < FMath::Max(1, NumSec); s++)
            Comp->SetMaterial(s, Mat);
    };

    UMaterialInterface* BuildingMat = ResolvePBRMaterial(Resolve(WallMaterialOverride, BuildingWallMaterial));
    UMaterialInterface* RoofMat     = ResolvePBRMaterial(Resolve(RoofMaterialOverride, RoofMaterial));
    UMaterialInterface* RoadMat     = ResolvePBRMaterial(Resolve(RoadMaterialOverride, RoadMaterial));
    UMaterialInterface* WaterMat    = Resolve(WaterMaterialOverride, WaterMaterial);
    UMaterialInterface* GroundMat   = ResolvePBRMaterial(Resolve(GroundMaterialOverride, GroundGrassMaterial));
    UMaterialInterface* SidewalkMat = ResolvePBRMaterial(Resolve(nullptr, SidewalkMaterial));
    UMaterialInterface* VegMat      = Resolve(nullptr, VegetationMaterial);
    UMaterialInterface* RailwayMat    = Resolve(RailwayMaterialOverride, RailwayBallastMaterial);
    UMaterialInterface* POIMat      = bUseVertexColorMaterial ? VCMat : Resolve(POIMaterialOverride, VCMat);

    // Buildings: Apply base material to all sections first
    ApplyAll(BuildingMeshComp, BuildingMat);

    // ========================================================================
    // 3DWindow toggle — manage ALL window-related sections:
    //   Section 1 = GlassData (3D glass for tall commercial buildings)
    //   Section 2 = BrickData (facade panels)
    //   Section 3 = DarkData (facade panels)
    //   Section 4 = LightData (facade panels)
    //   Section 5 = ColorWallData (towers/churches - OSM color only)
    //   Section 6 = WindowData (flat window texture panels)    [b3DWindow=false]
    //   Section 7 = WindowFrameData (3D window frame bars)     [b3DWindow=true]
    //   Section 8 = WindowGlassData (3D window glass panes)    [b3DWindow=true]
    // ========================================================================
    if (BuildingMeshComp && BuildingMeshComp->GetNumSections() > 1)
    {
        // Load M_Glass material for 3D window mode
        UMaterialInterface* GlassMat = BuildingGlassMaterial
            ? BuildingGlassMaterial
            : LoadObject<UMaterialInterface>(nullptr, TEXT("/HiroGeo/Materials/M_Glass.M_Glass"));
        UMaterialInterface* FinalGlassMat = GlassMat ? GlassMat : BuildingMat;

        // Section 0: WallData — always building material
        BuildingMeshComp->SetMaterial(0, BuildingMat);

        // Section 1: Glass curtain-walls (tall commercial buildings)
        if (BuildingMeshComp->GetNumSections() > 1)
        {
            BuildingMeshComp->SetMaterial(1, FinalGlassMat);
            BuildingMeshComp->SetMeshSectionVisible(1, true);
        }

        // Section 2,3,4: Facade panels — always building material
        for (int32 Sec : {2, 3, 4})
        {
            if (BuildingMeshComp->GetNumSections() > Sec)
            {
                BuildingMeshComp->SetMaterial(Sec, BuildingMat);
                BuildingMeshComp->SetMeshSectionVisible(Sec, true);
            }
        }

        // Section 5: Color-only buildings (towers/churches)
        if (BuildingMeshComp->GetNumSections() > 5)
        {
            BuildingMeshComp->SetMaterial(5, BuildingMat);
            BuildingMeshComp->SetMeshSectionVisible(5, true);
        }

        if (b3DWindow)
        {
            // === 3D WINDOW MODE ===
            // Section 6: Flat window panels — HIDE
            if (BuildingMeshComp->GetNumSections() > 6)
            {
                BuildingMeshComp->SetMeshSectionVisible(6, false);
            }
            // Section 7: 3D window frame bars — SHOW with building material (facade texture)
            if (BuildingMeshComp->GetNumSections() > 7)
            {
                BuildingMeshComp->SetMaterial(7, BuildingMat);
                BuildingMeshComp->SetMeshSectionVisible(7, true);
            }
            // Section 8: 3D window glass panes — SHOW with M_Glass
            if (BuildingMeshComp->GetNumSections() > 8)
            {
                BuildingMeshComp->SetMaterial(8, FinalGlassMat);
                BuildingMeshComp->SetMeshSectionVisible(8, true);
            }
        }
        else
        {
            // === FLAT WINDOW MODE ===
            // Section 6: Flat window panels — SHOW with building material
            // (reads WindowSlice from UV1 → TA_BuildingDiffuse atlas window texture)
            if (BuildingMeshComp->GetNumSections() > 6)
            {
                BuildingMeshComp->SetMaterial(6, BuildingMat);
                BuildingMeshComp->SetMeshSectionVisible(6, true);
            }
            // Section 7: 3D window frame bars — HIDE
            if (BuildingMeshComp->GetNumSections() > 7)
            {
                BuildingMeshComp->SetMeshSectionVisible(7, false);
            }
            // Section 8: 3D window glass panes — HIDE
            if (BuildingMeshComp->GetNumSections() > 8)
            {
                BuildingMeshComp->SetMeshSectionVisible(8, false);
            }
        }
    }

    ApplyAll(RoadMeshComp,     RoadMat);
    ApplyAll(SidewalkMeshComp, SidewalkMat);
    ApplyAll(WaterMeshComp,    WaterMat);
    ApplyAll(GroundMeshComp,   GroundMat);
    ApplyAll(RoofMeshComp,     RoofMat);
    ApplyAll(VegetationMeshComp, VegMat);

    if (RailwayMeshComp)
    {
        if (RailwayMeshComp->GetNumSections() > 0)
                RailwayMeshComp->SetMaterial(0, RailwayMat ? RailwayMat : RoadMat);
        if (RailwayMeshComp->GetNumSections() > 1)
                RailwayMeshComp->SetMaterial(1, RailwayRailMaterial ? RailwayRailMaterial : RoadMat);
    }
    if (FenceMeshComp)
    {
        UMaterialInterface* FenceMat = bUseVertexColorMaterial ? VCMat : (RailwayMat ? RailwayMat : RoadMat);
        for (int32 Sec = 0; Sec < FenceMeshComp->GetNumSections(); Sec++)
                FenceMeshComp->SetMaterial(Sec, FenceMat ? FenceMat : VCMat);
    }
    ApplyAll(POIMeshComp, POIMat);
}

// ============================================================================
// SpawnVegetation / RemoveVegetation
// ============================================================================
void AHiroGeoActor::SpawnVegetation(
        const TArray<FTransform>& TreeTransforms,
        const TArray<FTransform>& GrassTransforms)
{
        RemoveVegetation();

        if (TreeMesh && TreeInstancedComp)
        {
                TreeInstancedComp->SetStaticMesh(TreeMesh);
                TreeInstancedComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
                TreeInstancedComp->SetCollisionProfileName(TEXT("BlockAll"));
                int32 Cap = FMath::Min(TreeTransforms.Num(), MaxTreeInstances);
                // Local instances so vegetation moves with the actor when the
                // importer places multi-tile cities at non-zero world offsets.
                // AddInstanceWorldSpace previously left trees/grass at local
                // coords interpreted as world space, so tiles away from origin
                // had vegetation stranded at the wrong place.
                for (int32 i=0;i<Cap;i++) TreeInstancedComp->AddInstance(TreeTransforms[i]);
                UE_LOG(LogTemp, Log, TEXT("HiroGeo: Trees %d/%d"), Cap, TreeTransforms.Num());
        }
        if (GrassMesh && GrassInstancedComp)
        {
                GrassInstancedComp->SetStaticMesh(GrassMesh);
                // Dense grass instances — keep off physics (visual only).
                GrassInstancedComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                GrassInstancedComp->SetCollisionProfileName(TEXT("NoCollision"));
                int32 Cap = FMath::Min(GrassTransforms.Num(), MaxGrassInstances);
                for (int32 Index = 0; Index < Cap; ++Index)
                {
                        const int32 SampleIndex = static_cast<int64>(Index) * GrassTransforms.Num() / Cap;
                        GrassInstancedComp->AddInstance(GrassTransforms[SampleIndex]);
                }
                UE_LOG(LogTemp, Log, TEXT("HiroGeo: Grass %d/%d"), Cap, GrassTransforms.Num());
        }
}

void AHiroGeoActor::RemoveVegetation()
{
        if (TreeInstancedComp)  TreeInstancedComp->ClearInstances();
        if (GrassInstancedComp) GrassInstancedComp->ClearInstances();
}

// ============================================================================
// SpawnModels / RemoveModels - place OSM 3D models (street furniture, power,
// landmarks) from /HiroGeo/Models/, one instanced component per model type.
// ============================================================================
namespace
{
        // Folder name == model type; the StaticMesh asset inside is named after the
        // folder, except the two statues.
        FString HiroGeoModelMeshPath(const FString& Type)
        {
                FString Mesh = Type;
                if (Type == TEXT("statue_0"))      Mesh = TEXT("the_thinker");
                else if (Type == TEXT("statue_1")) Mesh = TEXT("kentaur");
                return FString::Printf(TEXT("/HiroGeo/Models/%s/%s.%s"), *Type, *Mesh, *Mesh);
        }
}

void AHiroGeoActor::RemoveModels()
{
        for (UInstancedStaticMeshComponent* C : ModelInstancedComps)
                if (C) C->DestroyComponent();
        ModelInstancedComps.Empty();
}

void AHiroGeoActor::SpawnModels(
        const TArray<FGeoModelInstance>& Models,
        const FHiroGeoHeightmap* DemHeightmap,
        double OriginLatitude, double OriginLongitude)
{
        RemoveModels();
        if (Models.Num() == 0) return;

        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const double RefElev = bHaveDem
                ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : 0.0;
        const double EarthRadiusM = 6371000.0;
        const double CosOriginLat = FMath::Cos(OriginLatitude * UE_PI / 180.0);

        // One instanced component per model type (cached while iterating).
        TMap<FString, UInstancedStaticMeshComponent*> ByType;

        int32 Placed = 0;
        for (const FGeoModelInstance& M : Models)
        {
                if (!ByType.Contains(M.ModelType))
                {
                        UInstancedStaticMeshComponent* NewISM = nullptr;
                        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *HiroGeoModelMeshPath(M.ModelType));
                        if (Mesh)
                        {
                                NewISM = NewObject<UInstancedStaticMeshComponent>(this);
                                NewISM->SetupAttachment(RootComponent);
                                NewISM->SetMobility(EComponentMobility::Movable);
                                NewISM->RegisterComponent();
                                NewISM->SetStaticMesh(Mesh);
                                // Street furniture / poles / landmarks — collide when the
                                // mesh asset has collision geometry; otherwise this is a no-op.
                                NewISM->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
                                NewISM->SetCollisionProfileName(TEXT("BlockAll"));
                                ModelInstancedComps.Add(NewISM);
                        }
                        else
                        {
                                UE_LOG(LogTemp, Warning, TEXT("HiroGeo: model mesh missing: %s"),
                                        *HiroGeoModelMeshPath(M.ModelType));
                        }
                        ByType.Add(M.ModelType, NewISM);
                }

                UInstancedStaticMeshComponent* ISM = ByType.FindRef(M.ModelType);
                if (!ISM) continue;

                // Locations are already in centered local-meter space (same as
                // buildings/roads). Y negated to match GeoMeshGenerator.
                const float WX = M.Location.X * ScaleFactor;
                const float WY = -M.Location.Y * ScaleFactor;

                float WZ = GroundOffset;
                if (bHaveDem)
                {
                        const double Lat = OriginLatitude + ((double)M.Location.Y / EarthRadiusM) * 180.0 / UE_PI;
                        const double Lon = OriginLongitude + ((double)M.Location.X / (EarthRadiusM * FMath::Max(CosOriginLat, 0.0001))) * 180.0 / UE_PI;
                        WZ = GroundOffset + (float)((DemHeightmap->SampleAtLatLon(Lat, Lon) - RefElev) * ScaleFactor);
                }

                float Yaw;
                if (M.RotationDeg >= 0.f)
                {
                        Yaw = -M.RotationDeg; // OSM bearing -> UE yaw (approx)
                }
                else
                {
                        // Deterministic pseudo-random yaw from position.
                        const int32 H = FMath::Abs((int32)(M.Location.X * 13.13f + M.Location.Y * 7.77f));
                        Yaw = (float)(H % 360);
                }

                // Most models import already upright (no base correction). A few
                // lie on their side and need a -90° roll to stand up. The heading
                // yaw about world Z is then applied on top.
                const bool bNeedsUpright =
                        (M.ModelType == TEXT("hydrant") || M.ModelType == TEXT("ad_column") ||
                        M.ModelType == TEXT("tower_crane"));
                const FQuat BaseQ = bNeedsUpright
                        ? FRotator(0.f, 0.f, -90.f).Quaternion()
                        : FQuat::Identity;

                // Per-type extra yaw correction on top of BaseQ, once the model
                // is confirmed standing upright. Leave at 0 until upright is verified.
                float ExtraYawDeg = 0.f;
                const FQuat ExtraYawQ(FVector::UpVector, FMath::DegreesToRadians(ExtraYawDeg));

                const FQuat YawQ(FVector::UpVector, FMath::DegreesToRadians(Yaw));
                const FTransform Xf(
                        YawQ * ExtraYawQ * BaseQ,
                        FVector(WX, WY, WZ),
                        FVector(ModelScale));
                ISM->AddInstance(Xf);
                ++Placed;
        }

        UE_LOG(LogTemp, Log, TEXT("HiroGeo: placed %d OSM models across %d type(s)"),
                Placed, ModelInstancedComps.Num());
}

// ============================================================================
// Generate
// ============================================================================
void AHiroGeoActor::Generate()
{
        LoadDefaultAssets();
        Clear();

        // Invalidate any elevation fetch still in flight from a previous Generate()
        // call so its completion callback can't clobber this run's ground mesh.
        const uint32 ThisGenerationId = ++CurrentGenerationId;
#if WITH_DEV_AUTOMATION_TESTS
        PendingAutomationElevation = nullptr;
#endif
        if (ElevationFetcher.IsValid())
        {
                ElevationFetcher->Cancel();
        }
        ElevationFetcher.Reset();

        // ---------- Load geodata ----------
        TArray<FGeoBuilding>   Buildings;
        TArray<FGeoRoad>       Roads;
        TArray<FGeoWater>      WaterBodies;
        TArray<FGeoPOI>        POIs;
        TArray<FGeoVegetation> Vegetations;
        TArray<FGeoModelInstance> ModelInstances;
        TArray<FGeoRailway> Railways;
        TArray<FGeoFence> Fences;
        TArray<FGeoPowerLine> PowerLines;
        TArray<FGeoTree> Trees;
        TArray<FElevationSample> ElevationSamples;

        // Captured from FOSMLoader before it goes out of scope below — needed both
        // to drive the online elevation fetch and to translate the local-meter grid
        // back to lat/lon for DEM sampling. Only populated for the OSM path.
        bool bHaveOsmBounds = false;
        double OsmMinLat = 0.0, OsmMaxLat = 0.0, OsmMinLon = 0.0, OsmMaxLon = 0.0;
        double OsmCenterLat = 0.0, OsmCenterLon = 0.0;

        if (bUseGeoJSON && !GeoJSONFilePath.IsEmpty())
        {
                FGeoJSONLoader Loader;
                if (!Loader.LoadGeoJSON(GeoJSONFilePath))
                {
                        UE_LOG(LogTemp, Error, TEXT("HiroGeo: Failed to load GeoJSON: %s"), *GeoJSONFilePath);
                        return;
                }
                Buildings       = Loader.GetBuildings();
                Roads           = Loader.GetRoads();
                WaterBodies     = Loader.GetWaters();
                POIs            = Loader.GetPOIs();
                Vegetations     = Loader.GetVegetations();
                ModelInstances  = Loader.GetModelInstances();
                Railways        = Loader.GetRailways();
                Trees           = Loader.GetTrees();
                ElevationSamples= Loader.GetElevationSamples();

                // GeoJSON is also projected to local meters around the feature
                // centroid — capture the same bounds the OSM path uses so grid
                // clipping / DEM sampling can invert meters -> lat/lon correctly
                // (previously bHaveOsmBounds stayed false and ToLatLon treated
                // meter X/Y as degrees, wiping the cell).
                bHaveOsmBounds = true;
                OsmMinLat = Loader.GetMinLat();
                OsmMaxLat = Loader.GetMaxLat();
                OsmMinLon = Loader.GetMinLon();
                OsmMaxLon = Loader.GetMaxLon();
                OsmCenterLat = Loader.GetCenterLatitude();
                OsmCenterLon = Loader.GetCenterLongitude();
        }
        else if (!OSMFilePath.IsEmpty())
        {
                FOSMLoader Loader;
                if (!Loader.LoadOSMFile(OSMFilePath))
                {
                        UE_LOG(LogTemp, Error, TEXT("HiroGeo: Failed to load OSM: %s"), *OSMFilePath);
                        return;
                }
                Buildings       = Loader.GetBuildings();
                Roads           = Loader.GetRoads();
                WaterBodies     = Loader.GetWaters();
                POIs            = Loader.GetPOIs();
                Vegetations     = Loader.GetVegetations();
                ModelInstances  = Loader.GetModelInstances();
                Railways        = Loader.GetRailways();
                Fences          = Loader.GetFences();
                PowerLines      = Loader.GetPowerLines();
                Trees           = Loader.GetTrees();
                ElevationSamples= Loader.GetElevationSamples();

                bHaveOsmBounds = true;
                OsmMinLat = Loader.GetMinLat();
                OsmMaxLat = Loader.GetMaxLat();
                OsmMinLon = Loader.GetMinLon();
                OsmMaxLon = Loader.GetMaxLon();
                OsmCenterLat = Loader.GetCenterLatitude();
                OsmCenterLon = Loader.GetCenterLongitude();
        }
        else
        {
                UE_LOG(LogTemp, Warning, TEXT("HiroGeo: No data file specified"));
                return;
        }

        // ---------- نکته ۲: فیلتر داده‌ها به اندازه Grid ----------

        // Raw (uncentered, no-margin) ground-rect bounds for the selected grid
        // cell — populated below when HasGridBounds(), then shifted by -OX/-OY
        // once that centering offset is known, and stored on the actor so the
        // ground mesh is exactly the requested cell size (see GridGroundMinX
        // etc. in the header) instead of being sized off the data's own extent.
        bool bHaveGroundGridBoundsRaw = false;
        double GroundRectMinX_Raw = 0.0, GroundRectMaxX_Raw = 0.0;
        double GroundRectMinY_Raw = 0.0, GroundRectMaxY_Raw = 0.0;

        if (HasGridBounds())
        {
                UE_LOG(LogTemp, Log, TEXT("HiroGeo: Clipping geodata to grid bounds [%.6f,%.6f] - [%.6f,%.6f]"),
                        GridMinLat, GridMinLon, GridMaxLat, GridMaxLon);

                const double CGridMinLat = GridMinLat;
                const double CGridMinLon = GridMinLon;
                const double CGridMaxLat = GridMaxLat;
                const double CGridMaxLon = GridMaxLon;

                // Grid's own true lat/lon centre — only used for the "polygon fully
                // encloses the grid" fallback test below, NOT as a meter-projection
                // origin (see bug note further down).
                const double CCenterLat = (CGridMinLat + CGridMaxLat) * 0.5;
                const double CCenterLon = (CGridMinLon + CGridMaxLon) * 0.5;
                const double CDegToRad = 3.14159265358979323846 / 180.0;

                // 5% padding, expressed directly in degrees now (see bug note below).
                const double CMarginLat = (CGridMaxLat - CGridMinLat) * 0.05;
                const double CMarginLon = (CGridMaxLon - CGridMinLon) * 0.05;

                // BUG FIX: Buildings/Roads/WaterBodies/etc. coming out of FOSMLoader
                // (B.Center, B.Nodes, R.Points, ...) are already projected to local
                // meters relative to OsmCenterLat/OsmCenterLon — NOT raw lat/lon
                // degrees (see FOSMLoader::LatLonToLocal, and the same *ScaleFactor
                // usage throughout GeoMeshGenerator.cpp, e.g. its RAW-local-meters
                // building-centroid comment). The old code compared these meter
                // values directly against degree-based grid bounds, so every point
                // failed the check and Buildings/Roads/etc. were always zeroed out
                // after grid-clip (B:0/235 etc. in the log). The GeoJSON path
                // doesn't go through that projection, so its points are passed
                // through unchanged (bHaveOsmBounds is only set for the OSM path).
                const double CEarthRadiusM = 6371000.0;
                const double CCosOsmCenterLat = FMath::Cos(OsmCenterLat * CDegToRad);

                auto ToLatLon = [&](double X, double Y, double& OutLat, double& OutLon)
                {
                        if (bHaveOsmBounds)
                        {
                                OutLat = OsmCenterLat + (Y / CEarthRadiusM) * (180.0 / UE_PI);
                                OutLon = OsmCenterLon + (X / (CEarthRadiusM * FMath::Max(CCosOsmCenterLat, 0.0001))) * (180.0 / UE_PI);
                        }
                        else
                        {
                                OutLat = X;
                                OutLon = Y;
                        }
                };

                auto IsPointInGrid = [&](double X, double Y) -> bool
                {
                        double LatDeg, LonDeg;
                        ToLatLon(X, Y, LatDeg, LonDeg);
                        return (LatDeg >= CGridMinLat - CMarginLat && LatDeg <= CGridMaxLat + CMarginLat &&
                                LonDeg >= CGridMinLon - CMarginLon && LonDeg <= CGridMaxLon + CMarginLon);
                };

                // Ray-casting point-in-polygon test (Lat treated as X, Lon as Y — same
                // planar approach FGeoMeshGenerator's vegetation scatter code uses).
                // Polygon points go through the same ToLatLon conversion as above for
                // the same reason.
                auto IsLatLonInsidePolygon = [&](double Lat, double Lon, const TArray<FVector2D>& Poly) -> bool
                {
                        const int32 N = Poly.Num();
                        bool bInside = false;
                        for (int32 i = 0, j = N - 1; i < N; j = i++)
                        {
                                double LatI, LonI, LatJ, LonJ;
                                ToLatLon(Poly[i].X, Poly[i].Y, LatI, LonI);
                                ToLatLon(Poly[j].X, Poly[j].Y, LatJ, LonJ);
                                if (((LonI > Lon) != (LonJ > Lon)) &&
                                        (Lat < (LatJ - LatI) * (Lon - LonI) / (LonJ - LonI) + LatI))
                                {
                                        bInside = !bInside;
                                }
                        }
                        return bInside;
                };

                auto IsPolygonInGrid = [&](const TArray<FVector2D>& Points) -> bool
                {
                        for (const FVector2D& P : Points)
                        {
                                if (IsPointInGrid(P.X, P.Y)) return true;
                        }
                        // Edge case: a polygon that fully ENCLOSES the grid (so none of its
                        // own vertices land inside the grid bounds) should still count as
                        // intersecting — check whether the grid's centre point falls inside
                        // THIS polygon, instead of the previous (always-true) check of
                        // whether the grid centre falls inside the grid itself.
                        if (Points.Num() >= 3 && IsLatLonInsidePolygon(CCenterLat, CCenterLon, Points))
                                return true;
                        return false;
                };

                // ---------------------------------------------------------------
                // BUG FIX: IsPolygonInGrid() above is an all-or-nothing test — if
                // even ONE vertex of a water/vegetation polygon lands inside the
                // grid, the WHOLE original polygon (which for a river/lake/forest
                // can run for kilometres) was kept untouched. That (a) drew a
                // huge, wrongly-shaped water mesh stretching far outside the
                // selected cell, (b) fed those far-away points into
                // ComputeDataBounds(), inflating the ground plane way past the
                // intended 500x500m grid, and (c) made the water's single
                // centroid-sampled elevation (GenerateWaterMeshes) land somewhere
                // with unrelated terrain height, so water sat far above/below
                // the actual ground.
                //
                // Fix: actually CLIP each polygon to the grid rectangle
                // (Sutherland-Hodgman) instead of an include/exclude test, in the
                // same raw local-meter space IsPointInGrid operates in (i.e.
                // BEFORE the OX/OY re-centring done further below).
                // ---------------------------------------------------------------
                auto ClipPolygonToRect = [](const TArray<FVector2D>& Poly,
                        double RMinX, double RMaxX, double RMinY, double RMaxY) -> TArray<FVector2D>
                {
                        auto ClipEdge = [](const TArray<FVector2D>& In, int32 Axis, double Limit, bool bKeepLessEq) -> TArray<FVector2D>
                        {
                                TArray<FVector2D> Out;
                                const int32 N = In.Num();
                                if (N == 0) return Out;
                                auto Inside = [&](const FVector2D& P) -> bool
                                {
                                        const double V = (Axis == 0) ? P.X : P.Y;
                                        return bKeepLessEq ? (V <= Limit) : (V >= Limit);
                                };
                                auto Intersect = [&](const FVector2D& A, const FVector2D& B) -> FVector2D
                                {
                                        const double AV = (Axis == 0) ? A.X : A.Y;
                                        const double BV = (Axis == 0) ? B.X : B.Y;
                                        const double T = (BV != AV) ? (Limit - AV) / (BV - AV) : 0.0;
                                        return FVector2D(A.X + (B.X - A.X) * T, A.Y + (B.Y - A.Y) * T);
                                };
                                for (int32 i = 0; i < N; i++)
                                {
                                        const FVector2D& Cur  = In[i];
                                        const FVector2D& Prev = In[(i + N - 1) % N];
                                        const bool bCurIn  = Inside(Cur);
                                        const bool bPrevIn = Inside(Prev);
                                        if (bCurIn)
                                        {
                                                if (!bPrevIn) Out.Add(Intersect(Prev, Cur));
                                                Out.Add(Cur);
                                        }
                                        else if (bPrevIn)
                                        {
                                                Out.Add(Intersect(Prev, Cur));
                                        }
                                }
                                return Out;
                        };

                        TArray<FVector2D> Result = Poly;
                        Result = ClipEdge(Result, 0, RMinX, /*bKeepLessEq*/ false); // X >= MinX
                        Result = ClipEdge(Result, 0, RMaxX, /*bKeepLessEq*/ true);  // X <= MaxX
                        Result = ClipEdge(Result, 1, RMinY, /*bKeepLessEq*/ false); // Y >= MinY
                        Result = ClipEdge(Result, 1, RMaxY, /*bKeepLessEq*/ true);  // Y <= MaxY
                        return Result;
                };

                // NOTE: a margin-inclusive clip rect (CGridMin±CMargin) used to be
                // computed here and passed to every ClipPolygonToRect/
                // ClipPolylineToRect call below. That let water/road/vegetation/
                // railway geometry survive up to CMargin past the actual cell edge.
                // That was harmless while GenerateGroundMesh also padded the ground
                // by ~50m, but once the ground was tightened to the exact cell
                // (see GridGroundMinX/Max below), that leftover margin sliver was
                // left hanging past the ground's new edge — visible as water/rails
                // floating in the void right at tile boundaries. Every clip call now
                // uses GroundRectMinX_Raw/Max/Y (no margin) instead, below.

                // GroundRectMinX_Raw/MaxX_Raw/MinY_Raw/MaxY_Raw below: the exact
                // requested cell footprint (e.g. exactly 500x500m), no margin —
                // used both for ground sizing and for clipping every other feature.
                bHaveGroundGridBoundsRaw = true;
                if (bHaveOsmBounds)
                {
                        GroundRectMinY_Raw = (CGridMinLat - OsmCenterLat) * CEarthRadiusM * UE_PI / 180.0;
                        GroundRectMaxY_Raw = (CGridMaxLat - OsmCenterLat) * CEarthRadiusM * UE_PI / 180.0;
                        GroundRectMinX_Raw = (CGridMinLon - OsmCenterLon) * CEarthRadiusM * FMath::Max(CCosOsmCenterLat, 0.0001) * UE_PI / 180.0;
                        GroundRectMaxX_Raw = (CGridMaxLon - OsmCenterLon) * CEarthRadiusM * FMath::Max(CCosOsmCenterLat, 0.0001) * UE_PI / 180.0;
                }
                else
                {
                        GroundRectMinX_Raw = CGridMinLat; GroundRectMaxX_Raw = CGridMaxLat;
                        GroundRectMinY_Raw = CGridMinLon; GroundRectMaxY_Raw = CGridMaxLon;
                }

                int32 OrigB = Buildings.Num(), OrigR = Roads.Num(), OrigW = WaterBodies.Num();
                int32 OrigV = Vegetations.Num(), OrigRail = Railways.Num(), OrigP = POIs.Num(), OrigT = Trees.Num();

                // ---------------------------------------------------------------
                // BUG FIX (cont'd): Roads and Railways have the exact same
                // all-or-nothing problem as Water/Vegetation did — a road or rail
                // line that only clips the grid for a short stretch (e.g. a track
                // leading out of town) was kept in its ENTIRE original length,
                // which (a) drew a stray line running far outside the selected
                // cell (visible poking out past the ground plane) and (b) fed
                // those far-away points into ComputeDataBounds(), inflating the
                // ground size again even after the water/vegetation fix.
                //
                // Polylines aren't polygons — Sutherland-Hodgman doesn't apply.
                // Clip each segment to the rect (Liang-Barsky) and chain the
                // surviving sub-segments back into one or more continuous runs,
                // since a line can leave and re-enter the rect more than once.
                // ---------------------------------------------------------------
                auto ClipPolylineToRect = [](const TArray<FVector2D>& Line,
                        double RMinX, double RMaxX, double RMinY, double RMaxY) -> TArray<TArray<FVector2D>>
                {
                        TArray<TArray<FVector2D>> Runs;
                        if (Line.Num() < 2) return Runs;

                        auto LiangBarsky = [&](FVector2D A, FVector2D B, FVector2D& OutA, FVector2D& OutB) -> bool
                        {
                                const double Dx = B.X - A.X, Dy = B.Y - A.Y;
                                double T0 = 0.0, T1 = 1.0;
                                const double P[4] = { -Dx, Dx, -Dy, Dy };
                                const double Q[4] = { A.X - RMinX, RMaxX - A.X, A.Y - RMinY, RMaxY - A.Y };
                                for (int32 i = 0; i < 4; i++)
                                {
                                        if (FMath::IsNearlyZero(P[i]))
                                        {
                                                if (Q[i] < 0.0) return false;
                                        }
                                        else
                                        {
                                                const double R = Q[i] / P[i];
                                                if (P[i] < 0.0) { if (R > T1) return false; if (R > T0) T0 = R; }
                                                else            { if (R < T0) return false; if (R < T1) T1 = R; }
                                        }
                                }
                                OutA = FVector2D(A.X + T0 * Dx, A.Y + T0 * Dy);
                                OutB = FVector2D(A.X + T1 * Dx, A.Y + T1 * Dy);
                                return true;
                        };

                        TArray<FVector2D> Current;
                        for (int32 i = 0; i < Line.Num() - 1; i++)
                        {
                                FVector2D CA, CB;
                                if (LiangBarsky(Line[i], Line[i + 1], CA, CB))
                                {
                                        const bool bContiguous = Current.Num() > 0 && FVector2D::DistSquared(Current.Last(), CA) < 1.0;
                                        if (!bContiguous)
                                        {
                                                if (Current.Num() >= 2) Runs.Add(Current);
                                                Current.Reset();
                                                Current.Add(CA);
                                        }
                                        Current.Add(CB);
                                }
                                else if (Current.Num() >= 2)
                                {
                                        Runs.Add(Current);
                                        Current.Reset();
                                }
                                else
                                {
                                        Current.Reset();
                                }
                        }
                        if (Current.Num() >= 2) Runs.Add(Current);
                        return Runs;
                };

                Buildings = Buildings.FilterByPredicate([&](const FGeoBuilding& B)
                {
                        if (IsPointInGrid(B.Center.X, B.Center.Y)) return true;
                        return IsPolygonInGrid(B.Nodes);
                });

                {
                        TArray<FGeoRoad> ClippedRoads;
                        ClippedRoads.Reserve(Roads.Num());
                        for (const FGeoRoad& R : Roads)
                        {
                                TArray<TArray<FVector2D>> Runs = ClipPolylineToRect(R.Points, GroundRectMinX_Raw, GroundRectMaxX_Raw, GroundRectMinY_Raw, GroundRectMaxY_Raw);
                                for (TArray<FVector2D>& Run : Runs)
                                {
                                        FGeoRoad NR = R;
                                        NR.Points = MoveTemp(Run);
                                        ClippedRoads.Add(MoveTemp(NR));
                                }
                        }
                        Roads = MoveTemp(ClippedRoads);
                }

                {
                        TArray<FGeoWater> ClippedWater;
                        ClippedWater.Reserve(WaterBodies.Num());
                        for (const FGeoWater& W : WaterBodies)
                        {
                                if (W.bIsClosed)
                                {
                                        // Lake/pond/sea: closed polygon -> Sutherland-Hodgman clip (original behaviour).
                                        TArray<FVector2D> Clipped = ClipPolygonToRect(W.Points, GroundRectMinX_Raw, GroundRectMaxX_Raw, GroundRectMinY_Raw, GroundRectMaxY_Raw);
                                        if (Clipped.Num() >= 3)
                                        {
                                                FGeoWater NW = W;
                                                NW.Points = MoveTemp(Clipped);
                                                ClippedWater.Add(MoveTemp(NW));
                                        }
                                }
                                else
                                {
                                        // River/stream/canal: open line -> Liang-Barsky polyline clip, same as
                                        // roads/railways below (Sutherland-Hodgman would wrongly wrap it into a ring).
                                        TArray<TArray<FVector2D>> Runs = ClipPolylineToRect(W.Points, GroundRectMinX_Raw, GroundRectMaxX_Raw, GroundRectMinY_Raw, GroundRectMaxY_Raw);
                                        for (TArray<FVector2D>& Run : Runs)
                                        {
                                                FGeoWater NW = W;
                                                NW.Points = MoveTemp(Run);
                                                ClippedWater.Add(MoveTemp(NW));
                                        }
                                }
                        }
                        WaterBodies = MoveTemp(ClippedWater);
                }

                {
                        TArray<FGeoVegetation> ClippedVeg;
                        ClippedVeg.Reserve(Vegetations.Num());
                        for (FGeoVegetation V : Vegetations)
                        {
                                TArray<FVector2D> Clipped = ClipPolygonToRect(V.Points, GroundRectMinX_Raw, GroundRectMaxX_Raw, GroundRectMinY_Raw, GroundRectMaxY_Raw);
                                if (Clipped.Num() >= 3)
                                {
                                        V.Points = MoveTemp(Clipped);
                                        ClippedVeg.Add(MoveTemp(V));
                                }
                        }
                        Vegetations = MoveTemp(ClippedVeg);
                }

                {
                        TArray<FGeoRailway> ClippedRail;
                        ClippedRail.Reserve(Railways.Num());
                        for (const FGeoRailway& Ry : Railways)
                        {
                                TArray<TArray<FVector2D>> Runs = ClipPolylineToRect(Ry.Points, GroundRectMinX_Raw, GroundRectMaxX_Raw, GroundRectMinY_Raw, GroundRectMaxY_Raw);
                                for (TArray<FVector2D>& Run : Runs)
                                {
                                        FGeoRailway NRy = Ry;
                                        NRy.Points = MoveTemp(Run);
                                        ClippedRail.Add(MoveTemp(NRy));
                                }
                        }
                        Railways = MoveTemp(ClippedRail);
                }

                {
                        TArray<FGeoFence> ClippedFence;
                        ClippedFence.Reserve(Fences.Num());
                        for (const FGeoFence& F : Fences)
                        {
                                TArray<TArray<FVector2D>> Runs = ClipPolylineToRect(F.Points, GroundRectMinX_Raw, GroundRectMaxX_Raw, GroundRectMinY_Raw, GroundRectMaxY_Raw);
                                for (TArray<FVector2D>& Run : Runs)
                                {
                                        FGeoFence NF = F;
                                        NF.Points = MoveTemp(Run);
                                        ClippedFence.Add(MoveTemp(NF));
                                }
                        }
                        Fences = MoveTemp(ClippedFence);
                }

                {
                        TArray<FGeoPowerLine> ClippedPL;
                        ClippedPL.Reserve(PowerLines.Num());
                        for (const FGeoPowerLine& PL : PowerLines)
                        {
                                TArray<TArray<FVector2D>> Runs = ClipPolylineToRect(PL.Points, GroundRectMinX_Raw, GroundRectMaxX_Raw, GroundRectMinY_Raw, GroundRectMaxY_Raw);
                                for (TArray<FVector2D>& Run : Runs)
                                {
                                        FGeoPowerLine NPL = PL;
                                        NPL.Points = MoveTemp(Run);
                                        ClippedPL.Add(MoveTemp(NPL));
                                }
                        }
                        PowerLines = MoveTemp(ClippedPL);
                }

                POIs = POIs.FilterByPredicate([&](const FGeoPOI& P)
                {
                        return IsPointInGrid(P.Location.X, P.Location.Y);
                });

                Trees = Trees.FilterByPredicate([&](const FGeoTree& T)
                {
                        return IsPointInGrid(T.Location.X, T.Location.Y);
                });

                ModelInstances = ModelInstances.FilterByPredicate([&](const FGeoModelInstance& M)
                {
                        return IsPointInGrid(M.Location.X, M.Location.Y);
                });

                UE_LOG(LogTemp, Log, TEXT("HiroGeo: Grid clip - B:%d/%d R:%d/%d W:%d/%d V:%d/%d Rail:%d/%d P:%d/%d T:%d/%d Models:%d"),
                        Buildings.Num(), OrigB, Roads.Num(), OrigR, WaterBodies.Num(), OrigW,
                        Vegetations.Num(), OrigV, Railways.Num(), OrigRail, POIs.Num(), OrigP, Trees.Num(), OrigT,
                        ModelInstances.Num());
        }

        UE_LOG(LogTemp, Log, TEXT("HiroGeo: Loaded B=%d R=%d W=%d P=%d V=%d Rail=%d Trees=%d Ele=%d"),
                Buildings.Num(), Roads.Num(), WaterBodies.Num(), POIs.Num(), Vegetations.Num(),
                Railways.Num(), Trees.Num(), ElevationSamples.Num());

        // ---------- Centre-offset (double precision) ----------
        double DMinX=DBL_MAX,DMaxX=-DBL_MAX,DMinY=DBL_MAX,DMaxY=-DBL_MAX;
        auto ExpandD = [&](double X, double Y)
        {
                if(X<DMinX)DMinX=X; if(X>DMaxX)DMaxX=X;
                if(Y<DMinY)DMinY=Y; if(Y>DMaxY)DMaxY=Y;
        };
        for (const FGeoBuilding& B : Buildings)
                for (const FVector2D& N : B.Nodes) ExpandD(N.X, N.Y);
        for (const FGeoRoad& R : Roads)
                for (const FVector2D& P : R.Points) ExpandD(P.X, P.Y);
        for (const FGeoWater& W : WaterBodies)
                for (const FVector2D& P : W.Points) ExpandD(P.X, P.Y);
        for (const FGeoVegetation& V : Vegetations)
                for (const FVector2D& P : V.Points) ExpandD(P.X, P.Y);
        for (const FGeoRailway& Ry : Railways)
                for (const FVector2D& P : Ry.Points) ExpandD(P.X, P.Y);
        for (const FGeoFence& F : Fences)
                for (const FVector2D& P : F.Points) ExpandD(P.X, P.Y);
        for (const FGeoPowerLine& PL : PowerLines)
                for (const FVector2D& P : PL.Points) ExpandD(P.X, P.Y);
        for (const FGeoPOI& P : POIs) ExpandD(P.Location.X, P.Location.Y);
        for (const FGeoTree& T : Trees) ExpandD(T.Location.X, T.Location.Y);
        for (const FGeoModelInstance& M : ModelInstances) ExpandD(M.Location.X, M.Location.Y);
        for (const FElevationSample& E : ElevationSamples) ExpandD(E.Location.X, E.Location.Y);

        float OX=0.f, OY=0.f;
        if (bHaveGroundGridBoundsRaw && bHaveOsmBounds)
        {
                // Grid mode: centre local (0,0) on the requested cell centre, not the
                // data centroid nor the whole-OSM-file OsmCenter — otherwise RefElev is
                // sampled outside the cell (e.g. Origin south of the selected bbox) and
                // every layer shares a wrong vertical zero.
                OX = (float)((GroundRectMinX_Raw + GroundRectMaxX_Raw) * 0.5);
                OY = (float)((GroundRectMinY_Raw + GroundRectMaxY_Raw) * 0.5);
        }
        else if (DMinX<DMaxX && DMinY<DMaxY)
        {
                OX = (float)((DMinX+DMaxX)*0.5);
                OY = (float)((DMinY+DMaxY)*0.5);
        }

        // The OX/OY shift below re-centres everything to local (0,0). For the DEM
        // sampling path we need to know what lat/lon that new (0,0) corresponds to,
        // which is FOSMLoader's own local-meter origin (OsmCenterLat/Lon) offset by
        // (OX,OY) — i.e. FOSMLoader::LatLonToLocal() inverted.
        double OriginLatitude = OsmCenterLat, OriginLongitude = OsmCenterLon;
        if (bHaveGroundGridBoundsRaw && bHaveOsmBounds)
        {
                OriginLatitude = (GridMinLat + GridMaxLat) * 0.5;
                OriginLongitude = (GridMinLon + GridMaxLon) * 0.5;
        }
        else if (bHaveOsmBounds)
        {
                const double EarthRadiusM = 6371000.0;
                OriginLatitude  = OsmCenterLat + ((double)OY / EarthRadiusM) * 180.0 / UE_PI;
                OriginLongitude = OsmCenterLon + ((double)OX / (EarthRadiusM * FMath::Cos(OsmCenterLat * UE_PI / 180.0))) * 180.0 / UE_PI;
        }

        // مهم: این مقدار همان نقطه‌ی جغرافیایی واقعی است که محل (0,0,0) محلی این
        // مش به آن اشاره می‌کند. SHiroGeoImporterWindow باید برای جای‌گذاری صحیح
        // این اکتور در دنیا (نسبت به سایر کاشی‌های گرید) از همین مقدار استفاده
        // کند، نه از مرکز bbox درخواستی — چون این دو نقطه معمولاً یکی نیستند.
        MeshOriginLatitude = OriginLatitude;
        MeshOriginLongitude = OriginLongitude;

        // همان OX/OY که زیر همه‌ی هندسه (ساختمان، جاده، ...) را به مرکز (0,0)
        // می‌برد، باید روی مستطیل دقیق سلول گرید هم اعمال شود تا زمین هم در همان
        // فضای محلی Centered باشد که سایر مش‌ها هستند.
        bHasGridGroundBounds = bHaveGroundGridBoundsRaw;
        if (bHasGridGroundBounds)
        {
                GridGroundMinX = (float)(GroundRectMinX_Raw - OX);
                GridGroundMaxX = (float)(GroundRectMaxX_Raw - OX);
                GridGroundMinY = (float)(GroundRectMinY_Raw - OY);
                GridGroundMaxY = (float)(GroundRectMaxY_Raw - OY);
        }

        // Shift all coordinates so centre = (0,0)
        auto ShiftNodes = [&](TArray<FVector2D>& Arr)
        { for (FVector2D& P : Arr) { P.X -= OX; P.Y -= OY; } };

        for (FGeoBuilding& B : Buildings)
        {
                ShiftNodes(B.Nodes);
                for (TArray<FVector2D>& Hole : B.Holes) ShiftNodes(Hole);
                if (B.Nodes.Num() > 0)
                {
                        FVector2D S(0,0);
                        for (const FVector2D& N : B.Nodes) S += N;
                        B.Center = S / (float)B.Nodes.Num();
                }
        }
        for (FGeoRoad& R : Roads)       ShiftNodes(R.Points);
        for (FGeoWater& W : WaterBodies)
        {
                ShiftNodes(W.Points);
                for (TArray<FVector2D>& Hole : W.Holes) ShiftNodes(Hole);
        }
        for (FGeoVegetation& V : Vegetations)
        {
                ShiftNodes(V.Points);
                for (TArray<FVector2D>& Hole : V.Holes) ShiftNodes(Hole);
        }
        for (FGeoRailway& Ry : Railways) ShiftNodes(Ry.Points);
        for (FGeoFence& F : Fences) ShiftNodes(F.Points);
        for (FGeoPowerLine& PL : PowerLines) ShiftNodes(PL.Points);
        for (FGeoPOI& P : POIs) { P.Location.X -= OX; P.Location.Y -= OY; }
        for (FGeoTree& T : Trees) { T.Location.X -= OX; T.Location.Y -= OY; }
        for (FGeoModelInstance& M : ModelInstances) { M.Location.X -= OX; M.Location.Y -= OY; }
        for (FElevationSample& E : ElevationSamples) { E.Location.X -= OX; E.Location.Y -= OY; }

        // ---------- Generate meshes ----------
        FGeoMeshGenerator MeshGen;
        MeshGen.ScaleFactor = ScaleFactor;

        // Buildings
        // Per-building terrain offset from the sparse OSM "ele"/GeoJSON-Z elevation
        // samples — mirrors what RebuildMeshesWithHeightmap does with a real DEM, so
        // buildings sit on the same terrain the ground mesh below them is deformed
        // to instead of floating on/sinking into it (no online DEM is available yet
        // at this point in Generate(), hence DemHeightmap=nullptr here).
        TArray<float> BuildingZOffsets = FGeoMeshGenerator::ComputeBuildingTerrainOffsets(
                Buildings, ScaleFactor, /*DemHeightmap*/ nullptr, 0.0, 0.0, &ElevationSamples);

        FGeoMeshGenerator::FMeshData WallData, GlassData, BrickData, DarkData, LightData, ColorWallData, WindowData;
        FGeoMeshGenerator::FMeshData WindowFrameData, WindowGlassData;
        MeshGen.GenerateBuildingMeshes(Buildings, WallData, GlassData, BrickData, DarkData, LightData, ColorWallData, WindowData,
                WindowFrameData, WindowGlassData, &BuildingZOffsets);

        static const TArray<FVector2D> EmptyUV;
        static const TArray<FVector> EmptyV3;
        static const TArray<FColor> EmptyCol;
        static const TArray<FProcMeshTangent> EmptyTang;

        auto CreateSection = [&](UProceduralMeshComponent* Comp, int32 Sec,
                                  FGeoMeshGenerator::FMeshData& D, bool bCollision)
        {
                // ALWAYS create the section — even with zero vertices — so that
                // section indices remain contiguous (PMC requires 0..N-1 with no
                // gaps).  Empty sections are invisible and cost nothing.
                if (D.Vertices.Num() > 0)
                {
                        Comp->CreateMeshSection(Sec,
                                D.Vertices, D.Triangles, D.Normals,
                                D.UV0, D.UV1, EmptyUV, EmptyUV,
                                D.VertexColors, D.Tangents, bCollision);
                }
                else
                {
                        Comp->CreateMeshSection(Sec,
                                EmptyV3, TArray<int32>(), EmptyV3,
                                EmptyUV, EmptyUV, EmptyUV, EmptyUV,
                                EmptyCol, EmptyTang, bCollision);
                }
        };

        // Collision on solid wall sections so characters cannot walk through
        // buildings. Thin decorative 3D window frame/pane sections stay off.
        CreateSection(BuildingMeshComp, 0, WallData,      true);
        CreateSection(BuildingMeshComp, 1, GlassData,     true);  // curtain-wall glass (solid facade)
        CreateSection(BuildingMeshComp, 2, BrickData,     true);
        CreateSection(BuildingMeshComp, 3, DarkData,      true);
        CreateSection(BuildingMeshComp, 4, LightData,     true);
        CreateSection(BuildingMeshComp, 5, ColorWallData, true);  // tower/church walls (OSM colour)
        CreateSection(BuildingMeshComp, 6, WindowData,    true);  // wall-inset windows (*_Window atlas slice)
        CreateSection(BuildingMeshComp, 7, WindowFrameData, false); // 3D window frame bars (thin)
        CreateSection(BuildingMeshComp, 8, WindowGlassData, false); // 3D window glass panes (thin)
        if (BuildingMeshComp)
        {
                BuildingMeshComp->SetCollisionProfileName(TEXT("BlockAll"));
        }

        // Roads
        FGeoMeshGenerator::FMeshData RoadData, SWData;
        MeshGen.GenerateRoadMeshes(Roads, RoadData, SWData, nullptr, 0.0, 0.0, &ElevationSamples);
        CreateSection(RoadMeshComp,     0, RoadData, true);
        CreateSection(SidewalkMeshComp, 0, SWData,   true);
        if (RoadMeshComp)     RoadMeshComp->SetCollisionProfileName(TEXT("BlockAll"));
        if (SidewalkMeshComp) SidewalkMeshComp->SetCollisionProfileName(TEXT("BlockAll"));

        // Water — در پاس synchronous بدون ElevationSamples صاف می‌ماند
        // (همان دلیل زمین: IDW پراکنده روی centroid دریاچه/رودخانه مقدار غلط می‌دهد
        //  و آب را بالاتر از زمین صاف می‌برد؛ DEM واقعی آن را درست می‌کند)
        FGeoMeshGenerator::FMeshData WaterData;
        MeshGen.GenerateWaterMeshes(WaterBodies, WaterData, nullptr, 0.0, 0.0, nullptr);
        CreateSection(WaterMeshComp, 0, WaterData, false);

        // Ground (flat or terrain-deformed from ele / GeoJSON Z) — this synchronous
        // pass uses the sparse OSM "ele" tags (if any) and always renders immediately.
        FGeoMeshGenerator::FMeshData GroundData;
        // زمین در پاس synchronous صاف می‌ماند تا DEM آنلاین برسد.
        // اگر از همین الان bUseTerrainElevation=true داشته باشیم، GenerateGroundMesh
        // سعی می‌کند با ElevationSamples (تگ‌های ele پراکنده) زمین را deform کند —
        // اما ساختمان‌ها و جاده‌ها در همین مرحله IDW کافی برای centroid خودشان
        // ندارند و روی Z=0 می‌مانند. نتیجه: زمین بالا می‌رود ولی بقیه صاف می‌مانند،
        // ساختمان‌ها زیر زمین می‌روند یا جاده‌ها معلق می‌شوند.
        // وقتی DEM واقعی از Esri می‌رسد (RebuildMeshesWithHeightmap)، همه‌ی
        // feature‌ها با هم و از همان RefElev دقیقاً re-elevated می‌شوند.
        MeshGen.GenerateGroundMesh(Buildings, Roads, WaterBodies, Vegetations,
                ElevationSamples, GroundData, GroundOffset, /*bUseTerrainElevation=*/ false, TerrainGridSubdivisions,
                nullptr, 0.0, 0.0,
                bHasGridGroundBounds, GridGroundMinX, GridGroundMaxX, GridGroundMinY, GridGroundMaxY);
        CreateSection(GroundMeshComp, 0, GroundData, true);
        if (GroundMeshComp) GroundMeshComp->SetCollisionProfileName(TEXT("BlockAll"));

        // Cache hits can complete inline, before Generate() has emitted roofs,
        // railways, vegetation, and instances. Gate completion until the entire
        // flat pass is done so cache and HTTP paths have the same ordering.
        TSharedRef<FHiroGeoGenerationGate> ElevationGate = MakeShared<FHiroGeoGenerationGate>();

        // If enabled, kick off an elevation fetch and rebuild the affected
        // meshes once it arrives — see RebuildMeshesWithHeightmap().
        if (bFetchOnlineElevation && bUseTerrainElevation && bHaveOsmBounds)
        {
                TWeakObjectPtr<AHiroGeoActor> WeakThis(this);
                TArray<FGeoBuilding> CapturedBuildings = Buildings;
                TArray<FGeoRoad> CapturedRoads = Roads;
                TArray<FGeoWater> CapturedWaterBodies = WaterBodies;
                TArray<FGeoVegetation> CapturedVegetations = Vegetations;
                TArray<FGeoTree> CapturedTrees = Trees;
                TArray<FGeoRailway> CapturedRailways = Railways;
                TArray<FGeoFence> CapturedFences = Fences;
                TArray<FGeoPowerLine> CapturedPowerLines = PowerLines;
                TArray<FGeoModelInstance> CapturedModels = ModelInstances;

                double FetchMinLat = OsmMinLat, FetchMinLon = OsmMinLon;
                double FetchMaxLat = OsmMaxLat, FetchMaxLon = OsmMaxLon;
                if (HasGridBounds())
                {
                        // درخواست تایل‌های ارتفاع باید دقیقاً همون باکسی باشه که داده‌های
                        // OSM هم باهاش clip شدن (GridMinLat/...), نه bounding box کل فایل
                        // OSM لود‌شده — وگرنه GroundMesh به‌مراتب بزرگ‌تر از انتخاب کاربر می‌شه.
                        FetchMinLat = GridMinLat; FetchMinLon = GridMinLon;
                        FetchMaxLat = GridMaxLat; FetchMaxLon = GridMaxLon;
                }

                FHiroGeoElevationFetcher::FOnComplete ElevationComplete =
                        FHiroGeoElevationFetcher::FOnComplete::CreateLambda(
                                [WeakThis, CapturedBuildings, CapturedRoads, CapturedWaterBodies, CapturedVegetations,
                                 CapturedTrees, CapturedRailways, CapturedFences, CapturedPowerLines, CapturedModels,
                                 OriginLatitude, OriginLongitude, ThisGenerationId, ElevationGate]
                                (bool bSuccess, TSharedPtr<FHiroGeoHeightmap> Heightmap)
                                {
                                        ElevationGate->Submit(
                                        [WeakThis, CapturedBuildings, CapturedRoads, CapturedWaterBodies, CapturedVegetations,
                                         CapturedTrees, CapturedRailways, CapturedFences, CapturedPowerLines, CapturedModels,
                                         OriginLatitude, OriginLongitude, ThisGenerationId, bSuccess, Heightmap]
                                        ()
                                        {
                                                if (AHiroGeoActor* Actor = WeakThis.Get())
                                                {
                                                        if (bSuccess && Heightmap.IsValid())
                                                        {
                                                                Actor->RebuildMeshesWithHeightmap(
                                                                        CapturedBuildings, CapturedRoads, CapturedWaterBodies, CapturedVegetations,
                                                                        CapturedTrees, CapturedRailways, CapturedFences, CapturedPowerLines,
                                                                        CapturedModels, Heightmap,
                                                                        OriginLatitude, OriginLongitude, ThisGenerationId);
                                                        }
                                                        else
                                                        {
                                                                UE_LOG(LogTemp, Warning, TEXT("HiroGeo: online elevation fetch failed — keeping the OSM \"ele\"-tag ground mesh."));
                                                                if (GEngine)
                                                                {
                                                                        GEngine->AddOnScreenDebugMessage(
                                                                                INDEX_NONE, 8.f, FColor::Yellow,
                                                                                TEXT("HiroGeo: online elevation is incomplete or unavailable; keeping the existing ground mesh."));
                                                                }
                                                        }
                                                        Actor->ElevationFetcher.Reset();
                                                }
                                        });
                                });

#if WITH_DEV_AUTOMATION_TESTS
                if (AutomationElevationHeightmap.IsValid())
                {
                        TSharedPtr<FHiroGeoHeightmap> ControlledHeightmap = AutomationElevationHeightmap;
                        if (bAutomationElevationInline)
                        {
                                ElevationComplete.Execute(true, ControlledHeightmap);
                        }
                        else
                        {
                                PendingAutomationElevation =
                                        [ElevationComplete, ControlledHeightmap]() mutable
                                        {
                                                ElevationComplete.Execute(true, ControlledHeightmap);
                                        };
                        }
                }
                else
#endif
                {
                        ElevationFetcher = FHiroGeoElevationFetcher::FetchForBounds(
                                FetchMinLat, FetchMinLon, FetchMaxLat, FetchMaxLon, ElevationZoomLevel,
                                ElevationComplete, bUseElevationDiskCache);
                }
                if (ElevationFetcher.IsValid() && ElevationFetcher->IsFinished())
                {
                        ElevationFetcher.Reset();
                }
        }

        // Railways — در پاس synchronous هم صاف (بدون ElevationSamples)
        if (bShowRailways)
        {
                FGeoMeshGenerator::FMeshData BallastData, RailStripData;
                MeshGen.GenerateRailwayMeshes(Railways, BallastData, RailStripData,
                        nullptr, 0.0, 0.0, nullptr);
                CreateSection(RailwayMeshComp, 0, BallastData, false);
                CreateSection(RailwayMeshComp, 1, RailStripData, false);
        }

        // Fences / walls / hedges + power-line wires + pitch markings
        {
                FGeoMeshGenerator::FMeshData FenceData, WireData, PitchData;
                MeshGen.GenerateFenceMeshes(Fences, FenceData, nullptr, 0.0, 0.0, nullptr);
                MeshGen.GeneratePowerLineMeshes(PowerLines, WireData, nullptr, 0.0, 0.0, nullptr, &ModelInstances);
                MeshGen.GeneratePitchMarkings(Vegetations, PitchData, nullptr, 0.0, 0.0, nullptr);
                CreateSection(FenceMeshComp, 0, FenceData, true);  // fences/walls/hedges
                CreateSection(FenceMeshComp, 1, WireData, false);  // powerline wires (thin)
                CreateSection(FenceMeshComp, 2, PitchData, false); // pitch markings (flat decals)
                if (FenceMeshComp) FenceMeshComp->SetCollisionProfileName(TEXT("BlockAll"));
        }

        // POI markers
        FGeoMeshGenerator::FMeshData POIData;
        if (bShowPOIMarkers)
        {
                MeshGen.GeneratePOIMarkers(POIs, POIData, POIMarkerRadius, POIMarkerHeight);
                CreateSection(POIMeshComp, 0, POIData, false);
        }

        // Roofs  ← flat roofs (atlas) in section 0, shaped/landmark roofs (OSM colour) in section 1
        // Same BuildingZOffsets array/order as the walls above, so roofs stay flush
        // on top of their own building instead of detaching from it.
        FGeoMeshGenerator::FMeshData RoofData, RoofColorData;
        MeshGen.GenerateRoofMeshes(Buildings, RoofData, RoofColorData, &BuildingZOffsets);
        CreateSection(RoofMeshComp, 0, RoofData,      true);
        CreateSection(RoofMeshComp, 1, RoofColorData, true);
        if (RoofMeshComp) RoofMeshComp->SetCollisionProfileName(TEXT("BlockAll"));

        // Vegetation
        FGeoMeshGenerator::FMeshData VegData;
        MeshGen.GenerateVegetationMeshes(Vegetations, VegData, nullptr, 0.0, 0.0, nullptr);
        CreateSection(VegetationMeshComp, 0, VegData, false);

        // ---------- Materials ----------
        ApplyMaterials();

        // ---------- Update bounds ----------
        auto UpdateB = [](UProceduralMeshComponent* C) { if (C) C->UpdateBounds(); };
        UpdateB(BuildingMeshComp); UpdateB(RoadMeshComp); UpdateB(SidewalkMeshComp);
        UpdateB(WaterMeshComp);    UpdateB(GroundMeshComp); UpdateB(RoofMeshComp);
        UpdateB(VegetationMeshComp); UpdateB(RailwayMeshComp); UpdateB(FenceMeshComp); UpdateB(POIMeshComp);

        // ---------- Vegetation instances ----------
        TArray<FTransform> TreeXf, GrassXf;
        MeshGen.GenerateVegetationTransforms(Vegetations, Buildings, Roads,
                TreeXf, GrassXf, TreeDensityScale, GrassDensityScale,
                /*DemHeightmap*/ nullptr, 0.0, 0.0, &ElevationSamples);
        FGeoMeshGenerator::AppendOSMTreeTransforms(Trees, TreeXf, ScaleFactor, GroundOffset,
                /*DemHeightmap*/ nullptr, 0.0, 0.0, &ElevationSamples);
        SpawnVegetation(TreeXf, GrassXf);

        // ---------- OSM 3D models (hydrants, benches, towers, statues, …) ----------
        SpawnModels(ModelInstances);
        UE_LOG(LogTemp, Log, TEXT("  Models=%d"), ModelInstances.Num());

        // ---------- Log summary ----------
        UE_LOG(LogTemp, Log, TEXT("HiroGeo: Done. ScaleFactor=%.0f  Centre(%.1f, %.1f)m"), ScaleFactor, OX, OY);
        UE_LOG(LogTemp, Log, TEXT("  Verts: Wall=%d Glass=%d Road=%d SW=%d Water=%d Ground=%d Roof=%d Veg=%d POI=%d"),
                WallData.Vertices.Num(), GlassData.Vertices.Num(),
                RoadData.Vertices.Num(), SWData.Vertices.Num(),
                WaterData.Vertices.Num(), GroundData.Vertices.Num(),
                RoofData.Vertices.Num(), VegData.Vertices.Num(), POIData.Vertices.Num());
        UE_LOG(LogTemp, Log, TEXT("  Trees=%d  Grass=%d"), TreeXf.Num(), GrassXf.Num());

        // Log building:part count for debugging
        int32 NumParts = 0;
        for (const FGeoBuilding& B : Buildings) if (B.bIsBuildingPart) NumParts++;
        UE_LOG(LogTemp, Log, TEXT("  Buildings=%d  (building:parts=%d)"), Buildings.Num(), NumParts);

        // Log roof shape distribution
        TMap<ERoofShape,int32> RoofCount;
        for (const FGeoBuilding& B : Buildings) RoofCount.FindOrAdd(B.RoofShape)++;
        UE_LOG(LogTemp, Log, TEXT("  Roof shapes: Flat=%d Gabled=%d Hipped=%d Pyramidal=%d Other=%d"),
                RoofCount.FindRef(ERoofShape::Flat),
                RoofCount.FindRef(ERoofShape::Gabled),
                RoofCount.FindRef(ERoofShape::Hipped),
                RoofCount.FindRef(ERoofShape::Pyramidal),
                Buildings.Num()
                  - RoofCount.FindRef(ERoofShape::Flat)
                  - RoofCount.FindRef(ERoofShape::Gabled)
                  - RoofCount.FindRef(ERoofShape::Hipped)
                  - RoofCount.FindRef(ERoofShape::Pyramidal));

        // Flush an inline cache completion only after every non-DEM section and
        // instance has been created. Async completion passes straight through.
        ElevationGate->Open();
}

// ============================================================================
// RebuildMeshesWithHeightmap
// Re-runs every generator step that GenerateGroundMesh's heightmap parameter
// also affects — buildings, roofs, roads/sidewalks, water, vegetation-area
// meshes, the ground, and scattered/tagged tree & grass instances — using a
// freshly fetched DEM heightmap, and re-uploads the corresponding mesh
// sections / respawns the vegetation instances. Called from the async
// elevation fetch's completion callback (FHiroGeoElevationFetcher), so it has
// its own short-lived FGeoMeshGenerator + CreateMeshSection calls rather than
// reusing the locals from Generate(), which has already returned by the time
// this runs.
// ============================================================================
void AHiroGeoActor::RebuildMeshesWithHeightmap(
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
        uint32 ForGenerationId)
{
        // A newer Generate() call superseded the one that started this fetch —
        // discard the result rather than overwrite the current meshes.
        if (ForGenerationId != CurrentGenerationId || !GroundMeshComp || !Heightmap.IsValid())
        {
                return;
        }

        // Always sample the DEM with the mesh origin stored at Generate() time —
        // the async callback's captured doubles can be stale if Generate() was edited.
        double SampleOriginLat = OriginLatitude;
        double SampleOriginLon = OriginLongitude;
        if (HasMeshOrigin())
        {
                SampleOriginLat = MeshOriginLatitude;
                SampleOriginLon = MeshOriginLongitude;
        }

        FGeoMeshGenerator MeshGen;
        MeshGen.ScaleFactor = ScaleFactor;
        const FHiroGeoHeightmap* Dem = Heightmap.Get();

        // ---------------------------------------------------------------------
        // TEMP DIAGNOSTIC: compare the DEM-driven Z range Ground ends up with
        // against the Z range/offsets Buildings, Roads and Water end up with,
        // so we can see numerically whether they agree (resolution-only effect)
        // or diverge (an actual bug). Safe to remove once the terrain-offset
        // mismatch is understood — search "ZDiag" to find every line.
        // ---------------------------------------------------------------------
        const double DiagRefElev = Dem->SampleAtLatLon(SampleOriginLat, SampleOriginLon);
        UE_LOG(LogTemp, Log, TEXT("HiroGeo[ZDiag] Origin=(%.6f,%.6f) RefElev=%.2fm ScaleFactor=%.1f"),
                SampleOriginLat, SampleOriginLon, DiagRefElev, ScaleFactor);
        if (HasGridBounds())
        {
                const bool bOriginInGrid =
                        SampleOriginLat >= GridMinLat && SampleOriginLat <= GridMaxLat &&
                        SampleOriginLon >= GridMinLon && SampleOriginLon <= GridMaxLon;
                if (!bOriginInGrid)
                {
                        UE_LOG(LogTemp, Warning,
                                TEXT("HiroGeo[ZDiag] Origin outside grid [%.6f,%.6f]-[%.6f,%.6f] — check MeshOrigin"),
                                GridMinLat, GridMinLon, GridMaxLat, GridMaxLon);
                }
        }

        // ---------------------------------------------------------------------
        // هم‌ترازی عمودی (Z) بین کاشی‌ها:
        // تمام vertexها local هستند: (DEM - RefElev) * ScaleFactor. بنابراین
        // ActorZ فقط باید اختلاف RefElev این tile با ارتفاع «مبدأ افقی دنیا» باشد.
        // مقدار قدیمی OriginElevation بدون marker معتبر نیست: نسخه‌های قبل آن را
        // گاهی به 0 ذخیره می‌کردند، که برای Zurich یعنی +43,000cm به هر actor و
        // tileها را بر اساس relief در هوا پخش می‌کرد. baseline فقط وقتی initialize
        // می‌شود که DEM همان OriginLatLon جهان را پوشش داده باشد.
        // ---------------------------------------------------------------------
        MeshOriginElevation = DiagRefElev;
        bHasMeshOriginElevation = true;
        if (UWorld* OwningWorld = GetWorld())
        {
                const FString Section = FString::Printf(TEXT("HiroGeoImporter.%s"), *OwningWorld->GetMapName());
            FString OriginLatLonStr;
            double WorldOriginLat = 0.0, WorldOriginLon = 0.0;
            bool bHaveWorldOriginLatLon = false;
            if (GConfig->GetString(*Section, TEXT("OriginLatLon"), OriginLatLonStr, GEditorPerProjectIni))
                {
                TArray<FString> Parts;
                OriginLatLonStr.ParseIntoArray(Parts, TEXT(","));
                if (Parts.Num() == 2)
                {
                    WorldOriginLat = FCString::Atod(*Parts[0]);
                    WorldOriginLon = FCString::Atod(*Parts[1]);
                    bHaveWorldOriginLatLon = true;
                }
                }

            // OriginLatLon is saved to 8 decimal places. One microdegree is still
            // under 12cm, comfortably tighter than a grid-cell centre.
            const bool bThisIsWorldOrigin = !bHaveWorldOriginLatLon ||
                    (FMath::IsNearlyEqual(SampleOriginLat, WorldOriginLat, 1.e-6) &&
                     FMath::IsNearlyEqual(SampleOriginLon, WorldOriginLon, 1.e-6));

            FString InitializedStr;
            const bool bBaselineInitialized =
                    GConfig->GetString(*Section, TEXT("OriginElevationInitialized"), InitializedStr, GEditorPerProjectIni) &&
                    (InitializedStr == TEXT("1") || InitializedStr.Equals(TEXT("true"), ESearchCase::IgnoreCase));

            double WorldOriginElevation = 0.0;
            FString ElevStr;
            bool bCanApplyWorldOffset = bBaselineInitialized &&
                    GConfig->GetString(*Section, TEXT("OriginElevation"), ElevStr, GEditorPerProjectIni);
            if (bCanApplyWorldOffset)
                {
                WorldOriginElevation = FCString::Atod(*ElevStr);
            }
            else if (bThisIsWorldOrigin)
            {
                // This is the only safe moment to establish the vertical datum:
                // the horizontal map origin and DEM sample describe the same point.
                WorldOriginElevation = DiagRefElev;
                        GConfig->SetString(*Section, TEXT("OriginElevation"), *FString::Printf(TEXT("%.4f"), DiagRefElev), GEditorPerProjectIni);
                GConfig->SetString(*Section, TEXT("OriginElevationInitialized"), TEXT("1"), GEditorPerProjectIni);
                        GConfig->Flush(false, GEditorPerProjectIni);
                bCanApplyWorldOffset = true;
                UE_LOG(LogTemp, Log, TEXT("HiroGeo[ZDiag] Initialized world elevation baseline at origin: %.2fm"), WorldOriginElevation);
                }

            if (bCanApplyWorldOffset)
            {
                // Apply one shared parent-space transform. All procedural meshes,
                // ISMs and dynamically-created model ISMs are attached to this
                // actor, so their vertices/instances must remain local and never
                // receive this elevation again.
                for (TActorIterator<AHiroGeoActor> It(OwningWorld); It; ++It)
                {
                    AHiroGeoActor* Tile = *It;
                    if (!Tile || !Tile->HasMeshOriginElevation()) continue;
                    FVector TileLoc = Tile->GetActorLocation();
                    const double TileDeltaElevM = Tile->GetMeshOriginElevation() - WorldOriginElevation;
                    TileLoc.Z = (float)(TileDeltaElevM * Tile->ScaleFactor);
                    Tile->SetActorLocation(TileLoc);
                }

                const double DeltaElevM = DiagRefElev - WorldOriginElevation;
                UE_LOG(LogTemp, Log, TEXT("HiroGeo[ZDiag] WorldOriginElevation=%.2fm DeltaElevM=%.2fm -> ActorZ=%.1f UE"),
                        WorldOriginElevation, DeltaElevM, GetActorLocation().Z);
            }
            else
            {
                UE_LOG(LogTemp, Warning,
                        TEXT("HiroGeo[ZDiag] Waiting for DEM of world OriginLatLon=(%.6f,%.6f); keeping this tile at its existing ActorZ=%.1f UE"),
                        WorldOriginLat, WorldOriginLon, GetActorLocation().Z);
            }
        }

        auto LogZRange = [this](const TCHAR* Label, const FGeoMeshGenerator::FMeshData& D)
        {
                if (D.Vertices.Num() == 0)
                {
                        UE_LOG(LogTemp, Log, TEXT("HiroGeo[ZDiag] %-12s: (no vertices)"), Label);
                        return;
                }
                float MinZ = D.Vertices[0].Z, MaxZ = D.Vertices[0].Z;
                for (const FVector& V : D.Vertices) { MinZ = FMath::Min(MinZ, V.Z); MaxZ = FMath::Max(MaxZ, V.Z); }
                UE_LOG(LogTemp, Log, TEXT("HiroGeo[ZDiag] %-12s: Z [%.1f .. %.1f] UE  (%.2fm .. %.2fm)"),
                        Label, MinZ, MaxZ, MinZ / ScaleFactor, MaxZ / ScaleFactor);
        };

        static const TArray<FVector2D> EmptyUV;
        static const TArray<FVector> EmptyV3;
        static const TArray<FColor> EmptyCol;
        static const TArray<FProcMeshTangent> EmptyTang;

        auto CreateSection = [&](UProceduralMeshComponent* Comp, int32 Sec,
                                  FGeoMeshGenerator::FMeshData& D, bool bCollision)
        {
                if (!Comp) return;
                if (D.Vertices.Num() > 0)
                {
                        Comp->CreateMeshSection(Sec,
                                D.Vertices, D.Triangles, D.Normals,
                                D.UV0, D.UV1, EmptyUV, EmptyUV,
                                D.VertexColors, D.Tangents, bCollision);
                }
                else
                {
                        Comp->CreateMeshSection(Sec,
                                EmptyV3, TArray<int32>(), EmptyV3,
                                EmptyUV, EmptyUV, EmptyUV, EmptyUV,
                                EmptyCol, EmptyTang, bCollision);
                }
        };

        // GenerateGroundMesh doesn't use OSM "ele" tags when a DEM is supplied (see
        // GeoMeshGenerator.cpp), so an empty array here is fine.
        static const TArray<FElevationSample> EmptyElevationSamples;
        FGeoMeshGenerator::FMeshData GroundData;
        MeshGen.GenerateGroundMesh(Buildings, Roads, WaterBodies, Vegetations,
                EmptyElevationSamples, GroundData, GroundOffset, bUseTerrainElevation, TerrainGridSubdivisions,
                Dem, SampleOriginLat, SampleOriginLon,
                bHasGridGroundBounds, GridGroundMinX, GridGroundMaxX, GridGroundMinY, GridGroundMaxY);
        LogZRange(TEXT("GroundData"), GroundData);
        CreateSection(GroundMeshComp, 0, GroundData, true);
        if (GroundMeshComp) GroundMeshComp->SetCollisionProfileName(TEXT("BlockAll"));

        const int32 GroundCells = FMath::Clamp(TerrainGridSubdivisions, 8, 256);
        const int32 GroundStride = GroundCells + 1;
        if (bUseTerrainElevation && GroundData.Vertices.Num() == GroundStride * GroundStride)
        {
                const FVector Min = GroundData.Vertices[0];
                const FVector Max = GroundData.Vertices.Last();
                if (Max.X > Min.X && Max.Y > Min.Y)
                {
                        MeshGen.SampleGroundZ = [&GroundData, Min, Max, GroundCells, GroundStride](const FVector2D& Point)
                        {
                                const double X = FMath::Clamp((Point.X - Min.X) / (Max.X - Min.X), 0.0, 1.0) * GroundCells;
                                const double Y = FMath::Clamp((Point.Y - Min.Y) / (Max.Y - Min.Y), 0.0, 1.0) * GroundCells;
                                const int32 Column = FMath::Min(FMath::FloorToInt(X), GroundCells - 1);
                                const int32 Row = FMath::Min(FMath::FloorToInt(Y), GroundCells - 1);
                                const int32 Corner = Row * GroundStride + Column;
                                const double U = X - Column;
                                const double V = Y - Row;
                                const double Z00 = GroundData.Vertices[Corner].Z;
                                const double Z11 = GroundData.Vertices[Corner + GroundStride + 1].Z;

                                // Match the two triangles emitted by GenerateGroundMesh.
                                if (U >= V)
                                        return (1.0 - U) * Z00 + (U - V) * GroundData.Vertices[Corner + 1].Z + V * Z11;
                                return (1.0 - V) * Z00 + (V - U) * GroundData.Vertices[Corner + GroundStride].Z + U * Z11;
                        };
                }
        }

        // Per-building terrain offset (one entry per Buildings element), shared
        // by walls and roofs so a building's roof never detaches from its walls.
        TArray<float> BuildingZOffsets = FGeoMeshGenerator::ComputeBuildingTerrainOffsets(
                Buildings, ScaleFactor, Dem, SampleOriginLat, SampleOriginLon);

        TArray<float> BuildingFoundationDepths;
        BuildingFoundationDepths.SetNumZeroed(Buildings.Num());
        if (MeshGen.SampleGroundZ)
        {
                for (int32 Index = 0; Index < Buildings.Num(); ++Index)
                {
                        const FGeoBuilding& Building = Buildings[Index];
                        if (Building.Nodes.Num() < 3)
                                continue;

                        double Low = TNumericLimits<double>::Max();
                        double High = -Low;
                        for (int32 Node = 0; Node < Building.Nodes.Num(); ++Node)
                        {
                                const FVector2D& Start = Building.Nodes[Node];
                                const FVector2D& End = Building.Nodes[(Node + 1) % Building.Nodes.Num()];
                                const int32 Steps = FMath::Max(1, FMath::CeilToInt(FVector2D::Distance(Start, End) / 5.0));
                                for (int32 Step = 0; Step < Steps; ++Step)
                                {
                                        const FVector2D P = FMath::Lerp(Start, End, static_cast<double>(Step) / Steps);
                                        const double Z = MeshGen.SampleGroundZ(FVector2D(P.X * ScaleFactor, -P.Y * ScaleFactor));
                                        Low = FMath::Min(Low, Z);
                                        High = FMath::Max(High, Z);
                                }
                        }
                        BuildingZOffsets[Index] = High;
                        BuildingFoundationDepths[Index] = High - Low;
                }
        }

        // ZDiag: raw per-building offsets, before they're applied to geometry.
        if (BuildingZOffsets.Num() > 0)
        {
                float MinO = BuildingZOffsets[0], MaxO = BuildingZOffsets[0], SumO = 0.f;
                for (float O : BuildingZOffsets) { MinO = FMath::Min(MinO, O); MaxO = FMath::Max(MaxO, O); SumO += O; }
                UE_LOG(LogTemp, Log, TEXT("HiroGeo[ZDiag] BuildingZOffsets: [%.1f .. %.1f] UE avg=%.1f UE (%.2fm .. %.2fm avg=%.2fm) over %d buildings"),
                        MinO, MaxO, SumO / BuildingZOffsets.Num(),
                        MinO / ScaleFactor, MaxO / ScaleFactor, (SumO / BuildingZOffsets.Num()) / ScaleFactor,
                        BuildingZOffsets.Num());
        }

        // Buildings (walls/windows/glass) — same 9 sections as Generate().
        FGeoMeshGenerator::FMeshData WallData, GlassData, BrickData, DarkData, LightData, ColorWallData, WindowData;
        FGeoMeshGenerator::FMeshData WindowFrameData, WindowGlassData;
        MeshGen.GenerateBuildingMeshes(Buildings, WallData, GlassData, BrickData, DarkData, LightData, ColorWallData,
                WindowData, WindowFrameData, WindowGlassData, &BuildingZOffsets, &BuildingFoundationDepths);
        LogZRange(TEXT("WallData"), WallData);
        // Same collision flags as Generate() — solid walls/facades on, thin panes off.
        CreateSection(BuildingMeshComp, 0, WallData,        true);
        CreateSection(BuildingMeshComp, 1, GlassData,       true);
        CreateSection(BuildingMeshComp, 2, BrickData,       true);
        CreateSection(BuildingMeshComp, 3, DarkData,        true);
        CreateSection(BuildingMeshComp, 4, LightData,       true);
        CreateSection(BuildingMeshComp, 5, ColorWallData,   true);
        CreateSection(BuildingMeshComp, 6, WindowData,      true);
        CreateSection(BuildingMeshComp, 7, WindowFrameData, false);
        CreateSection(BuildingMeshComp, 8, WindowGlassData, false);
        if (BuildingMeshComp)
        {
                BuildingMeshComp->SetCollisionProfileName(TEXT("BlockAll"));
        }

        // Roofs — same BuildingZOffsets array/order keeps them flush on the walls.
        FGeoMeshGenerator::FMeshData RoofData, RoofColorData;
        MeshGen.GenerateRoofMeshes(Buildings, RoofData, RoofColorData, &BuildingZOffsets);
        CreateSection(RoofMeshComp, 0, RoofData,      true);
        CreateSection(RoofMeshComp, 1, RoofColorData, true);
        if (RoofMeshComp) RoofMeshComp->SetCollisionProfileName(TEXT("BlockAll"));

        // Roads + sidewalks — sampled per polyline vertex so they follow slopes.
        FGeoMeshGenerator::FMeshData RoadData, SWData;
        MeshGen.GenerateRoadMeshes(Roads, RoadData, SWData, Dem, SampleOriginLat, SampleOriginLon);
        LogZRange(TEXT("RoadData"), RoadData);
        LogZRange(TEXT("SWData"), SWData);
        CreateSection(RoadMeshComp,     0, RoadData, true);
        CreateSection(SidewalkMeshComp, 0, SWData,   true);
        if (RoadMeshComp)     RoadMeshComp->SetCollisionProfileName(TEXT("BlockAll"));
        if (SidewalkMeshComp) SidewalkMeshComp->SetCollisionProfileName(TEXT("BlockAll"));

        // Water — one flat offset per body, sampled at its footprint centroid.
        FGeoMeshGenerator::FMeshData WaterData;
        MeshGen.GenerateWaterMeshes(WaterBodies, WaterData, Dem, SampleOriginLat, SampleOriginLon);
        LogZRange(TEXT("WaterData"), WaterData);
        CreateSection(WaterMeshComp, 0, WaterData, false);

        // Railways — ballast bed + rails, one offset per polyline vertex so the
        // track follows rising/falling terrain just like roads (previously this
        // step was never re-run here, so railways stayed perfectly flat at their
        // initial Z forever, floating over hills/sinking into dips on anything
        // but flat ground).
        if (bShowRailways && RailwayMeshComp)
        {
                FGeoMeshGenerator::FMeshData BallastData, RailStripData;
                MeshGen.GenerateRailwayMeshes(Railways, BallastData, RailStripData,
                        Dem, SampleOriginLat, SampleOriginLon);
                CreateSection(RailwayMeshComp, 0, BallastData, false);
                CreateSection(RailwayMeshComp, 1, RailStripData, false);
        }

        // Mutable copy so powerline socket pass can orient poles (RotationDeg).
        TArray<FGeoModelInstance> MutableModels = Models;

        if (FenceMeshComp)
        {
                FGeoMeshGenerator::FMeshData FenceData, WireData, PitchData;
                MeshGen.GenerateFenceMeshes(Fences, FenceData, Dem, SampleOriginLat, SampleOriginLon);
                MeshGen.GeneratePowerLineMeshes(PowerLines, WireData, Dem, SampleOriginLat, SampleOriginLon, nullptr, &MutableModels);
                MeshGen.GeneratePitchMarkings(Vegetations, PitchData, Dem, SampleOriginLat, SampleOriginLon);
                CreateSection(FenceMeshComp, 0, FenceData, true);
                CreateSection(FenceMeshComp, 1, WireData, false);
                CreateSection(FenceMeshComp, 2, PitchData, false);
                FenceMeshComp->SetCollisionProfileName(TEXT("BlockAll"));
        }

        // Vegetation ground-cover areas — one flat offset per patch.
        FGeoMeshGenerator::FMeshData VegData;
        MeshGen.GenerateVegetationMeshes(Vegetations, VegData, Dem, SampleOriginLat, SampleOriginLon);
        CreateSection(VegetationMeshComp, 0, VegData, false);

        // Scattered trees/grass + individually-tagged OSM trees — one offset per
        // instance, so vegetation follows the same terrain as everything else
        // instead of staying frozen on the flat plane it was scattered on before
        // the online DEM arrived.
        TArray<FTransform> TreeXf, GrassXf;
        MeshGen.GenerateVegetationTransforms(Vegetations, Buildings, Roads,
                TreeXf, GrassXf, TreeDensityScale, GrassDensityScale,
                Dem, SampleOriginLat, SampleOriginLon);
        FGeoMeshGenerator::AppendOSMTreeTransforms(Trees, TreeXf, ScaleFactor, GroundOffset,
                Dem, SampleOriginLat, SampleOriginLon);
        SpawnVegetation(TreeXf, GrassXf);

        // Re-place street furniture / landmarks on the DEM so they don't float
        // at Z=0 after terrain rebuild (previously only trees were re-elevated).
        SpawnModels(MutableModels, Dem, SampleOriginLat, SampleOriginLon);

        // CreateMeshSection replaces geometry and clears per-section materials —
        // re-bind them or the rebuilt city renders unlit/default grey.
        ApplyMaterials();

        auto UpdateB = [](UProceduralMeshComponent* C) { if (C) C->UpdateBounds(); };
        UpdateB(BuildingMeshComp); UpdateB(RoofMeshComp); UpdateB(RoadMeshComp);
        UpdateB(SidewalkMeshComp); UpdateB(WaterMeshComp); UpdateB(VegetationMeshComp);
        UpdateB(GroundMeshComp); UpdateB(RailwayMeshComp); UpdateB(FenceMeshComp);

        UE_LOG(LogTemp, Log, TEXT("HiroGeo: meshes rebuilt with online elevation data (%dx%d px @ zoom %d)."),
                Heightmap->Width, Heightmap->Height, Heightmap->Zoom);
}

// ============================================================================
// Clear
// ============================================================================
void AHiroGeoActor::Clear()
{
        if (BuildingMeshComp)   BuildingMeshComp->ClearAllMeshSections();
        if (RoadMeshComp)       RoadMeshComp->ClearAllMeshSections();
        if (SidewalkMeshComp)   SidewalkMeshComp->ClearAllMeshSections();
        if (WaterMeshComp)      WaterMeshComp->ClearAllMeshSections();
        if (GroundMeshComp)     GroundMeshComp->ClearAllMeshSections();
        if (RoofMeshComp)       RoofMeshComp->ClearAllMeshSections();
        if (VegetationMeshComp) VegetationMeshComp->ClearAllMeshSections();
        if (RailwayMeshComp)    RailwayMeshComp->ClearAllMeshSections();
        if (FenceMeshComp)      FenceMeshComp->ClearAllMeshSections();
        if (POIMeshComp)        POIMeshComp->ClearAllMeshSections();
        RemoveVegetation();
        RemoveModels();
}
