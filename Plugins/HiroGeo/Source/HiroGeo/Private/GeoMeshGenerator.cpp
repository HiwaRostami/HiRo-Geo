// Copyright Hiro. All Rights Reserved.

#include "GeoMeshGenerator.h"
#include "ProceduralMeshComponent.h"
#include "Algo/Reverse.h"
#include "HiroGeoElevationFetcher.h"

// ============================================================================
// Local helper: Convert FLinearColor to FColor
// ============================================================================
static FColor LinearToFColor(const FLinearColor& LC)
{
        return FColor(
                (uint8)FMath::Clamp(FMath::RoundToInt(LC.R * 255.f), 0, 255),
                (uint8)FMath::Clamp(FMath::RoundToInt(LC.G * 255.f), 0, 255),
                (uint8)FMath::Clamp(FMath::RoundToInt(LC.B * 255.f), 0, 255),
                255);
}

// ============================================================================
// Local helper: RecomputeNormals from triangle faces
// ============================================================================
static void RecomputeNormals(FGeoMeshGenerator::FMeshData& MeshData)
{
        MeshData.Normals.Init(FVector::ZeroVector, MeshData.Vertices.Num());

        for (int32 ti = 0; ti + 2 < MeshData.Triangles.Num(); ti += 3)
        {
                int32 I0 = MeshData.Triangles[ti];
                int32 I1 = MeshData.Triangles[ti + 1];
                int32 I2 = MeshData.Triangles[ti + 2];

                if (I0 >= MeshData.Vertices.Num() ||
                        I1 >= MeshData.Vertices.Num() ||
                        I2 >= MeshData.Vertices.Num())
                        continue;

                const FVector& V0 = MeshData.Vertices[I0];
                const FVector& V1 = MeshData.Vertices[I1];
                const FVector& V2 = MeshData.Vertices[I2];

                FVector FaceN = FVector::CrossProduct(V1 - V0, V2 - V0);
                if (!FaceN.IsNearlyZero())
                {
                        FaceN.Normalize();
                        FaceN = -FaceN; // Reverse normal direction for roofs
                        MeshData.Normals[I0] += FaceN;
                        MeshData.Normals[I1] += FaceN;
                        MeshData.Normals[I2] += FaceN;
                }
        }

        for (int32 i = 0; i < MeshData.Normals.Num(); i++)
                if (!MeshData.Normals[i].IsNearlyZero())
                        MeshData.Normals[i].Normalize();
}

// ============================================================================
// Local helper: RecomputeTangents
// ============================================================================
static void RecomputeTangents(FGeoMeshGenerator::FMeshData& MeshData)
{
        MeshData.Tangents.SetNum(MeshData.Vertices.Num());
        for (int32 i = 0; i < MeshData.Vertices.Num(); i++)
        {
                const FVector& N = MeshData.Normals[i];
                FVector Tangent(1, 0, 0);
                if (FMath::Abs(FVector::DotProduct(Tangent, N)) > 0.9f)
                        Tangent = FVector(0, 1, 0);
                Tangent -= FVector::DotProduct(Tangent, N) * N;
                if (!Tangent.IsNearlyZero()) Tangent.Normalize();
                else Tangent = FVector(1, 0, 0);
                MeshData.Tangents[i] = FProcMeshTangent(Tangent, false);
        }
}

// ============================================================================
// Local helper: FlipNormals (used for roads/ground that need downward-facing)
// ============================================================================
static void FlipNormals(FGeoMeshGenerator::FMeshData& MeshData)
{
        for (FVector& N : MeshData.Normals)
                N = -N;
}

// ============================================================================
// Local helper: convert local-meter coordinates (relative to OriginLatitude/
// OriginLongitude) to a WGS84 lat/lon — the inverse of FOSMLoader::LatLonToLocal.
// Mirrors the exact projection GenerateGroundMesh uses so DEM samples line up
// with the rest of the OSM-derived geometry.
// ============================================================================
static void LocalMetersToLatLon(
        double OriginLat, double OriginLon, double LocalXM, double LocalYM,
        double& OutLat, double& OutLon)
{
        const double EarthRadiusM = 6371000.0;
        const double CosOriginLat = FMath::Cos(OriginLat * UE_PI / 180.0);
        OutLat = OriginLat + (LocalYM / EarthRadiusM) * 180.0 / UE_PI;
        OutLon = OriginLon + (LocalXM / (EarthRadiusM * FMath::Max(CosOriginLat, 0.0001))) * 180.0 / UE_PI;
}

// ============================================================================
// Local helper: sample the DEM under a local-meter point and return the
// elevation as a UE-unit Z offset, relative to RefElev (the DEM elevation at
// the data's local origin) — i.e. 0 at the origin, exactly matching how
// GenerateGroundMesh's own RefElev/SampleElevUE establish "ground zero" for
// the OSM-derived geometry. Returns 0.f whenever DemHeightmap is null/invalid.
// ============================================================================
static float SampleDemOffsetUE(
        const FHiroGeoHeightmap* DemHeightmap, double OriginLat, double OriginLon,
        double RefElev, float ScaleFactor, float LocalXM, float LocalYM)
{
        if (!DemHeightmap || !DemHeightmap->IsValid())
                return 0.f;

        double Lat, Lon;
        LocalMetersToLatLon(OriginLat, OriginLon, (double)LocalXM, (double)LocalYM, Lat, Lon);
        const float ElevM = DemHeightmap->SampleAtLatLon(Lat, Lon) - (float)RefElev;
        return ElevM * ScaleFactor;
}

// Minimum DEM offset over a polyline/polygon in local meters — streets-gl uses
// terrainMinHeight at building footprint vertices so walls sit on the lowest
// ground under the outline (slopes don't float on the centroid).
static float SampleDemMinOffsetUE(
        const FHiroGeoHeightmap* DemHeightmap, double OriginLat, double OriginLon,
        double RefElev, float ScaleFactor, const TArray<FVector2D>& Points)
{
        if (!DemHeightmap || !DemHeightmap->IsValid() || Points.Num() == 0)
                return 0.f;

        float MinOff = FLT_MAX;
        for (const FVector2D& P : Points)
                MinOff = FMath::Min(MinOff, SampleDemOffsetUE(DemHeightmap, OriginLat, OriginLon,
                        RefElev, ScaleFactor, P.X, P.Y));
        return MinOff == FLT_MAX ? 0.f : MinOff;
}

// ============================================================================
// Local helper: minimum elevation among sparse OSM "ele"/GeoJSON-Z samples,
// used as RefElev so the IDW-derived terrain sits at Z≈0 locally — exactly how
// GenerateGroundMesh's own non-DEM branch establishes "ground zero" for the
// OSM-derived geometry. Returns 0.0 when Samples is empty.
// ============================================================================
static double ComputeElevationSamplesRefElev(const TArray<FElevationSample>& Samples)
{
        if (Samples.Num() == 0)
                return 0.0;

        double RefElev = Samples[0].Elevation;
        for (const FElevationSample& S : Samples)
                RefElev = FMath::Min(RefElev, (double)S.Elevation);
        return RefElev;
}

// ============================================================================
// Local helper: IDW-interpolate the sparse OSM "ele"/GeoJSON-Z elevation
// samples at a local-meter point and return the UE-unit Z offset relative to
// RefElev. This is the exact inverse-distance-weighted formula
// GenerateGroundMesh's own SampleElevUE fallback uses (Power=2,
// MinDistM=0.5), factored out here so buildings/roofs/roads/sidewalks/water/
// vegetation can sit on the same sparse-sample terrain as the ground mesh
// instead of floating on a flat Z=0 plane whenever no DEM heightmap is
// available (e.g. online elevation fetch disabled, or the GeoJSON import
// path, which never has OSM lat/lon bounds to fetch a DEM for in the first
// place). Returns 0.f whenever Samples is empty.
// ============================================================================
static float SampleElevationSamplesOffsetUE(
        const TArray<FElevationSample>& Samples, double RefElev,
        float ScaleFactor, float LocalXM, float LocalYM)
{
        if (Samples.Num() == 0)
                return 0.f;

        float WeightSum = 0.f, ElevSum = 0.f;
        const float Power = 2.f;
        const float MinDistM = 0.5f;
        for (const FElevationSample& S : Samples)
        {
                const float DX = LocalXM - S.Location.X;
                const float DY = LocalYM - S.Location.Y;
                const float Dist = FMath::Sqrt(DX * DX + DY * DY);
                if (Dist < MinDistM)
                        return (float)((S.Elevation - RefElev) * (double)ScaleFactor);
                const float W = 1.f / FMath::Pow(Dist, Power);
                ElevSum += W * (float)(S.Elevation - RefElev);
                WeightSum += W;
        }
        return WeightSum > 0.f ? (ElevSum / WeightSum) * ScaleFactor : 0.f;
}

static float SampleElevationSamplesMinOffsetUE(
        const TArray<FElevationSample>& Samples, double RefElev,
        float ScaleFactor, const TArray<FVector2D>& Points)
{
        if (Samples.Num() == 0 || Points.Num() == 0)
                return 0.f;

        float MinOff = FLT_MAX;
        for (const FVector2D& P : Points)
                MinOff = FMath::Min(MinOff, SampleElevationSamplesOffsetUE(Samples, RefElev,
                        ScaleFactor, P.X, P.Y));
        return MinOff == FLT_MAX ? 0.f : MinOff;
}

// ============================================================================
// Local helper: insert extra colinear points along every edge of Pts so no
// segment is longer than MaxSegM (in local meters). Every other generator
// (roads, railways, water, vegetation) samples terrain height only at its
// existing OSM vertices, which on a straight stretch can be tens or hundreds
// of meters apart. Linearly extruding/flattening between two such far-apart
// samples draws a straight CHORD between them, which on anything but flat
// ground cuts below (or pokes above) the ground mesh's own much finer DEM
// grid in between — the geometry appears to sink into / float above uneven
// terrain even though every original vertex itself is correctly placed.
// Densifying the polyline first means the later per-vertex DEM sampling step
// (already present in every caller) naturally follows the terrain's curve
// instead of skipping over it. bClosed=true treats the last point as also
// connecting back to the first (polygons); bClosed=false leaves the two
// endpoints alone (open polylines like roads/railways).
// ============================================================================
static TArray<FVector2D> ResamplePolylineForTerrain(const TArray<FVector2D>& Pts, float MaxSegM, bool bClosed)
{
        const int32 N = Pts.Num();
        if (N < 2) return Pts;

        TArray<FVector2D> Out;
        Out.Reserve(N * 2);
        const int32 EdgeCount = bClosed ? N : (N - 1);
        for (int32 i = 0; i < EdgeCount; i++)
        {
                const FVector2D& A = Pts[i];
                const FVector2D& B = Pts[(i + 1) % N];
                Out.Add(A);

                const float Dist = FVector2D::Distance(A, B);
                const int32 NumExtra = (MaxSegM > 0.01f) ? FMath::FloorToInt(Dist / MaxSegM) : 0;
                for (int32 s = 1; s <= NumExtra; s++)
                {
                        const float T = (float)s / (float)(NumExtra + 1);
                        Out.Add(FMath::Lerp(A, B, T));
                }
        }
        if (!bClosed) Out.Add(Pts.Last());
        return Out;
}

// ============================================================================
// Local helper: add DZ to every vertex's Z from StartIdx to the end of the
// array. Used to shift exactly the geometry one feature (one building, one
// water body, ...) just added to a shared MeshData buffer, the same way
// FillSliceUV1 already stamps the atlas slice for that same vertex range.
// ============================================================================
static void ShiftZRange(FGeoMeshGenerator::FMeshData& MeshData, int32 StartIdx, float DZ)
{
        if (FMath::IsNearlyZero(DZ))
                return;
        for (int32 VI = StartIdx; VI < MeshData.Vertices.Num(); VI++)
                MeshData.Vertices[VI].Z += DZ;
}

// ============================================================================
// ============================================================================
// PRIMITIVE HELPERS
// ============================================================================
// ============================================================================

// ============================================================================
// AddQuad
// ============================================================================
void FGeoMeshGenerator::AddQuad(
        FMeshData& MeshData,
        const FVector& V0, const FVector& V1,
        const FVector& V2, const FVector& V3,
        const FColor& Color)
{
        int32 BI = MeshData.Vertices.Num();
        MeshData.Vertices.Add(V0); MeshData.Vertices.Add(V1);
        MeshData.Vertices.Add(V2); MeshData.Vertices.Add(V3);

        MeshData.Triangles.Add(BI + 0); MeshData.Triangles.Add(BI + 1); MeshData.Triangles.Add(BI + 2);
        MeshData.Triangles.Add(BI + 0); MeshData.Triangles.Add(BI + 2); MeshData.Triangles.Add(BI + 3);

        FVector E1 = V1 - V0, E2 = V3 - V0;
        FVector Normal = FVector::CrossProduct(E1, E2);
        if (Normal.IsNearlyZero()) Normal = FVector::CrossProduct(V2 - V0, V3 - V1);
        if (Normal.IsNearlyZero()) Normal = FVector(0, 0, 1);
        Normal.Normalize();

        FVector Tangent = E1;
        Tangent -= FVector::DotProduct(Tangent, Normal) * Normal;
        if (!Tangent.IsNearlyZero()) Tangent.Normalize(); else Tangent = FVector(1, 0, 0);

        float UScale = 100.0f, VScale = 100.0f;
        for (int32 i = 0; i < 4; i++)
        {
                MeshData.Normals.Add(Normal);
                MeshData.VertexColors.Add(Color);
                MeshData.Tangents.Add(FProcMeshTangent(Tangent, false));
        }
        MeshData.UV0.Add(FVector2D(0, V3.Z / VScale));
        MeshData.UV0.Add(FVector2D(FVector2D::Distance(FVector2D(V0.X, V0.Y), FVector2D(V1.X, V1.Y)) / UScale, V3.Z / VScale));
        MeshData.UV0.Add(FVector2D(FVector2D::Distance(FVector2D(V0.X, V0.Y), FVector2D(V1.X, V1.Y)) / UScale, V2.Z / VScale));
        MeshData.UV0.Add(FVector2D(0, V2.Z / VScale));
}

// ============================================================================
// AddTriangle
// ============================================================================
void FGeoMeshGenerator::AddTriangle(
        FMeshData& MeshData,
        const FVector& V0, const FVector& V1, const FVector& V2,
        const FColor& Color)
{
        int32 BI = MeshData.Vertices.Num();
        MeshData.Vertices.Add(V0); MeshData.Vertices.Add(V1); MeshData.Vertices.Add(V2);
        MeshData.Triangles.Add(BI); MeshData.Triangles.Add(BI + 1); MeshData.Triangles.Add(BI + 2);

        FVector Normal = FVector::CrossProduct(V1 - V0, V2 - V0);
        if (Normal.IsNearlyZero()) Normal = FVector(0, 0, 1);
        Normal.Normalize();

        FVector Tangent = V1 - V0;
        Tangent -= FVector::DotProduct(Tangent, Normal) * Normal;
        if (!Tangent.IsNearlyZero()) Tangent.Normalize(); else Tangent = FVector(1, 0, 0);

        for (int32 i = 0; i < 3; i++)
        {
                MeshData.Normals.Add(Normal);
                MeshData.VertexColors.Add(Color);
                MeshData.Tangents.Add(FProcMeshTangent(Tangent, false));
        }
        MeshData.UV0.Add(FVector2D(0, 0)); MeshData.UV0.Add(FVector2D(1, 0)); MeshData.UV0.Add(FVector2D(0.5f, 1));
}

// ============================================================================
// AddDoubleSidedQuad
// ============================================================================
void FGeoMeshGenerator::AddDoubleSidedQuad(
        FMeshData& MeshData, const FVector& V0, const FVector& V1,
        const FVector& V2, const FVector& V3, const FColor& Color)
{
        AddQuad(MeshData, V0, V1, V2, V3, Color);
        AddQuad(MeshData, V3, V2, V1, V0, Color);
}

// ============================================================================
// AddPolygonTriangulated
// ============================================================================
void FGeoMeshGenerator::AddPolygonTriangulated(
        FMeshData& MeshData, const TArray<FVector>& Points,
        float Height, const FColor& Color, bool bUseVertexZ)
{
        TArray<FVector> WorkingPoints;
        for (const FVector& P : Points)
        {
                FVector TargetPos = FVector(P.X, P.Y, bUseVertexZ ? P.Z : Height);
                if (WorkingPoints.Num() > 0 && TargetPos.Equals(WorkingPoints[0], 1.0f)) continue;
                if (WorkingPoints.Num() == 0 || !TargetPos.Equals(WorkingPoints.Last(), 1.0f))
                {
                        WorkingPoints.Add(TargetPos);
                }
        }

        int32 N = WorkingPoints.Num();
        if (N < 3) return;

        // Compute signed area BEFORE adding vertices so we can normalize winding.
        // Positive area = CCW (face up), negative = CW (face down).
        float PolyArea = 0.f;
        for (int32 i = 0; i < N; i++)
        {
                int32 j = (i + 1) % N;
                PolyArea += WorkingPoints[i].X * WorkingPoints[j].Y - WorkingPoints[j].X * WorkingPoints[i].Y;
        }
        // Normalize to CCW so emitted triangles always face +Z and the hardcoded
        // (0,0,1) normals are correct. OSM polygons have Y negated on load, which
        // flips CW<->CCW; without this, vegetation and water normals point downward.
        // IMPORTANT: must do this BEFORE adding to MeshData so vertex order matches.
        if (PolyArea < 0.f)
        {
                Algo::Reverse(WorkingPoints);
                PolyArea = -PolyArea;
        }
        const float EarCrossSign = 1.f; // Always CCW after normalization above

        int32 BI = MeshData.Vertices.Num();
        FVector GlobalTangent = FVector(1, 0, 0);
        if (N > 1)
        {
                GlobalTangent = (WorkingPoints[1] - WorkingPoints[0]);
                GlobalTangent.Z = 0.f;
                GlobalTangent.Normalize();
        }

        for (const FVector& P : WorkingPoints)
        {
                MeshData.Vertices.Add(P);
                MeshData.Normals.Add(FVector(0.f, 0.f, 1.f));
                MeshData.VertexColors.Add(Color);
                MeshData.Tangents.Add(FProcMeshTangent(GlobalTangent, false));
                MeshData.UV0.Add(FVector2D(P.X / 100.f, P.Y / 100.f));
        }

        TArray<int32> Idx;
        for (int32 i = 0; i < N; i++) Idx.Add(i);

        int32 Fail = 0;
        while (Idx.Num() > 3)
        {
                bool bEar = false;
                int32 NI = Idx.Num();
                for (int32 i = 0; i < NI; i++)
                {
                        int32 ip = (i == 0) ? NI - 1 : i - 1;
                        int32 in = (i + 1) % NI;
                        int32 Ip = Idx[ip], Ic = Idx[i], In = Idx[in];

                        FVector2D vp(WorkingPoints[Ip].X, WorkingPoints[Ip].Y);
                        FVector2D vc(WorkingPoints[Ic].X, WorkingPoints[Ic].Y);
                        FVector2D vn(WorkingPoints[In].X, WorkingPoints[In].Y);

                        float Cross = (vc.X - vp.X) * (vn.Y - vp.Y) - (vc.Y - vp.Y) * (vn.X - vp.X);
                        if (FMath::Abs(Cross) <= 0.01f) continue;
                        // Only accept ears that are convex corners (matching polygon winding)
                        if (Cross * EarCrossSign < 0.f) continue;

                        bool bIsEar = true;
                        for (int32 j = 0; j < NI; j++)
                        {
                                if (j == ip || j == i || j == in) continue;
                                FVector2D tp(WorkingPoints[Idx[j]].X, WorkingPoints[Idx[j]].Y);
                                if (IsPointInTriangle2D(tp, vp, vc, vn)) { bIsEar = false; break; }
                        }
                        if (bIsEar)
                        {
                                MeshData.Triangles.Add(BI + Ip);
                                MeshData.Triangles.Add(BI + Ic);
                                MeshData.Triangles.Add(BI + In);

                                Idx.RemoveAt(i);
                                bEar = true;
                                Fail = 0;
                                break;
                        }
                }
                if (!bEar)
                {
                        Fail++;
                        if (Fail > NI * 3)
                        {
                                // Emergency fan fallback: only emit triangles whose centroid
                                // lies inside the original polygon to avoid spurious fills
                                // on U/L-shaped concave footprints.
                                for (int32 i = 1; i < Idx.Num() - 1; i++)
                                {
                                        FVector2D CA = FVector2D(WorkingPoints[Idx[0]].X, WorkingPoints[Idx[0]].Y);
                                        FVector2D CB = FVector2D(WorkingPoints[Idx[i]].X, WorkingPoints[Idx[i]].Y);
                                        FVector2D CC = FVector2D(WorkingPoints[Idx[i + 1]].X, WorkingPoints[Idx[i + 1]].Y);
                                        FVector2D TriCen = (CA + CB + CC) / 3.f;

                                        // Point-in-polygon test for the triangle centroid
                                        bool bInside = false;
                                        TArray<FVector2D> PolyPts;
                                        for (const FVector& V : WorkingPoints)
                                                PolyPts.Add(FVector2D(V.X, V.Y));
                                        int32 PN = PolyPts.Num();
                                        for (int32 pi = 0, pj = PN - 1; pi < PN; pj = pi++)
                                        {
                                                if (((PolyPts[pi].Y > TriCen.Y) != (PolyPts[pj].Y > TriCen.Y)) &&
                                                        (TriCen.X < (PolyPts[pj].X - PolyPts[pi].X) * (TriCen.Y - PolyPts[pi].Y) / (PolyPts[pj].Y - PolyPts[pi].Y) + PolyPts[pi].X))
                                                        bInside = !bInside;
                                        }
                                        if (bInside)
                                        {
                                                MeshData.Triangles.Add(BI + Idx[0]);
                                                MeshData.Triangles.Add(BI + Idx[i]);
                                                MeshData.Triangles.Add(BI + Idx[i + 1]);
                                        }
                                }
                                return;
                        }
                }
        }
        if (Idx.Num() == 3)
        {
                // Final triangle: verify its centroid is inside the polygon
                FVector2D CA = FVector2D(WorkingPoints[Idx[0]].X, WorkingPoints[Idx[0]].Y);
                FVector2D CB = FVector2D(WorkingPoints[Idx[1]].X, WorkingPoints[Idx[1]].Y);
                FVector2D CC = FVector2D(WorkingPoints[Idx[2]].X, WorkingPoints[Idx[2]].Y);
                FVector2D TriCen = (CA + CB + CC) / 3.f;
                bool bInside = false;
                int32 PN = N; // use original vertex count for polygon test
                for (int32 pi = 0, pj = PN - 1; pi < PN; pj = pi++)
                {
                        FVector2D PA(WorkingPoints[pi].X, WorkingPoints[pi].Y);
                        FVector2D PB(WorkingPoints[pj].X, WorkingPoints[pj].Y);
                        if (((PA.Y > TriCen.Y) != (PB.Y > TriCen.Y)) &&
                                (TriCen.X < (PB.X - PA.X) * (TriCen.Y - PA.Y) / (PB.Y - PA.Y) + PA.X))
                                bInside = !bInside;
                }
                if (bInside)
                {
                        MeshData.Triangles.Add(BI + Idx[0]);
                        MeshData.Triangles.Add(BI + Idx[1]);
                        MeshData.Triangles.Add(BI + Idx[2]);
                }
        }
}

bool FGeoMeshGenerator::AddPolygonWithHolesTriangulated(
        FMeshData& MeshData, const TArray<FVector2D>& Outer,
        const TArray<TArray<FVector2D>>& Holes, float Height, const FColor& Color)
{
        if (Outer.Num() < 3 || Holes.Num() == 0) return false;

        TArray<int32> Tris;
        TArray<FVector2D> Steiner;
        FCGALSkeletonGenerator::TriangulateWithCGAL(Outer, Holes, Tris, Steiner);
        if (Tris.Num() < 3) return false;

        TArray<FVector2D> All = Outer;
        All.Append(Steiner);
        const int32 Base = MeshData.Vertices.Num();
        for (const FVector2D& P : All)
        {
                MeshData.Vertices.Add(FVector(P.X, P.Y, Height));
                MeshData.Normals.Add(FVector::UpVector);
                MeshData.VertexColors.Add(Color);
                MeshData.Tangents.Add(FProcMeshTangent(FVector::ForwardVector, false));
                MeshData.UV0.Add(FVector2D(P.X / 100.f, P.Y / 100.f));
        }
        for (int32 Index : Tris)
                MeshData.Triangles.Add(Base + Index);
        return true;
}

// ============================================================================
// AddExtrudedPolygon
// ============================================================================
void FGeoMeshGenerator::AddExtrudedPolygon(
        FMeshData& MeshData, const TArray<FVector>& Points,
        float BaseH, float TopH, const FColor& WallColor, bool bBottom, const FColor& BotColor)
{
        int32 N = Points.Num(); if (N < 3) return;

        TArray<FVector> WorkPoints = Points;
        if (ComputeSignedArea2D(WorkPoints) < 0.f)
                Algo::Reverse(WorkPoints);

        for (int32 i = 0; i < N; i++)
        {
                int32 Next = (i + 1) % N;
                FVector B0(WorkPoints[i].X, WorkPoints[i].Y, BaseH), B1(WorkPoints[Next].X, WorkPoints[Next].Y, BaseH);
                FVector T1(WorkPoints[Next].X, WorkPoints[Next].Y, TopH), T0(WorkPoints[i].X, WorkPoints[i].Y, TopH);
                FVector EdgeDir = B1 - B0;
                FVector OutN = FVector(EdgeDir.Y, -EdgeDir.X, 0);

                if (!OutN.IsNearlyZero()) OutN.Normalize(); else OutN = FVector(0, 0, 1);
                FVector Tang = EdgeDir; if (!Tang.IsNearlyZero()) Tang.Normalize(); else Tang = FVector(1, 0, 0);
                int32 BI = MeshData.Vertices.Num();

                MeshData.Vertices.Add(B0); MeshData.Vertices.Add(B1); MeshData.Vertices.Add(T1); MeshData.Vertices.Add(T0);
                MeshData.Triangles.Add(BI + 0); MeshData.Triangles.Add(BI + 2); MeshData.Triangles.Add(BI + 1);
                MeshData.Triangles.Add(BI + 0); MeshData.Triangles.Add(BI + 3); MeshData.Triangles.Add(BI + 2);

                float WW = FVector2D::Distance(FVector2D(B0.X, B0.Y), FVector2D(B1.X, B1.Y)) / 100.f;
                MeshData.UV0.Add(FVector2D(0, BaseH / 100.f)); MeshData.UV0.Add(FVector2D(WW, BaseH / 100.f));
                MeshData.UV0.Add(FVector2D(WW, TopH / 100.f)); MeshData.UV0.Add(FVector2D(0, TopH / 100.f));
                for (int32 v = 0; v < 4; v++) { MeshData.Normals.Add(OutN); MeshData.VertexColors.Add(WallColor); MeshData.Tangents.Add(FProcMeshTangent(Tang, false)); }
        }
        if (bBottom) AddPolygonTriangulated(MeshData, WorkPoints, BaseH, BotColor);
}

// ============================================================================
// AddSkillionExtrudedPolygon
// Like AddExtrudedPolygon but the TOP of each wall vertex follows the skillion
// roof slope, so the wall hugs the underside of the roof exactly.
// RoofDirection: OSM bearing (0=N,90=E,...) of the LOW eave — same convention
//                as AddSkillionRoof.  Pass -1 to fall back to flat top.
// WallTopFlat:   the uniform wall-top height used when RoofDirection < 0.
// RoofH:         vertical rise from low eave to high eave.
// ============================================================================
void FGeoMeshGenerator::AddSkillionExtrudedPolygon(
        FMeshData& MeshData, const TArray<FVector>& Points,
        float BaseH, float WallTopFlat,
        float RoofH, float RoofDirection,
        const FColor& WallColor)
{
        int32 N = Points.Num();
        if (N < 3) return;

        TArray<FVector> WorkPoints = Points;
        if (ComputeSignedArea2D(WorkPoints) < 0.f)
                Algo::Reverse(WorkPoints);

        // Pre-compute per-vertex top Z using the same slope logic as AddSkillionRoof.
        // Footprint Y is already negated (local space), so North = -Y: negate Cos.
        TArray<float> TopZ;
        TopZ.SetNum(N);

        if (RoofDirection >= 0.f)
        {
                // Match streets-gl SkillionRoofBuilder.getRotation (same as the roof):
                //   rotation = -toRad(direction) - PI/2, slope-up = (-cos D, -sin D).
                // OSM roof:direction is the downslope azimuth (clockwise from north).
                // In local space X=east, Y=-north, so the upslope (toward the high eave)
                // is (-sin D, cos D). MaxProj = high-eave (tall wall), MinProj = low-eave.
                float D_rad = FMath::DegreesToRadians(RoofDirection);
                FVector2D SlopeDir(-FMath::Sin(D_rad), FMath::Cos(D_rad));

                float MinProj = FLT_MAX, MaxProj = -FLT_MAX;
                for (const FVector& V : WorkPoints)
                {
                        float Proj = V.X * SlopeDir.X + V.Y * SlopeDir.Y;
                        MinProj = FMath::Min(MinProj, Proj);
                        MaxProj = FMath::Max(MaxProj, Proj);
                }
                float Range = FMath::Max(MaxProj - MinProj, 1.f);

                for (int32 i = 0; i < N; i++)
                {
                        float Proj = WorkPoints[i].X * SlopeDir.X + WorkPoints[i].Y * SlopeDir.Y;
                        // Use IDENTICAL T mapping as AddSkillionRoof:
                        // T=0 -> MinProj (low eave, Z=WallTopFlat+0)
                        // T=1 -> MaxProj (high eave, Z=WallTopFlat+RoofH)
                        // This makes wall top == roof bottom at every vertex.
                        float T = (Proj - MinProj) / Range;
                        TopZ[i] = WallTopFlat + FMath::Lerp(0.f, RoofH, T);
                }
        }
        else
        {
                for (int32 i = 0; i < N; i++)
                        TopZ[i] = WallTopFlat;
        }

        // Emit one quad per edge, with per-vertex top heights.
        for (int32 i = 0; i < N; i++)
        {
                int32 Next = (i + 1) % N;
                FVector B0(WorkPoints[i].X,    WorkPoints[i].Y,    BaseH);
                FVector B1(WorkPoints[Next].X, WorkPoints[Next].Y, BaseH);
                FVector T1(WorkPoints[Next].X, WorkPoints[Next].Y, TopZ[Next]);
                FVector T0(WorkPoints[i].X,    WorkPoints[i].Y,    TopZ[i]);

                FVector EdgeDir = B1 - B0;
                FVector OutN = FVector(EdgeDir.Y, -EdgeDir.X, 0);
                if (!OutN.IsNearlyZero()) OutN.Normalize(); else OutN = FVector(0, 0, 1);
                FVector Tang = EdgeDir; if (!Tang.IsNearlyZero()) Tang.Normalize(); else Tang = FVector(1, 0, 0);

                int32 BI = MeshData.Vertices.Num();
                MeshData.Vertices.Add(B0); MeshData.Vertices.Add(B1);
                MeshData.Vertices.Add(T1); MeshData.Vertices.Add(T0);
                MeshData.Triangles.Add(BI + 0); MeshData.Triangles.Add(BI + 2); MeshData.Triangles.Add(BI + 1);
                MeshData.Triangles.Add(BI + 0); MeshData.Triangles.Add(BI + 3); MeshData.Triangles.Add(BI + 2);

                float WW = FVector2D::Distance(FVector2D(B0.X, B0.Y), FVector2D(B1.X, B1.Y)) / 100.f;
                float V0 = BaseH   / 100.f;
                float V1i = TopZ[i]    / 100.f;
                float V1n = TopZ[Next] / 100.f;
                MeshData.UV0.Add(FVector2D(0,  V0));  MeshData.UV0.Add(FVector2D(WW, V0));
                MeshData.UV0.Add(FVector2D(WW, V1n)); MeshData.UV0.Add(FVector2D(0,  V1i));
                for (int32 v = 0; v < 4; v++)
                {
                        MeshData.Normals.Add(OutN);
                        MeshData.VertexColors.Add(WallColor);
                        MeshData.Tangents.Add(FProcMeshTangent(Tang, false));
                }
        }
}

// ============================================================================
// ============================================================================
// GEOMETRY HELPERS
// ============================================================================
// ============================================================================

// ============================================================================
// IsPointInTriangle2D
// ============================================================================
bool FGeoMeshGenerator::IsPointInTriangle2D(const FVector2D& P, const FVector2D& A, const FVector2D& B, const FVector2D& C)
{
        float d1 = (P.X - C.X) * (A.Y - C.Y) - (A.X - C.X) * (P.Y - C.Y);
        float d2 = (P.X - A.X) * (B.Y - A.Y) - (B.X - A.X) * (P.Y - A.Y);
        float d3 = (P.X - B.X) * (C.Y - B.Y) - (C.X - B.X) * (P.Y - B.Y);
        return !(((d1 < 0) || (d2 < 0) || (d3 < 0)) && ((d1 > 0) || (d2 > 0) || (d3 > 0)));
}

// ============================================================================
// ComputeSignedArea2D
// ============================================================================
float FGeoMeshGenerator::ComputeSignedArea2D(const TArray<FVector>& P)
{
        float A = 0.f; int32 N = P.Num();
        for (int32 i = 0; i < N; i++) { int32 j = (i + 1) % N; A += P[i].X * P[j].Y - P[j].X * P[i].Y; }
        return A * 0.5f;
}

// ============================================================================
// SignedDistanceToLine
// Signed distance from point to infinite line A->B
// Formula: (DX*(Point.Y - LineA.Y) - DY*(Point.X - LineA.X)) / Len
// Positive = left side, Negative = right side
// ============================================================================
float FGeoMeshGenerator::SignedDistanceToLine(
        const FVector2D& Point, const FVector2D& LineA, const FVector2D& LineB)
{
        float DX = LineB.X - LineA.X;
        float DY = LineB.Y - LineA.Y;
        float Len = FMath::Sqrt(DX * DX + DY * DY);
        if (Len < 0.0001f) return 0.f;
        return (DX * (Point.Y - LineA.Y) - DY * (Point.X - LineA.X)) / Len;
}

// ============================================================================
// SplitPolygon2D
// Split polygon by ray into sub-polygons (streets.gl algorithm)
// ============================================================================
bool FGeoMeshGenerator::SplitPolygon2D(
        const TArray<FVector2D>& Polygon,
        const FVector2D& RayOrig,
        const FVector2D& RayDir,
        TArray<TArray<FVector2D>>& OutPolygons)
{
        OutPolygons.Empty();
        if (Polygon.Num() < 3) return false;

        struct FIntersection
        {
                int32 EdgeStart;
                int32 EdgeEnd;
                FVector2D Point;
                float T;
                TArray<FVector2D>* Crossback;
        };

        TArray<FIntersection> Intersections;
        int32 N = Polygon.Num();

        FVector2D Start = Polygon[N - 1];
        for (int32 iVert = 0; iVert < N; iVert++)
        {
                FVector2D End = Polygon[iVert];
                float EdgeDX = End.X - Start.X;
                float EdgeDY = End.Y - Start.Y;
                float Den = RayDir.X * EdgeDY - RayDir.Y * EdgeDX;

                if (FMath::Abs(Den) > 0.0001f)
                {
                        float NumS = RayDir.X * (RayOrig.Y - Start.Y) - RayDir.Y * (RayOrig.X - Start.X);
                        float S = NumS / Den;

                        if (S >= -0.001f && S <= 1.001f)
                        {
                                FVector2D P(Start.X + S * EdgeDX, Start.Y + S * EdgeDY);
                                float NumT = EdgeDX * (RayOrig.Y - Start.Y) - EdgeDY * (RayOrig.X - Start.X);
                                float T = NumT / Den;

                                FIntersection Inter;
                                Inter.EdgeStart = (iVert + N - 1) % N;
                                Inter.EdgeEnd = iVert;
                                Inter.Point = P;
                                Inter.T = T;
                                Inter.Crossback = nullptr;
                                Intersections.Add(Inter);
                        }
                }
                Start = End;
        }

        Intersections.Sort([](const FIntersection& A, const FIntersection& B) {
                return A.T < B.T;
        });

        if (Intersections.Num() < 2 || Intersections.Num() % 2 != 0)
                return false;

        // Reserve enough capacity so that OutPolygons never reallocates while we hold
        // raw pointers to its elements in the Crossback fields.
        OutPolygons.Reserve(Intersections.Num() / 2 + 1);
        OutPolygons.Add(TArray<FVector2D>());
        TArray<FVector2D>* CurPoly = &OutPolygons[0];

        for (int32 iVert = 0; iVert < N; iVert++)
        {
                // Check for intersection on the edge ENDING at this vertex BEFORE adding the vertex.
                // The intersection point must be emitted to the current polygon first, then we switch
                // polygons, then the current vertex goes into the new polygon.
                // (Old code added the vertex first, placing both endpoints of the cut edge into the
                // same sub-polygon and producing wrong top/bottom splits instead of left/right splits.)
                int32 InterIdx = -1;
                for (int32 j = 0; j < Intersections.Num(); j++)
                {
                        if (Intersections[j].EdgeEnd == iVert)
                        {
                                InterIdx = j;
                                break;
                        }
                }

                if (InterIdx >= 0)
                {
                        CurPoly->Add(Intersections[InterIdx].Point);

                        if (InterIdx % 2 == 0)
                                Intersections[InterIdx + 1].Crossback = CurPoly;
                        else
                                Intersections[InterIdx - 1].Crossback = CurPoly;

                        if (Intersections[InterIdx].Crossback != nullptr)
                                CurPoly = Intersections[InterIdx].Crossback;
                        else
                        {
                                OutPolygons.Add(TArray<FVector2D>());
                                CurPoly = &OutPolygons[OutPolygons.Num() - 1];
                        }

                        CurPoly->Add(Intersections[InterIdx].Point);
                }

                CurPoly->Add(Polygon[iVert]);
        }

        return OutPolygons.Num() >= 2;
}

// ============================================================================
// InsetPolygonByEdgeOffset
// Shrink polygon by offsetting edges inward (robust for concave polygons)
// ============================================================================
TArray<FVector> FGeoMeshGenerator::InsetPolygonByEdgeOffset(
        const TArray<FVector>& Footprint, float InsetDist)
{
        int32 N = Footprint.Num();
        if (N < 3) return Footprint;

        TArray<FVector> WorkFP = Footprint;
        float SignedArea = 0.f;
        for (int32 i = 0; i < N; i++)
        {
                int32 j = (i + 1) % N;
                SignedArea += WorkFP[i].X * WorkFP[j].Y - WorkFP[j].X * WorkFP[i].Y;
        }
        if (SignedArea < 0.f) Algo::Reverse(WorkFP);

        TArray<FVector> Result;
        Result.Reserve(N);

        for (int32 i = 0; i < N; i++)
        {
                int32 Prev = (i == 0) ? N - 1 : i - 1;
                int32 Next = (i + 1) % N;

                FVector2D PPrev(WorkFP[Prev].X, WorkFP[Prev].Y);
                FVector2D PCurr(WorkFP[i].X, WorkFP[i].Y);
                FVector2D PNext(WorkFP[Next].X, WorkFP[Next].Y);

                FVector2D Edge1 = PCurr - PPrev;
                float Edge1Len = Edge1.Size();
                if (Edge1Len < 0.001f) { Result.Add(WorkFP[i]); continue; }
                Edge1 /= Edge1Len;
                FVector2D Norm1(-Edge1.Y, Edge1.X); // INWARD normal (CCW) -> positive InsetDist shrinks

                FVector2D Edge2 = PNext - PCurr;
                float Edge2Len = Edge2.Size();
                if (Edge2Len < 0.001f) { Result.Add(WorkFP[i]); continue; }
                Edge2 /= Edge2Len;
                FVector2D Norm2(-Edge2.Y, Edge2.X); // INWARD normal (CCW) -> positive InsetDist shrinks

                FVector2D L1A = PPrev + Norm1 * InsetDist;
                FVector2D L1B = PCurr + Norm1 * InsetDist;
                FVector2D L2A = PCurr + Norm2 * InsetDist;
                FVector2D L2B = PNext + Norm2 * InsetDist;

                float Den = (L1B.X - L1A.X) * (L2B.Y - L2A.Y) - (L1B.Y - L1A.Y) * (L2B.X - L2A.X);

                FVector2D Candidate;
                if (FMath::Abs(Den) < 0.001f)
                {
                        Candidate = (L1B + L2A) * 0.5f;
                }
                else
                {
                        float T = ((L2A.X - L1A.X) * (L2B.Y - L2A.Y) - (L2A.Y - L1A.Y) * (L2B.X - L2A.X)) / Den;
                        T = FMath::Clamp(T, -2.f, 2.f);
                        Candidate = FVector2D(L1A.X + T * (L1B.X - L1A.X), L1A.Y + T * (L1B.Y - L1A.Y));
                }

                // For concave corners the inset vertex can overshoot outside the
                // polygon. Clamp it back: if it moved further from the centroid
                // than the original vertex, snap to a centroid-lerp position.
                {
                        FVector2D OrigXY(PCurr.X, PCurr.Y);
                        FVector2D CenXY(0.f, 0.f);
                        for (const FVector& V : WorkFP) { CenXY.X += V.X; CenXY.Y += V.Y; }
                        CenXY /= (float)N;
                        float OrigDistToCen = FVector2D::Distance(OrigXY, CenXY);
                        float CandDistToCen = FVector2D::Distance(Candidate, CenXY);
                        if (CandDistToCen > OrigDistToCen || FVector2D::Distance(Candidate, OrigXY) > InsetDist * 3.f)
                        {
                                float MoveAmt = FMath::Min(InsetDist, OrigDistToCen * 0.45f);
                                FVector2D Dir = CenXY - OrigXY;
                                if (!Dir.IsNearlyZero()) Dir.Normalize();
                                Candidate = OrigXY + Dir * MoveAmt;
                        }
                }

                Result.Add(FVector(Candidate.X, Candidate.Y, WorkFP[i].Z));
        }

        return Result;
}

// ============================================================================
// TriangulatePolygon2D
// Ear-clipping triangulation of 2D polygon
// ============================================================================
void FGeoMeshGenerator::TriangulatePolygon2D(
        const TArray<FVector2D>& Polygon,
        TArray<int32>& OutTriangles)
{
        OutTriangles.Empty();
        int32 N = Polygon.Num();
        if (N < 3) return;

        TArray<int32> Idx;
        for (int32 i = 0; i < N; i++) Idx.Add(i);

        int32 Fail = 0;
        while (Idx.Num() > 3)
        {
                bool bEar = false;
                int32 NI = Idx.Num();

                for (int32 i = 0; i < NI; i++)
                {
                        int32 ip = (i == 0) ? NI - 1 : i - 1;
                        int32 in2 = (i + 1) % NI;
                        int32 Ip = Idx[ip], Ic = Idx[i], In = Idx[in2];

                        const FVector2D& vp = Polygon[Ip];
                        const FVector2D& vc = Polygon[Ic];
                        const FVector2D& vn = Polygon[In];

                        float Cross = (vc.X - vp.X) * (vn.Y - vp.Y) - (vc.Y - vp.Y) * (vn.X - vp.X);
                        if (FMath::Abs(Cross) <= 0.01f) continue;

                        bool bIsEar = true;
                        for (int32 j = 0; j < NI; j++)
                        {
                                if (j == ip || j == i || j == in2) continue;
                                if (IsPointInTriangle2D(Polygon[Idx[j]], vp, vc, vn))
                                {
                                        bIsEar = false;
                                        break;
                                }
                        }

                        if (bIsEar)
                        {
                                OutTriangles.Add(Ip);
                                OutTriangles.Add(Ic);
                                OutTriangles.Add(In);
                                Idx.RemoveAt(i);
                                bEar = true;
                                Fail = 0;
                                break;
                        }
                }

                if (!bEar)
                {
                        Fail++;
                        if (Fail > NI * 3)
                        {
                                for (int32 i = 1; i < Idx.Num() - 1; i++)
                                {
                                        OutTriangles.Add(Idx[0]);
                                        OutTriangles.Add(Idx[i]);
                                        OutTriangles.Add(Idx[i + 1]);
                                }
                                return;
                        }
                }
        }

        if (Idx.Num() == 3)
        {
                OutTriangles.Add(Idx[0]);
                OutTriangles.Add(Idx[1]);
                OutTriangles.Add(Idx[2]);
        }
}

// ============================================================================
// ComputeFootprintBounds
// ============================================================================
void FGeoMeshGenerator::ComputeFootprintBounds(
        const TArray<FVector>& FP,
        float& MinX, float& MaxX, float& MinY, float& MaxY)
{
        MinX = FLT_MAX; MaxX = -FLT_MAX; MinY = FLT_MAX; MaxY = -FLT_MAX;
        for (const FVector& V : FP)
        {
                if (V.X < MinX) MinX = V.X; if (V.X > MaxX) MaxX = V.X;
                if (V.Y < MinY) MinY = V.Y; if (V.Y > MaxY) MaxY = V.Y;
        }
}

// ============================================================================
// ComputePolygonCentroid
// ============================================================================
FVector FGeoMeshGenerator::ComputePolygonCentroid(const TArray<FVector>& FP)
{
        FVector C(0, 0, 0);
        for (const FVector& V : FP) C += FVector(V.X, V.Y, 0);
        return C / (float)FP.Num();
}

// ============================================================================
// IsLongerAlongX
// ============================================================================
bool FGeoMeshGenerator::IsLongerAlongX(const TArray<FVector>& FP)
{
        float MinX, MaxX, MinY, MaxY;
        ComputeFootprintBounds(FP, MinX, MaxX, MinY, MaxY);
        return (MaxX - MinX) >= (MaxY - MinY);
}

// ============================================================================
// InsetPolygon (centroid-based, for flat roofs)
// ============================================================================
TArray<FVector> FGeoMeshGenerator::InsetPolygon(const TArray<FVector>& FP, float InsetDist)
{
        int32 N = FP.Num();
        if (N < 3) return FP;

        FVector Cen = ComputePolygonCentroid(FP);
        TArray<FVector> Out;
        for (const FVector& V : FP)
        {
                FVector2D Dir2D(Cen.X - V.X, Cen.Y - V.Y);
                float Len = Dir2D.Size();
                if (Len < 0.01f) { Out.Add(V); continue; }
                Dir2D /= Len;
                float MoveAmt = FMath::Min(InsetDist, Len * 0.45f);
                Out.Add(FVector(V.X + Dir2D.X * MoveAmt, V.Y + Dir2D.Y * MoveAmt, V.Z));
        }
        return Out;
}

// ============================================================================
// ComputeDataBounds
// ============================================================================
void FGeoMeshGenerator::ComputeDataBounds(
        const TArray<FGeoBuilding>& Buildings,
        const TArray<FGeoRoad>& Roads,
        const TArray<FGeoWater>& WaterBodies,
        const TArray<FGeoVegetation>& Vegetations,
        float& OutMinX, float& OutMaxX, float& OutMinY, float& OutMaxY)
{
        OutMinX = FLT_MAX; OutMaxX = -FLT_MAX; OutMinY = FLT_MAX; OutMaxY = -FLT_MAX;
        auto Expand = [&](float X, float Y)
        {
                if (X < OutMinX) OutMinX = X; if (X > OutMaxX) OutMaxX = X;
                if (Y < OutMinY) OutMinY = Y; if (Y > OutMaxY) OutMaxY = Y;
        };
        for (const FGeoBuilding& B : Buildings) for (const FVector2D& N : B.Nodes) Expand(N.X, N.Y);
        for (const FGeoRoad& R : Roads) for (const FVector2D& P : R.Points) Expand(P.X, P.Y);
        for (const FGeoWater& W : WaterBodies) for (const FVector2D& P : W.Points) Expand(P.X, P.Y);
        for (const FGeoVegetation& V : Vegetations) for (const FVector2D& P : V.Points) Expand(P.X, P.Y);
}

// ============================================================================
// ============================================================================
// ROOF SHAPE GENERATORS - streets.gl approach
// ============================================================================
// ============================================================================

// ----------------------------------------------------------------------------
// BarrelAxisDir — barrel (ridge) direction for a round roof, taken from the
// FOOTPRINT'S OWN EDGES rather than the world-aligned AABB, so the barrel always
// follows the base walls even when the building sits at an angle. The footprint's
// longest edge defines the long axis; "along" (default) runs the ridge along it,
// "across" runs it perpendicular. Shared by AddRoundRoof and ComputeBarrelRise so
// the rise used for wall-lowering matches the geometry the roof builder emits.
// ----------------------------------------------------------------------------
static FVector2D BarrelAxisDir(const TArray<FVector>& FP, const FString& RoofOrientation)
{
        const int32 N = FP.Num();
        FVector2D LongDir(1.f, 0.f);
        float BestLen = -1.f;
        for (int32 i = 0; i < N; i++)
        {
                const FVector& A = FP[i];
                const FVector& B = FP[(i + 1) % N];
                FVector2D E(B.X - A.X, B.Y - A.Y);
                const float Len = E.Size();
                if (Len > BestLen) { BestLen = Len; LongDir = E / FMath::Max(Len, KINDA_SMALL_NUMBER); }
        }

        const bool bAcross = (RoofOrientation.ToLower() == TEXT("across"));
        // Default "along": ridge runs along the longest edge. "across": perpendicular.
        return bAcross ? FVector2D(-LongDir.Y, LongDir.X) : LongDir;
}

// ============================================================================
// ComputeBarrelRise — rise (arch radius) of a round/barrel roof = half the
// footprint span perpendicular to the (rotated) barrel axis. Footprint must be
// in local units (cm). Mirrors AddRoundRoof so wall-lowering matches the roof.
// ============================================================================
static float ComputeBarrelRise(const TArray<FVector>& FP, const FString& RoofOrientation)
{
        if (FP.Num() < 3) return 0.f;
        const FVector2D AxisDir = BarrelAxisDir(FP, RoofOrientation);
        const FVector2D CrossDir(-AxisDir.Y, AxisDir.X);
        float CrossMin = FLT_MAX, CrossMax = -FLT_MAX;
        for (const FVector& V : FP)
        {
                const float CP = V.X * CrossDir.X + V.Y * CrossDir.Y;
                CrossMin = FMath::Min(CrossMin, CP);
                CrossMax = FMath::Max(CrossMax, CP);
        }
        return (CrossMax - CrossMin) * 0.5f;
}

// ============================================================================
// GentleSkillionRise — soften over-steep SMALL triangular skillion "shoulders".
// The little triangular skillion parts that flank a rounded apse (e.g. the
// Frauenkirche) carry a roof:height far larger than their tiny run, so they
// render as near-vertical (~60°) slanted walls. Clamp those to a believable
// pitch (~24°). Only footprints with up to 4 vertices are affected, so large
// multi-vertex skillion towers/spires keep their intended steep design.
// Footprint and rise are in local units (cm); RoofDirectionDeg < 0 means "unset".
// ============================================================================
static float GentleSkillionRise(const TArray<FVector>& FP, float RoofDirectionDeg, float Rise)
{
        if (RoofDirectionDeg < 0.f || FP.Num() < 2 || Rise <= 0.f) return Rise;
        if (FP.Num() > 4) return Rise;   // only small triangular/quad shoulders

        const float D = FMath::DegreesToRadians(RoofDirectionDeg);
        const FVector2D Dir(FMath::Sin(D), FMath::Cos(D));
        float MinP = FLT_MAX, MaxP = -FLT_MAX;
        for (const FVector& V : FP)
        {
                const float P = V.X * Dir.X + V.Y * Dir.Y;
                MinP = FMath::Min(MinP, P); MaxP = FMath::Max(MaxP, P);
        }
        const float Run = FMath::Max(MaxP - MinP, 1.f);
        const float MaxRatio = 0.45f;    // tan(~24°) — cap the pitch
        return FMath::Min(Rise, MaxRatio * Run);
}

// Does a pitched roof shape have a real eave below its peak?
static bool RoofShapeHasEave(ERoofShape S)
{
        switch (S)
        {
        case ERoofShape::Gabled:    case ERoofShape::Saltbox:
        case ERoofShape::QuadrupleSaltbox: case ERoofShape::CrossGabled:
        case ERoofShape::Hipped:    case ERoofShape::HalfHipped:
        case ERoofShape::Pyramidal: case ERoofShape::Mansard:
        case ERoofShape::Gambrel:   case ERoofShape::Round:
        case ERoofShape::Skillion:
                return true;
        default:
                return false;
        }
}

// Two parts are adjacent if their footprints share an EDGE (>=2 common nodes).
static bool PartsShareEdge(const FGeoBuilding& A, const FGeoBuilding& B)
{
        int32 Shared = 0;
        for (const FVector2D& N : A.Nodes)
        {
                for (const FVector2D& M : B.Nodes)
                        if (N.Equals(M, 0.05f)) { Shared++; break; }
                if (Shared >= 2) return true;
        }
        return false;
}

// True if this part shares a footprint edge with any onion/dome turret.
static bool IsAdjacentToOnionDome(const FGeoBuilding& B, const TArray<FGeoBuilding>& All)
{
        for (const FGeoBuilding& O : All)
        {
                if (&O == &B) continue;
                if (O.RoofShape != ERoofShape::Onion && O.RoofShape != ERoofShape::Dome) continue;
                if (PartsShareEdge(B, O)) return true;
        }
        return false;
}

// ============================================================================
// AdjacentRoofEave — for an onion/dome turret, the LOWEST pitched-roof eave (cm)
// found anywhere in the connected building complex the turret belongs to. Parts
// are connected when they share a footprint edge; a breadth-first walk over that
// graph means a turret aligns to the main roofline even when it only touches a
// neighbour (e.g. a skillion shoulder) that in turn connects to the main gabled
// roof. Returns -1 if the complex has no pitched roof. Fully generic.
// ============================================================================
static float AdjacentRoofEave(const FGeoBuilding& B, const TArray<FGeoBuilding>& All, float ScaleFactor)
{
        const int32 N = All.Num();
        int32 Start = -1;
        for (int32 i = 0; i < N; i++) if (&All[i] == &B) { Start = i; break; }
        if (Start < 0) return -1.f;

        TArray<bool> Visited; Visited.Init(false, N);
        TArray<int32> Queue;  Queue.Add(Start); Visited[Start] = true;

        float Best = -1.f;
        for (int32 qi = 0; qi < Queue.Num(); qi++)
        {
                const FGeoBuilding& C = All[Queue[qi]];

                if (Queue[qi] != Start && C.RoofHeight > 0.0 && RoofShapeHasEave(C.RoofShape))
                {
                        const float Eave = (float)((C.Height - C.RoofHeight) * (double)ScaleFactor);
                        if (Eave > 1.f && (Best < 0.f || Eave < Best)) Best = Eave;
                }

                for (int32 j = 0; j < N; j++)
                        if (!Visited[j] && PartsShareEdge(C, All[j]))
                        {
                                Visited[j] = true;
                                Queue.Add(j);
                        }
        }
        return Best;
}

// ============================================================================
// DomeNestleEave — the eave a dome/onion/skillion part should "nestle into",
// or -1 to disable the heuristic for this part.
//
// The "drop the column / stretch the skillion down to the adjacent roofline"
// behaviour is meant for a SMALL turret or shoulder poking out of a neighbour's
// pitched roof. It must NOT fire for a free-standing tower (building:part=tower,
// e.g. the 60 m Alter Peter tower) nor for any part so much taller than the
// surrounding eave that attaching would collapse it. The guard: skip towers
// outright, and only nestle when the eave sits in the UPPER HALF of the part —
// a turret pops out a little (eave high), a tower towers over a low roofline.
// Top/Base are the part's absolute top and base in the same (local cm) units.
// ============================================================================
static float DomeNestleEave(const FGeoBuilding& B, const TArray<FGeoBuilding>& All,
        float ScaleFactor, float Top, float Base)
{
        if (B.bIsTowerPart) return -1.f;                       // free-standing tower
        const float Eave = AdjacentRoofEave(B, All, ScaleFactor);
        if (Eave <= Base + 1.f || Eave >= Top) return -1.f;    // no usable neighbour eave
        if (Eave < 0.5f * (Top - Base) + Base) return -1.f;    // a tower, not a turret
        return Eave;
}

// ============================================================================
// AddRoofGeometry - router that dispatches based on ERoofShape
// ============================================================================
void FGeoMeshGenerator::AddRoofGeometry(
        FMeshData& OutRoofData,
        ERoofShape RoofShape,
        const TArray<FVector>& LocalFootprint,
        float WallTopZ,
        float RoofH,
        const FColor& RoofFColor,
        const TArray<TArray<FVector2D>>& Holes,
        float RoofDirection,
        const FString& RoofOrientation,
        const FColor& WallColor)
{
        switch (RoofShape)
        {
        case ERoofShape::Flat:
        default:
        {
                // streets.gl FlatRoofBuilder: the flat roof is simply the
                // building footprint triangulated flush with the wall top
                // (no parapet, no inset). The robust CGAL CDT handles concave
                // footprints (L/U-shaped buildings) and courtyard holes
                // correctly, replacing the old inset+parapet hack that produced
                // an extra plane across the U-notch and detached upward caps.
                const float RoofZ = WallTopZ;

                TArray<FVector2D> Outer2D;
                Outer2D.Reserve(LocalFootprint.Num());
                for (const FVector& V : LocalFootprint) Outer2D.Add(FVector2D(V.X, V.Y));

                TArray<int32> Tris;
                TArray<FVector2D> Steiner;
                FCGALSkeletonGenerator::TriangulateWithCGAL(Outer2D, Holes, Tris, Steiner);

                if (Tris.Num() >= 3)
                {
                        TArray<FVector2D> All;
                        All.Reserve(Outer2D.Num() + Steiner.Num());
                        All.Append(Outer2D);
                        All.Append(Steiner);

                        // Flat-roof UVs: stretch ONE texture across the whole
                        // building footprint, oriented to the building's principal
                        // axis (its longest footprint edge), so the texture's U/V
                        // run parallel to the walls (the building's right-angle
                        // frame) instead of along world X/Y. Mirrors streets-gl's
                        // stretched FlatRoofBuilder (oriented bounding box).
                        FVector2D Dir(1.f, 0.f);
                        {
                                float BestLen = -1.f;
                                const int32 N = Outer2D.Num();
                                for (int32 i = 0; i < N; ++i)
                                {
                                        const FVector2D Edge = Outer2D[(i + 1) % N] - Outer2D[i];
                                        const float L = Edge.SizeSquared();
                                        if (L > BestLen) { BestLen = L; Dir = Edge; }
                                }
                                if (!Dir.Normalize()) Dir = FVector2D(1.f, 0.f);
                        }
                        const FVector2D Perp(-Dir.Y, Dir.X);

                        // Oriented bounding box of the footprint in (Dir, Perp).
                        float UMin = FLT_MAX, UMax = -FLT_MAX, VMin = FLT_MAX, VMax = -FLT_MAX;
                        for (const FVector2D& P : Outer2D)
                        {
                                const float U = FVector2D::DotProduct(P, Dir);
                                const float V = FVector2D::DotProduct(P, Perp);
                                UMin = FMath::Min(UMin, U); UMax = FMath::Max(UMax, U);
                                VMin = FMath::Min(VMin, V); VMax = FMath::Max(VMax, V);
                        }
                        const float USize = FMath::Max(UMax - UMin, 1.f);
                        const float VSize = FMath::Max(VMax - VMin, 1.f);

                        const int32 Base = OutRoofData.Vertices.Num();
                        for (const FVector2D& P : All)
                        {
                                OutRoofData.Vertices.Add(FVector(P.X, P.Y, RoofZ));
                                OutRoofData.Normals.Add(FVector(0, 0, 1));
                                OutRoofData.VertexColors.Add(RoofFColor);
                                OutRoofData.Tangents.Add(FProcMeshTangent(FVector(Dir.X, Dir.Y, 0.f), false));
                                const float U = (FVector2D::DotProduct(P, Dir) - UMin) / USize;
                                const float V = (FVector2D::DotProduct(P, Perp) - VMin) / VSize;
                                OutRoofData.UV0.Add(FVector2D(U, V));
                        }
                        for (int32 T : Tris) OutRoofData.Triangles.Add(Base + T);
                }
                else
                {
                        AddPolygonTriangulated(OutRoofData, LocalFootprint, RoofZ, RoofFColor);
                }
        }
        break;

        case ERoofShape::Gabled:
        case ERoofShape::Saltbox:
        case ERoofShape::QuadrupleSaltbox:
        case ERoofShape::CrossGabled:
                // CGAL straight-skeleton roof first; gabled skeleton as fallback.
                if (!AddStraightSkeletonRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor, Holes))
                        AddSkeletonGabledRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor, Holes, RoofDirection, RoofOrientation);
                break;

        case ERoofShape::Hipped:
        case ERoofShape::HalfHipped:
        case ERoofShape::Pyramidal:
                // CGAL straight-skeleton roof first; pyramid/hipped skeleton as fallback.
                if (!AddStraightSkeletonRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor, Holes))
                {
                        if (RoofShape == ERoofShape::Pyramidal)
                                AddPyramidalRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor);
                        else
                                AddSkeletonHippedRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor, Holes);
                }
                break;

        case ERoofShape::Skillion:
                AddSkillionRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor, RoofDirection);
                break;

        case ERoofShape::Mansard:
                AddSkeletonMansardRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor);
                break;

        case ERoofShape::Gambrel:
                AddGambrelRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor, RoofDirection);
                break;

        case ERoofShape::Dome:
                AddDomeRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor, false);
                break;

        case ERoofShape::Onion:
                AddDomeRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor, true);
                break;

        case ERoofShape::Round:
                AddRoundRoof(OutRoofData, LocalFootprint, WallTopZ, RoofH, RoofFColor, RoofDirection, RoofOrientation, WallColor);
                break;
        }
}

// ============================================================================
// AddPyramidalRoof - simple apex at centroid
// ============================================================================
void FGeoMeshGenerator::AddPyramidalRoof(
        FMeshData& OutData, const TArray<FVector>& FP,
        float BaseZ, float ApexH, const FColor& Color)
{
        if (FP.Num() < 3) return;

        TArray<FVector> CleanFP;
        for (const FVector& V : FP)
        {
                FVector P(V.X, V.Y, BaseZ);
                if (CleanFP.Num() > 0 && P.Equals(CleanFP.Last(), 0.5f)) continue;
                if (CleanFP.Num() > 0 && P.Equals(CleanFP[0], 0.5f)) continue;
                CleanFP.Add(P);
        }
        if (CleanFP.Num() < 3) return;
        if (ComputeSignedArea2D(CleanFP) < 0.f) Algo::Reverse(CleanFP);

        FVector Apex = ComputePolygonCentroid(CleanFP);
        Apex.Z = BaseZ + ApexH;

        for (int32 i = 0; i < CleanFP.Num(); i++)
        {
                int32 Next = (i + 1) % CleanFP.Num();
                const FVector& A = CleanFP[i];
                const FVector& B = CleanFP[Next];
                if (FVector2D::Distance(FVector2D(A.X, A.Y), FVector2D(B.X, B.Y)) < 1.f) continue;
                if (A.Equals(Apex, 0.5f) || B.Equals(Apex, 0.5f)) continue;
                AddTriangle(OutData, A, B, Apex, Color);
        }
}

// ============================================================================
// AddSkillionRoof - single-direction slope
// ============================================================================
void FGeoMeshGenerator::AddSkillionRoof(
        FMeshData& OutData, const TArray<FVector>& FP,
        float BaseZ, float RidgeH, const FColor& Color,
        float RoofDirection)
{
        if (FP.Num() < 3) return;

        if (RoofDirection >= 0.f)
        {
                // Match streets-gl SkillionRoofBuilder.getRotation:
                //   rotation = -toRad(direction) - PI/2, slope-up = (-cos D, -sin D).
                // OSM roof:direction is the downslope azimuth (clockwise from north).
                // In local space X=east, Y=-north, so the upslope (toward the high edge)
                // is (-sin D, cos D). MaxProj = high edge, MinProj = low edge.
                float D_rad = FMath::DegreesToRadians(RoofDirection);
                FVector2D SlopeDir(-FMath::Sin(D_rad), FMath::Cos(D_rad));

                float MinProj = FLT_MAX, MaxProj = -FLT_MAX;
                for (const FVector& V : FP)
                {
                        float Proj = V.X * SlopeDir.X + V.Y * SlopeDir.Y;
                        MinProj = FMath::Min(MinProj, Proj);
                        MaxProj = FMath::Max(MaxProj, Proj);
                }
                float Range = FMath::Max(MaxProj - MinProj, 1.f);

                TArray<FVector> SlopedFP;
                for (const FVector& V : FP)
                {
                        float Proj = V.X * SlopeDir.X + V.Y * SlopeDir.Y;
                        float T = (Proj - MinProj) / Range; // T=0 -> low edge (faces roof:direction), T=1 -> high edge
                        // OSM roof:direction points toward the LOW eaves. SlopeDir is
                        // built along that direction, so MinProj is the low edge and
                        // must sit on the wall top (Z=BaseZ); the high edge reaches
                        // BaseZ+RidgeH. The old Lerp(RidgeH,0,T) inverted this, lifting
                        // the low eaves into the air (detached, upward-tilted roof).
                        float Z = BaseZ + FMath::Lerp(0.f, RidgeH, T);
                        SlopedFP.Add(FVector(V.X, V.Y, Z));
                }
                AddPolygonTriangulated(OutData, SlopedFP, 0.f, Color, true);
                return;
        }

        float MinX, MaxX, MinY, MaxY;
        ComputeFootprintBounds(FP, MinX, MaxX, MinY, MaxY);

        bool bLongX = (MaxX - MinX) >= (MaxY - MinY);

        TArray<FVector> SlopedFP;
        for (const FVector& V : FP)
        {
                float T;
                if (bLongX)
                        T = (MaxY > MinY) ? (V.Y - MinY) / (MaxY - MinY) : 0.f;
                else
                        T = (MaxX > MinX) ? (V.X - MinX) / (MaxX - MinX) : 0.f;
                // Low edge (T=0) sits on the wall top; high edge (T=1) at BaseZ+RidgeH.
                float Z = BaseZ + FMath::Lerp(0.f, RidgeH, T);
                SlopedFP.Add(FVector(V.X, V.Y, Z));
        }
        AddPolygonTriangulated(OutData, SlopedFP, 0.f, Color, true);
}

// ============================================================================
// AddMansardRoof - inset polygon + lower extrusion + upper pyramidal
// Algorithm:
// 1. LowerH = RidgeH * 0.6, UpperH = RidgeH * 0.4
// 2. MidZ = BaseZ + LowerH
// 3. InsetFP = InsetPolygonByEdgeOffset(FP, LowerH * 0.8)
// 4. AddExtrudedPolygon for the inset (BaseZ to MidZ) - steep lower part
// 5. AddPyramidalRoof on inset at MidZ with UpperH - upper part
// ============================================================================
void FGeoMeshGenerator::AddMansardRoof(
        FMeshData& OutData, const TArray<FVector>& FP,
        float BaseZ, float RidgeH, const FColor& Color)
{
        if (FP.Num() < 3) return;
        float LowerH = RidgeH * 0.6f;
        float UpperH = RidgeH * 0.4f;
        float MidZ = BaseZ + LowerH;

        TArray<FVector> InsetFP = InsetPolygonByEdgeOffset(FP, LowerH * 0.8f);
        for (FVector& V : InsetFP) V.Z = 0.f;

        AddExtrudedPolygon(OutData, InsetFP, BaseZ, MidZ, Color, false);
        AddPyramidalRoof(OutData, InsetFP, MidZ, UpperH, Color);
}

// ============================================================================
// AddDomeRoof - smooth hemisphere with shared vertices and analytical normals.
//
// Uses many angular segments (independent of base polygon complexity) so the
// dome surface is smooth regardless of whether the base is a quad or octagon.
// Vertices are shared between adjacent quads so Gouraud shading interpolates
// normals across the surface without stepped lighting bands.
//
// Algorithm:
//   1. Cast a ray from the AABB centre at each angular sample to find the
//      base-polygon boundary radius at that angle.
//   2. Scale that radius by cos(Phi) per ring to project onto the ellipsoid.
//   3. Z = BaseZ + DomeH * sin(Phi).
//   4. Normals are the ellipsoid gradient (analytical, smooth).
//   5. For bOnion: radius scale = cos(Phi) * (1 + 0.8 * sin(Phi)) to bulge mid.
// ============================================================================
void FGeoMeshGenerator::AddDomeRoof(
        FMeshData& OutData, const TArray<FVector>& FP,
        float BaseZ, float DomeH, const FColor& Color,
        bool bOnion)
{
        if (FP.Num() < 3) return;

        // Clean footprint, ensure CCW.
        TArray<FVector> CleanFP;
        for (const FVector& V : FP)
        {
                FVector P(V.X, V.Y, BaseZ);
                if (CleanFP.Num() > 0 && P.Equals(CleanFP.Last(), 0.5f)) continue;
                if (CleanFP.Num() > 0 && P.Equals(CleanFP[0], 0.5f)) continue;
                CleanFP.Add(P);
        }
        if (CleanFP.Num() < 3) return;
        if (ComputeSignedArea2D(CleanFP) < 0.f) Algo::Reverse(CleanFP);

        float MinX, MaxX, MinY, MaxY;
        ComputeFootprintBounds(CleanFP, MinX, MaxX, MinY, MaxY);
        const FVector2D Cen2D((MinX + MaxX) * 0.5f, (MinY + MaxY) * 0.5f);
        const float RadX = FMath::Max((MaxX - MinX) * 0.5f, 1.f);
        const float RadY = FMath::Max((MaxY - MinY) * 0.5f, 1.f);
        const int32 N = CleanFP.Num();

        // Ray from centre → polygon boundary at angle Theta.
        auto PolygonRadius = [&](float Theta) -> float
        {
                FVector2D Dir(FMath::Cos(Theta), FMath::Sin(Theta));
                float Best = FLT_MAX;
                for (int32 i = 0; i < N; i++)
                {
                        FVector2D A(CleanFP[i].X - Cen2D.X, CleanFP[i].Y - Cen2D.Y);
                        FVector2D B(CleanFP[(i + 1) % N].X - Cen2D.X, CleanFP[(i + 1) % N].Y - Cen2D.Y);
                        FVector2D AB = B - A;
                        float Denom = Dir.X * AB.Y - Dir.Y * AB.X;
                        if (FMath::Abs(Denom) < 1e-6f) continue;
                        float t = (A.X * AB.Y - A.Y * AB.X) / Denom;
                        float s = -(Dir.X * A.Y - Dir.Y * A.X) / Denom;
                        if (t > 0.f && s >= -0.001f && s <= 1.001f)
                                Best = FMath::Min(Best, t);
                }
                return (Best < FLT_MAX) ? Best : FMath::Max(RadX, RadY);
        };

        // More segments = smoother silhouette; independent of base polygon vertex count.
        const int32 Segments = FMath::Max(32, N * 6);
        // Onion needs more rings so the slender spire/neck near the top is not faceted.
        const int32 Rings    = bOnion ? 28 : 16;

        // Width blend for onion silhouette: 0 = narrow/slender bulb (towers),
        // 1 = wide bulb (entrance). Driven by the SCALE-INVARIANT aspect ratio
        // (half short side / dome height) so it is independent of world units:
        // narrow tower bulbs measure ~0.39, wide entrance bulbs ~0.69. Narrow
        // bulbs stay slender; wide bulbs get a taller belly and a sharper,
        // shorter tip so a broad bulb does not look squashed.
        const float OnionHalfShort = FMath::Min(RadX, RadY);
        const float OnionAspect = (DomeH > 1.f) ? (OnionHalfShort / DomeH) : 0.39f;
        const float OnionWBlend = FMath::Clamp((OnionAspect - 0.39f) / (0.69f - 0.39f), 0.f, 1.f);

        // -----------------------------------------------------------------------
        // Profile function — returns { heightFrac (0..1), radScale } for ring t in [0,1].
        //
        // Onion: a classic *ogee* (S-shaped) cross-section instead of a single
        // sinusoid.  The silhouette is defined by hand-tuned control points
        // (heightFrac : radScale) smoothly interpolated with Catmull-Rom:
        //
        //   - springs from the drum at radScale = 1.0
        //   - bulges out gently to ~1.20x at ~20% height (the "belly")
        //   - curves back inward through an S inflection in the mid section
        //   - pinches to a slender neck (~0.13x) around 90% height
        //   - draws out to a fine point at the apex
        //
        // The control points are denser near the top so the spire gets more rings
        // and stays smooth.  Same profile is used for every base polygon (no
        // special case for quads) — the footprint shape is handled separately by
        // PolygonRadius(), so a square base produces a square-belly onion and an
        // octagon a round one, both with the correct ogee silhouette.
        //
        // Dome (non-onion): plain hemisphere — sinP / cosP of t*PI/2.
        // -----------------------------------------------------------------------
        auto ProfileAt = [&](float t) -> TPair<float, float>
        {
                if (bOnion)
                {
                        // Two ogee onion silhouettes (heightFrac : radScale), blended by
                        // bulb width via OnionWBlend. Both share the classic shape: plump
                        // belly low down, a concave waist, then a slender drawn-out finial.
                        //
                        //  NARROW (tower bulb): slender, belly to ~38% height, fine tip.
                        //  WIDE  (entrance bulb): belly pushed taller (to ~45% height) so a
                        //    broad bulb does not look squashed, and the tip is thinner and
                        //    SHORTER (compressed into the top), giving a sharper point.
                        static const float Hn[] = { 0.00f, 0.07f, 0.18f, 0.28f, 0.38f, 0.47f, 0.56f, 0.64f, 0.72f, 0.80f, 0.87f, 0.94f, 1.00f };
                        static const float Rn[] = { 1.00f, 1.14f, 1.23f, 1.20f, 1.05f, 0.78f, 0.50f, 0.31f, 0.19f, 0.11f, 0.060f, 0.025f, 0.00f };
                        static const float Hw[] = { 0.00f, 0.09f, 0.22f, 0.34f, 0.45f, 0.55f, 0.63f, 0.71f, 0.78f, 0.84f, 0.90f, 0.95f, 1.00f };
                        static const float Rw[] = { 1.00f, 1.16f, 1.26f, 1.23f, 1.08f, 0.78f, 0.46f, 0.26f, 0.14f, 0.075f, 0.035f, 0.013f, 0.00f };
                        const int32 M = 13;

                        float  c = FMath::Clamp(t, 0.f, 1.f) * (float)(M - 1);
                        int32  i = FMath::Clamp((int32)c, 0, M - 2);
                        float  f = c - (float)i;

                        // Catmull-Rom interpolation of a control array at local param f
                        // within segment [i, i+1], clamping the outer tangent points.
                        auto CR = [&](const float* A) -> float
                        {
                                float p0 = A[FMath::Max(i - 1, 0)];
                                float p1 = A[i];
                                float p2 = A[i + 1];
                                float p3 = A[FMath::Min(i + 2, M - 1)];
                                float f2 = f * f, f3 = f2 * f;
                                return 0.5f * ((2.f * p1)
                                        + (-p0 + p2) * f
                                        + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * f2
                                        + (-p0 + 3.f * p1 - 3.f * p2 + p3) * f3);
                        };

                        float h = FMath::Lerp(CR(Hn), CR(Hw), OnionWBlend);
                        float r = FMath::Lerp(CR(Rn), CR(Rw), OnionWBlend);
                        return { FMath::Clamp(h, 0.f, 1.f), FMath::Max(r, 0.f) };
                }

                // Hemisphere dome.
                float Phi = t * PI * 0.5f;
                return { FMath::Sin(Phi), FMath::Cos(Phi) };
        };

        // Determine which segment columns sit on a footprint vertex.
        // A segment s is a "seam" column if its angle Theta_s is within a small
        // tolerance of the angle from the centroid to any footprint vertex.
        // At seam columns the vertex is duplicated so the two flanking face-groups
        // get independent normals → hard crease.  All other columns are shared
        // → smooth interpolation across rings and across non-seam segment boundaries.
        //
        // This gives exactly N sharp ridges (one per footprint edge) rising from
        // the footprint corners to the apex, while the dome surface between ridges
        // is perfectly smooth.
        // -----------------------------------------------------------------------

        // Precompute the angle of each footprint vertex from the centroid.
        TArray<float> FootprintAngles;
        FootprintAngles.Reserve(N);
        for (int32 i = 0; i < N; i++)
        {
                float Ax = CleanFP[i].X - Cen2D.X;
                float Ay = CleanFP[i].Y - Cen2D.Y;
                FootprintAngles.Add(FMath::Atan2(Ay, Ax));  // [-PI, PI]
        }

        // For each footprint vertex, find the single nearest segment column index.
        // That column (and only that column) becomes a seam — exactly N seams total.
        // Also store the exact footprint vertex position for snapping RingPts.
        TSet<int32>             SeamSet;
        TMap<int32, FVector2D>  SeamSnap;   // column s → exact footprint vertex XY
        for (int32 i = 0; i < N; i++)
        {
                float FA    = FootprintAngles[i];
                float Angle = FA < 0.f ? FA + 2.f * PI : FA;

                float ExactS   = Angle / (2.f * PI) * (float)Segments;
                int32 NearestS = FMath::RoundToInt(ExactS) % Segments;
                SeamSet.Add(NearestS);
                // Store the footprint vertex centroid-relative position.
                SeamSnap.Add(NearestS, FVector2D(
                        CleanFP[i].X - Cen2D.X,
                        CleanFP[i].Y - Cen2D.Y));
        }

        // Returns true only for the one segment column that is closest to a footprint vertex.
        auto IsSeamColumn = [&](int32 s) -> bool
        {
                return SeamSet.Contains(s);
        };

        // Pre-compute ring vertex positions.
        TArray<TArray<FVector>> RingPts;
        RingPts.SetNum(Rings + 1);
        for (int32 r = 0; r <= Rings; r++)
        {
                float t = (float)r / (float)Rings;
                auto [sinP, radScale] = ProfileAt(t);

                RingPts[r].SetNum(Segments);
                for (int32 s = 0; s < Segments; s++)
                {
                        float Theta = 2.f * PI * (float)s / (float)Segments;
                        float X, Y;

                        if (const FVector2D* Snap = SeamSnap.Find(s))
                        {
                                // Seam column: at r=0 land exactly on the footprint vertex.
                                // For higher rings, scale the XY radially by the same radScale
                                // factor relative to the footprint radius at this angle.
                                float BaseR = Snap->Size();   // distance from centroid to fp vertex
                                float ScaledR = BaseR * radScale;
                                FVector2D Dir = BaseR > 0.f
                                        ? (*Snap / BaseR)
                                        : FVector2D(FMath::Cos(Theta), FMath::Sin(Theta));
                                X = Cen2D.X + Dir.X * ScaledR;
                                Y = Cen2D.Y + Dir.Y * ScaledR;
                        }
                        else
                        {
                                float R = PolygonRadius(Theta) * radScale;
                                X = Cen2D.X + FMath::Cos(Theta) * R;
                                Y = Cen2D.Y + FMath::Sin(Theta) * R;
                        }

                        RingPts[r][s] = FVector(X, Y, BaseZ + DomeH * sinP);
                }
        }

        const FVector Apex(Cen2D.X, Cen2D.Y, BaseZ + DomeH);

        // -----------------------------------------------------------------------
        // Smooth normal from the *actual* meridian slope of the profile.
        //
        // Along a meridian the surface point is ( R(t), Z(t) ) in the (radial,
        // vertical) plane, with R(t) = RadRef * radScale(t) and Z(t) = DomeH *
        // heightFrac(t).  The outward surface normal in that plane is the meridian
        // tangent (dR, dZ) rotated by -90°  →  (dZ, -dR).  In 3D this becomes
        //   N = ( dZ·cosθ, dZ·sinθ, -dR ).
        // Derivatives are taken by central finite difference of ProfileAt, so the
        // shading matches whatever silhouette the control points describe — this
        // correctly tilts the normal *downward* under the onion's belly (where the
        // radius grows with height) and steep near the spire.  For the plain
        // hemisphere it reduces to the usual cosΦ/sinΦ normal.
        // -----------------------------------------------------------------------
        const float RadRef = FMath::Max(FMath::Max(RadX, RadY), 1.f);

        auto ProfileNormal = [&](float Theta, float t) -> FVector
        {
                const float dt = 0.0015f;
                auto P0 = ProfileAt(FMath::Max(t - dt, 0.f));
                auto P1 = ProfileAt(FMath::Min(t + dt, 1.f));
                float dR = (P1.Value - P0.Value) * RadRef;   // radScale → radius
                float dZ = (P1.Key   - P0.Key)   * DomeH;    // heightFrac → height

                FVector Nrm(dZ * FMath::Cos(Theta), dZ * FMath::Sin(Theta), -dR);
                if (Nrm.IsNearlyZero()) Nrm = FVector(0, 0, 1);
                Nrm.Normalize();
                return Nrm;
        };

        // -----------------------------------------------------------------------
        // Build vertex buffer.
        //
        // We assign one logical "slot" per (ring, segment-column).  Non-seam
        // columns have a single slot shared by both flanking panels.  Seam
        // columns have TWO slots: one carrying the right-panel normal, one the
        // left-panel normal.  The slot index array ColIdx[s] gives the buffer
        // index of column s for the LEADING (left) side of panel s; the trailing
        // (right) side of panel s-1 may differ at a seam.
        //
        // Layout in OutData (starting at VBase):
        //   For each ring r (0..Rings):
        //     For each segment column s (0..Segments-1):
        //       Primary slot   (always present)         → used as LEFT  side of panel s
        //       Secondary slot (seam columns only)      → used as RIGHT side of panel s-1
        //   Apex slot at the end.
        //
        // We store the buffer offsets in two arrays:
        //   PrimarySlot[s]   = buffer index of primary   slot for column s, ring r=0
        //                      (add r*(total columns per ring) for ring r)
        //   SecondarySlot[s] = buffer index of secondary slot for column s, ring r=0
        //                      equals PrimarySlot[s] for non-seam columns (shared)
        // -----------------------------------------------------------------------

        // Count total columns per ring (seam columns contribute 2, others 1).
        int32 ColsPerRing = 0;
        TArray<int32> PrimaryOff;   PrimaryOff.SetNum(Segments);
        TArray<int32> SecondaryOff; SecondaryOff.SetNum(Segments);
        TArray<bool>  bSeam;        bSeam.SetNum(Segments);
        for (int32 s = 0; s < Segments; s++)
        {
                PrimaryOff[s]   = ColsPerRing;
                bSeam[s]        = IsSeamColumn(s);
                SecondaryOff[s] = ColsPerRing + (bSeam[s] ? 1 : 0);
                ColsPerRing    += bSeam[s] ? 2 : 1;
        }
        const int32 TotalRingVerts = ColsPerRing * (Rings + 1);

        int32 VBase    = OutData.Vertices.Num();
        int32 ApexSlot = VBase + TotalRingVerts;

        // Pre-allocate to avoid repeated reallocs.
        int32 TotalVerts = TotalRingVerts + 1;  // +1 apex
        OutData.Vertices.Reserve(   OutData.Vertices.Num()    + TotalVerts);
        OutData.Normals.Reserve(    OutData.Normals.Num()      + TotalVerts);
        OutData.VertexColors.Reserve(OutData.VertexColors.Num()+ TotalVerts);
        OutData.UV0.Reserve(        OutData.UV0.Num()          + TotalVerts);
        OutData.Tangents.Reserve(   OutData.Tangents.Num()     + TotalVerts);

        // Emit ring vertices.
        for (int32 r = 0; r <= Rings; r++)
        {
                float tRing  = (float)r / (float)Rings;

                for (int32 s = 0; s < Segments; s++)
                {
                        float Theta = 2.f * PI * (float)s / (float)Segments;
                        FVector  Pos = RingPts[r][s];
                        FVector  Nrm = ProfileNormal(Theta, tRing);
                        FVector  Tang(-FMath::Sin(Theta), FMath::Cos(Theta), 0.f);
                        FVector2D UV((float)s / (float)Segments, tRing);

                        // Primary slot — used as the LEFT edge of panel s.
                        OutData.Vertices.Add(Pos);
                        OutData.Normals.Add(Nrm);
                        OutData.VertexColors.Add(Color);
                        OutData.UV0.Add(UV);
                        OutData.Tangents.Add(FProcMeshTangent(Tang, false));

                        if (bSeam[s])
                        {
                                // Secondary slot — same position, but will get the
                                // average normal of the TWO panels meeting at this seam.
                                // We write the same smooth normal here; averaging is
                                // implicit because both flanking panels share this vertex
                                // only if they belong to the same face group — but since
                                // we duplicate we assign each side its own normal below.
                                //
                                // Right-side normal: the panel to the LEFT of this column
                                // (panel s-1) faces a slightly different direction.
                                int32 sPrev = (s + Segments - 1) % Segments;
                                float ThetaPrev = 2.f * PI * (float)sPrev / (float)Segments;
                                FVector NrmPrev = ProfileNormal(ThetaPrev, tRing);
                                // Use the average of the two flanking segment normals for
                                // the secondary slot so both sides of the crease angle match.
                                // The crease is created by the DISCONTINUITY in index, not
                                // by diverging normal directions — any difference in normal
                                // between primary and secondary produces the hard edge.
                                // Here we deliberately set secondary = NrmPrev (left panel's
                                // outward direction) so lighting breaks cleanly at the seam.
                                OutData.Vertices.Add(Pos);
                                OutData.Normals.Add(NrmPrev);
                                OutData.VertexColors.Add(Color);
                                OutData.UV0.Add(UV);
                                OutData.Tangents.Add(FProcMeshTangent(Tang, false));
                        }
                }
        }

        // Apex vertex — single shared point, straight-up normal.
        OutData.Vertices.Add(Apex);
        OutData.Normals.Add(FVector(0, 0, 1));
        OutData.VertexColors.Add(Color);
        OutData.UV0.Add(FVector2D(0.5f, 1.f));
        OutData.Tangents.Add(FProcMeshTangent(FVector(1, 0, 0), false));

        // -----------------------------------------------------------------------
        // Index helpers.
        // RingBase(r) = VBase + r * ColsPerRing
        // LeftIdx (r, s)  = RingBase(r) + PrimaryOff[s]   ← left  side of panel s
        // RightIdx(r, s)  = RingBase(r) + SecondaryOff[(s+1)%Segments]
        //                 which is the slot on the RIGHT side of panel s,
        //                 i.e. the secondary (or primary if non-seam) of column s+1.
        // -----------------------------------------------------------------------
        auto RingBase = [&](int32 r) -> int32
        {
                return VBase + r * ColsPerRing;
        };
        auto LeftIdx = [&](int32 r, int32 s) -> int32
        {
                return RingBase(r) + PrimaryOff[s];
        };
        // Right side of panel s = the slot on the LEFT wall of column (s+1),
        // seen from panel s's perspective.  If column (s+1) is a seam, the
        // secondary slot of (s+1) is used (it carries the normal from panel s's side).
        auto RightIdx = [&](int32 r, int32 s) -> int32
        {
                int32 sn = (s + 1) % Segments;
                // Secondary slot of sn is the slot that faces panel s (the panel to its left).
                return RingBase(r) + SecondaryOff[sn];
        };

        // Emit quad strips + apex triangles.
        for (int32 s = 0; s < Segments; s++)
        {
                for (int32 r = 0; r < Rings; r++)
                {
                        int32 BL = LeftIdx (r,     s);
                        int32 BR = RightIdx(r,     s);
                        int32 TL = LeftIdx (r + 1, s);
                        int32 TR = RightIdx(r + 1, s);

                        OutData.Triangles.Add(BL);
                        OutData.Triangles.Add(BR);
                        OutData.Triangles.Add(TL);

                        OutData.Triangles.Add(BR);
                        OutData.Triangles.Add(TR);
                        OutData.Triangles.Add(TL);
                }

                // Top triangle to apex.
                OutData.Triangles.Add(LeftIdx (Rings, s));
                OutData.Triangles.Add(RightIdx(Rings, s));
                OutData.Triangles.Add(ApexSlot);
        }
}

// ============================================================================
// AddRoundRoof - Barrel vault / semi-cylindrical roof
// Cross-section is an elliptical arch from one long edge to the other.
// RoofDirection (0-360): orientates the barrel axis.
// ============================================================================
void FGeoMeshGenerator::AddRoundRoof(
        FMeshData& OutData, const TArray<FVector>& FP,
        float BaseZ, float RoofH, const FColor& Color,
        float RoofDirection, const FString& RoofOrientation,
        const FColor& WallColor)
{
        if (FP.Num() < 3) return;

        TArray<FVector> CleanFP;
        for (const FVector& V : FP)
        {
                FVector P(V.X, V.Y, BaseZ);
                if (CleanFP.Num() > 0 && P.Equals(CleanFP.Last(), 0.5f)) continue;
                if (CleanFP.Num() > 0 && P.Equals(CleanFP[0], 0.5f)) continue;
                CleanFP.Add(P);
        }
        if (CleanFP.Num() < 3) return;

        float MinX, MaxX, MinY, MaxY;
        ComputeFootprintBounds(CleanFP, MinX, MaxX, MinY, MaxY);

        // Barrel axis direction (ridge runs along AxisDir; arch spans CrossDir).
        // The semicircular END CAPS face along ±AxisDir, so the arch is visible on
        // the walls perpendicular to AxisDir.
        FVector2D AxisDir;
        if (RoofDirection >= 0.f)
        {
                // RoofDirection = facing direction of slope → axis = perpendicular
                float D_rad = FMath::DegreesToRadians(RoofDirection);
                AxisDir = FVector2D(FMath::Cos(D_rad), -FMath::Sin(D_rad));
        }
        else
        {
                // Same convention as the gabled roof: "along" (default) runs the ridge
                // (barrel axis) along the LONG side; "across" along the SHORT side, so
                // the arch spans the perpendicular side. BarrelAxisDir also applies the
                // clockwise nudge so the barrel lines up with footprints rotated off the
                // world axes. Shared with ComputeBarrelRise so the rise matches.
                AxisDir = BarrelAxisDir(CleanFP, RoofOrientation);
        }
        FVector2D CrossDir(-AxisDir.Y, AxisDir.X); // perpendicular to barrel axis

        // Project footprint onto axis and cross directions
        float AxisMin = FLT_MAX, AxisMax = -FLT_MAX;
        float CrossMin = FLT_MAX, CrossMax = -FLT_MAX;
        for (const FVector& V : CleanFP)
        {
                float AP = V.X * AxisDir.X + V.Y * AxisDir.Y;
                float CP = V.X * CrossDir.X + V.Y * CrossDir.Y;
                AxisMin = FMath::Min(AxisMin, AP); AxisMax = FMath::Max(AxisMax, AP);
                CrossMin = FMath::Min(CrossMin, CP); CrossMax = FMath::Max(CrossMax, CP);
        }

        float CrossCenter = (CrossMin + CrossMax) * 0.5f;
        float Radius = (CrossMax - CrossMin) * 0.5f;
        if (Radius < 1.f) return;

        // Default RoofH to Radius (true semicircle) when none explicitly set
        if (RoofH < Radius * 0.1f)
                RoofH = Radius;

        const int32 CrossSteps = 36; // arch subdivisions (denser = smoother arch)
        const float StepLen = 150.f; // axis spacing in cm (more rings along the barrel)
        int32 AxisSteps = FMath::Max(1, FMath::RoundToInt((AxisMax - AxisMin) / StepLen));

        int32 GridBase = OutData.Vertices.Num();
        int32 Cols = CrossSteps + 1;

        // Build tessellated grid [AxisSteps+1][CrossSteps+1]
        for (int32 a = 0; a <= AxisSteps; a++)
        {
                float AxisPos = FMath::Lerp(AxisMin, AxisMax, (float)a / (float)AxisSteps);

                for (int32 c = 0; c <= CrossSteps; c++)
                {
                        float T = (float)c / (float)CrossSteps; // 0..1
                        float Angle = T * PI; // 0..PI
                        // Parametric semicircle: CrossProj moves CrossMin→CrossCenter→CrossMax
                        float CrossProj = CrossCenter - Radius * FMath::Cos(Angle);
                        float ZOffset = RoofH * FMath::Sin(Angle);

                        float WX = AxisPos * AxisDir.X + CrossProj * CrossDir.X;
                        float WY = AxisPos * AxisDir.Y + CrossProj * CrossDir.Y;
                        float WZ = BaseZ + ZOffset;

                        OutData.Vertices.Add(FVector(WX, WY, WZ));
                        OutData.Normals.Add(FVector(0, 0, 1)); // recomputed later
                        OutData.VertexColors.Add(Color);
                        OutData.Tangents.Add(FProcMeshTangent(FVector(AxisDir.X, AxisDir.Y, 0), false));
                        // Collapse UV to one texel: the round reads as a flat OSM colour
                        // (no tiled texture pattern), just tinted by the vertex colour.
                        OutData.UV0.Add(FVector2D(0.5f, 0.5f));
                }
        }

        // Curved barrel surface quads
        for (int32 a = 0; a < AxisSteps; a++)
        {
                for (int32 c = 0; c < CrossSteps; c++)
                {
                        int32 I00 = GridBase + a * Cols + c;
                        int32 I01 = GridBase + a * Cols + c + 1;
                        int32 I10 = GridBase + (a + 1) * Cols + c;
                        int32 I11 = GridBase + (a + 1) * Cols + c + 1;

                        OutData.Triangles.Add(I00); OutData.Triangles.Add(I10); OutData.Triangles.Add(I01);
                        OutData.Triangles.Add(I01); OutData.Triangles.Add(I10); OutData.Triangles.Add(I11);
                }
        }

        // Arched end caps (fan from bottom-center up through arch)
        for (int32 Pass = 0; Pass < 2; Pass++)
        {
                int32 a = (Pass == 0) ? 0 : AxisSteps;
                float AxisPos = (Pass == 0) ? AxisMin : AxisMax;
                float NX = AxisDir.X * (Pass == 0 ? -1.f : 1.f);
                float NY = AxisDir.Y * (Pass == 0 ? -1.f : 1.f);
                FVector EndNormal(NX, NY, 0.f);

                int32 CapBase = OutData.Vertices.Num();
                for (int32 c = 0; c <= CrossSteps; c++)
                {
                        int32 GVIdx = GridBase + a * Cols + c;
                        FVector CapVert = OutData.Vertices[GVIdx]; // copy before potential realloc
                        OutData.Vertices.Add(CapVert);
                        OutData.Normals.Add(EndNormal);
                        // End caps are the vertical semicircular GABLE faces — paint them
                        // with the facade colour so they read as a rounded WALL, not roof.
                        OutData.VertexColors.Add(WallColor);
                        OutData.Tangents.Add(FProcMeshTangent(FVector(CrossDir.X, CrossDir.Y, 0), false));
                        OutData.UV0.Add(FVector2D(0.5f, 0.5f)); // flat colour, no texture
                }
                // Bottom-center closing vertex (closes the D-shape)
                float BX = AxisPos * AxisDir.X + CrossCenter * CrossDir.X;
                float BY = AxisPos * AxisDir.Y + CrossCenter * CrossDir.Y;
                int32 CenterIdx = OutData.Vertices.Num();
                OutData.Vertices.Add(FVector(BX, BY, BaseZ));
                OutData.Normals.Add(EndNormal);
                OutData.VertexColors.Add(WallColor);
                OutData.Tangents.Add(FProcMeshTangent(FVector(CrossDir.X, CrossDir.Y, 0), false));
                OutData.UV0.Add(FVector2D(0.5f, 0.5f)); // flat colour, no texture

                for (int32 c = 0; c < CrossSteps; c++)
                {
                        if (Pass == 0)
                        {
                                // AxisMin cap: normal must point toward -AxisDir (outward).
                                // Cross product of (c-Center)×(c+1-Center) gives -AxisDir. ✓
                                OutData.Triangles.Add(CenterIdx);
                                OutData.Triangles.Add(CapBase + c);
                                OutData.Triangles.Add(CapBase + c + 1);
                        }
                        else
                        {
                                // AxisMax cap: normal must point toward +AxisDir (outward).
                                OutData.Triangles.Add(CenterIdx);
                                OutData.Triangles.Add(CapBase + c + 1);
                                OutData.Triangles.Add(CapBase + c);
                        }
                }
        }
}

// ============================================================================
// ============================================================================
// CGAL SKELETON-BASED ROOF GENERATORS
// These use CGAL Straight Skeleton for accurate roof geometry on arbitrary
// (non-rectangular) building footprints, following the streets.gl approach.
// ============================================================================
// ============================================================================

// ============================================================================
// Local helper: Compute roof height at a 2D point using skeleton vertex data
// Uses Inverse Distance Weighting (IDW) from skeleton vertices to interpolate
// heights. Boundary vertices have time=0 (height=BaseZ), interior vertices
// have time>0 (raised proportionally to RidgeHeight).
// ============================================================================
static float ComputeSkeletonHeight(
        const FVector2D& Point,
        const FCGALSkeletonResult& Skeleton,
        float BaseZ, float RidgeHeight)
{
        if (Skeleton.MaxTime < 0.01f || Skeleton.Vertices.Num() == 0)
                return BaseZ;

        // Check if point is very close to a skeleton vertex
        for (const FCGALSkeletonVertex& SV : Skeleton.Vertices)
        {
                if (FVector2D::Distance(Point, SV.Position) < 1.f)
                {
                        float NormalizedTime = SV.Time / Skeleton.MaxTime;
                        return BaseZ + RidgeHeight * NormalizedTime;
                }
        }

        // IDW interpolation from skeleton vertices
        float WeightSum = 0.f;
        float HeightWeightSum = 0.f;

        for (const FCGALSkeletonVertex& SV : Skeleton.Vertices)
        {
                float Dist = FVector2D::Distance(Point, SV.Position);
                float W = 1.f / FMath::Max(Dist * Dist, 1.f);
                float NormalizedTime = SV.Time / Skeleton.MaxTime;
                WeightSum += W;
                HeightWeightSum += W * (BaseZ + RidgeHeight * NormalizedTime);
        }

        return (WeightSum > 0.f) ? (HeightWeightSum / WeightSum) : BaseZ;
}

// ============================================================================
// Local helper: Check if a 2D point is on the polygon boundary
// ============================================================================
static bool IsOnBoundary(const FVector2D& Point, const TArray<FVector2D>& Polygon, float Tolerance = 5.f)
{
        for (const FVector2D& P : Polygon)
        {
                if (FVector2D::Distance(Point, P) < Tolerance)
                        return true;
        }
        return false;
}

// ============================================================================
// AddSkeletonHippedRoof
// Uses CGAL Straight Skeleton for accurate hipped roof generation.
//
// Algorithm:
// 1. Generate straight skeleton of the footprint
// 2. Triangulate the polygon using CGAL Constrained Delaunay Triangulation
// 3. For each vertex in the triangulation, compute roof height from skeleton:
//    - Boundary vertices: height = BaseZ (skeleton time = 0)
//    - Interior vertices: height = BaseZ + RidgeHeight * (time / MaxTime)
//    - Steiner points: IDW interpolation from nearby skeleton vertices
// 4. Build the mesh from triangulated vertices with computed heights
//
// This produces correct hipped roof geometry for ANY polygon shape,
// not just rectangles — concave, L-shaped, etc.
// ============================================================================
// ---------------------------------------------------------------------------
// Local geometry helpers for the skeleton-driven hipped roof.
// ---------------------------------------------------------------------------
static bool PointInPolygon2D(const FVector2D& P, const TArray<FVector2D>& Poly)
{
        const int32 N = Poly.Num();
        bool bIn = false;
        for (int32 i = 0, j = N - 1; i < N; j = i++)
        {
                const FVector2D& A = Poly[i];
                const FVector2D& B = Poly[j];
                if (((A.Y > P.Y) != (B.Y > P.Y)) &&
                        (P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X))
                        bIn = !bIn;
        }
        return bIn;
}

// Incircle predicate: true if D lies strictly inside the circumcircle of A,B,C.
static bool PointInCircumcircle2D(const FVector2D& A, const FVector2D& B,
        const FVector2D& C, const FVector2D& D)
{
        double ax = A.X, ay = A.Y, bx = B.X, by = B.Y, cx = C.X, cy = C.Y;
        const double Orient = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
        if (Orient < 0.0) { Swap(bx, cx); Swap(by, cy); } // force CCW
        const double adx = ax - D.X, ady = ay - D.Y;
        const double bdx = bx - D.X, bdy = by - D.Y;
        const double cdx = cx - D.X, cdy = cy - D.Y;
        const double AbD = adx * bdy - bdx * ady;
        const double BcD = bdx * cdy - cdx * bdy;
        const double CaD = cdx * ady - adx * cdy;
        const double AA = adx * adx + ady * ady;
        const double BB = bdx * bdx + bdy * bdy;
        const double CC = cdx * cdx + cdy * cdy;
        return (AA * BcD + BB * CaD + CC * AbD) > 0.0;
}

// Bowyer-Watson Delaunay triangulation of a 2D point set. Output: flat index
// triples into Pts. Self-contained (no CGAL) so it runs in the HiroGeo module.
static void DelaunayTriangulate2D(const TArray<FVector2D>& Pts, TArray<int32>& OutTris)
{
        OutTris.Reset();
        const int32 N = Pts.Num();
        if (N < 3) return;

        float MinX = Pts[0].X, MaxX = Pts[0].X, MinY = Pts[0].Y, MaxY = Pts[0].Y;
        for (const FVector2D& P : Pts)
        {
                MinX = FMath::Min(MinX, P.X); MaxX = FMath::Max(MaxX, P.X);
                MinY = FMath::Min(MinY, P.Y); MaxY = FMath::Max(MaxY, P.Y);
        }
        const float DMax = FMath::Max(FMath::Max(MaxX - MinX, MaxY - MinY), 1.f) * 100.f;
        const float MidX = 0.5f * (MinX + MaxX);
        const float MidY = 0.5f * (MinY + MaxY);

        TArray<FVector2D> V = Pts; // working list; super-triangle verts appended
        const int32 S0 = V.Add(FVector2D(MidX - 20.f * DMax, MidY - DMax));
        const int32 S1 = V.Add(FVector2D(MidX, MidY + 20.f * DMax));
        const int32 S2 = V.Add(FVector2D(MidX + 20.f * DMax, MidY - DMax));

        struct FDTri { int32 A, B, C; };
        TArray<FDTri> Tris;
        Tris.Add(FDTri{ S0, S1, S2 });

        for (int32 ip = 0; ip < N; ip++)
        {
                const FVector2D& P = V[ip];

                TArray<TPair<int32, int32>> Edges;
                auto AddEdge = [&Edges](int32 a, int32 b)
                {
                        for (int32 e = 0; e < Edges.Num(); e++)
                        {
                                if ((Edges[e].Key == a && Edges[e].Value == b) ||
                                        (Edges[e].Key == b && Edges[e].Value == a))
                                {
                                        Edges.RemoveAtSwap(e); // shared edge -> interior, drop
                                        return;
                                }
                        }
                        Edges.Add(TPair<int32, int32>(a, b));
                };

                for (int32 ti = Tris.Num() - 1; ti >= 0; ti--)
                {
                        const FDTri& T = Tris[ti];
                        if (PointInCircumcircle2D(V[T.A], V[T.B], V[T.C], P))
                        {
                                AddEdge(T.A, T.B);
                                AddEdge(T.B, T.C);
                                AddEdge(T.C, T.A);
                                Tris.RemoveAtSwap(ti);
                        }
                }

                for (const TPair<int32, int32>& E : Edges)
                        Tris.Add(FDTri{ E.Key, E.Value, ip });
        }

        for (const FDTri& T : Tris)
        {
                if (T.A >= N || T.B >= N || T.C >= N) continue; // drop super-triangle fans
                OutTris.Add(T.A); OutTris.Add(T.B); OutTris.Add(T.C);
        }
}

// ============================================================================
// AddStraightSkeletonRoof
// Build the roof directly from the straight-skeleton FACES. CGAL gives one
// face per footprint edge: the sloped panel rising from that edge up to the
// ridge. We lift each face vertex by its skeleton time (constant pitch,
// normalised so the deepest ridge point reaches RidgeHeight) and triangulate
// each face. Because the faces tile the footprint exactly, the result is
// watertight with NO missing triangles and NO sideways slivers — for any
// polygon shape (rectangle, L, U, cross, with courtyards).
// ============================================================================
bool FGeoMeshGenerator::AddStraightSkeletonRoof(
        FMeshData& OutRoofData,
        const TArray<FVector>& Footprint,
        float BaseZ,
        float RidgeHeight,
        const FColor& Color,
        const TArray<TArray<FVector2D>>& Holes)
{
        if (Footprint.Num() < 3) return false;

        try
        {
        // Clean footprint (CCW)
        TArray<FVector> CleanFP;
        for (const FVector& V : Footprint)
        {
                FVector P(V.X, V.Y, BaseZ);
                if (CleanFP.Num() > 0 && P.Equals(CleanFP.Last(), 0.5f)) continue;
                if (CleanFP.Num() > 0 && P.Equals(CleanFP[0], 0.5f)) continue;
                CleanFP.Add(P);
        }
        if (CleanFP.Num() < 3) return false;
        if (ComputeSignedArea2D(CleanFP) < 0.f) Algo::Reverse(CleanFP);

        TArray<FVector2D> Poly2D;
        for (const FVector& V : CleanFP) Poly2D.Add(FVector2D(V.X, V.Y));

        FCGALSkeletonResult Skel = FCGALSkeletonGenerator::GenerateSkeleton(Poly2D, Holes);
        if (!Skel.IsValid() || Skel.Faces.Num() == 0) return false;

        const float MaxT = FMath::Max(Skel.MaxTime, 1.f);
        auto ZForTime = [&](float T) -> float
        {
                return BaseZ + RidgeHeight * FMath::Clamp(T / MaxT, 0.f, 1.f);
        };

        // Boundary (eave) positions of the footprint, used to force any
        // skeleton vertex that lies ON the building outline to sit exactly
        // at BaseZ. Without this, tiny CGAL Time>0 values on outline
        // vertices (especially at corners) cause the roof edges/corners to
        // "curl up" like bird wings instead of resting flat on the walls.
        TArray<FVector2D> BoundaryPts2D;
        for (const FVector& V : CleanFP) BoundaryPts2D.Add(FVector2D(V.X, V.Y));

        auto IsOnBoundary = [&](const FVector2D& P) -> bool
        {
                const int32 N = BoundaryPts2D.Num();
                for (int32 i = 0; i < N; i++)
                {
                        const FVector2D& A = BoundaryPts2D[i];
                        const FVector2D& B = BoundaryPts2D[(i + 1) % N];
                        const FVector2D AB = B - A;
                        const float LenSq = AB.SizeSquared();
                        if (LenSq < KINDA_SMALL_NUMBER)
                        {
                                if (FVector2D::DistSquared(P, A) < 1.f) return true;
                                continue;
                        }
                        const float T = FVector2D::DotProduct(P - A, AB) / LenSq;
                        const float Tc = FMath::Clamp(T, 0.f, 1.f);
                        const FVector2D Closest = A + AB * Tc;
                        if (FVector2D::DistSquared(P, Closest) < 1.f) return true;
                }
                return false;
        };

        int32 Emitted = 0;
        for (const FCGALSkeletonFace& Face : Skel.Faces)
        {
                const int32 NF = Face.VertexIndices.Num();
                if (NF < 3) continue;

                // Collect the face boundary (2D + per-vertex roof height).
                TArray<FVector2D> FP2D;
                TArray<float> FZ;
                FP2D.Reserve(NF); FZ.Reserve(NF);
                for (int32 Idx : Face.VertexIndices)
                {
                        if (Idx < 0 || Idx >= Skel.Vertices.Num()) continue;
                        const FCGALSkeletonVertex& SV = Skel.Vertices[Idx];
                        // Drop consecutive duplicates that would break ear-clipping.
                        if (FP2D.Num() > 0 && FVector2D::DistSquared(FP2D.Last(), SV.Position) < 1.f) continue;
                        FP2D.Add(SV.Position);
                        FZ.Add(IsOnBoundary(SV.Position) ? BaseZ : ZForTime(SV.Time));
                }
                if (FP2D.Num() > 1 && FVector2D::DistSquared(FP2D[0], FP2D.Last()) < 1.f)
                {
                        FP2D.Pop(); FZ.Pop();
                }
                if (FP2D.Num() < 3) continue;

                // Triangulate the face. Ear-clipping handles the (usually convex,
                // occasionally reflex) skeleton panels; fan is the safety net.
                TArray<int32> Tris;
                TriangulatePolygon2D(FP2D, Tris);
                if (Tris.Num() < 3)
                {
                        Tris.Reset();
                        for (int32 k = 1; k + 1 < FP2D.Num(); k++)
                        {
                                Tris.Add(0); Tris.Add(k); Tris.Add(k + 1);
                        }
                }

                for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
                {
                        const int32 a = Tris[t], b = Tris[t + 1], c = Tris[t + 2];
                        if (a == b || b == c || a == c) continue;
                        FVector V0(FP2D[a].X, FP2D[a].Y, FZ[a]);
                        FVector V1(FP2D[b].X, FP2D[b].Y, FZ[b]);
                        FVector V2(FP2D[c].X, FP2D[c].Y, FZ[c]);
                        // Force the panel to face upward (consistent lighting).
                        if (FVector::CrossProduct(V1 - V0, V2 - V0).Z < 0.f)
                                Swap(V1, V2);
                        AddTriangle(OutRoofData, V0, V1, V2, Color);
                        Emitted++;
                }
        }

        return Emitted > 0;
        }
        catch (...)
        {
                UE_LOG(LogTemp, Warning, TEXT("AddStraightSkeletonRoof: exception, caller will fall back"));
                return false;
        }
}

void FGeoMeshGenerator::AddSkeletonHippedRoof(
        FMeshData& OutRoofData,
        const TArray<FVector>& Footprint,
        float BaseZ,
        float RidgeHeight,
        const FColor& Color,
        const TArray<TArray<FVector2D>>& Holes)
{
        if (Footprint.Num() < 3) return;

        try
        {
        // Clean footprint (CCW)
        TArray<FVector> CleanFP;
        for (const FVector& V : Footprint)
        {
                FVector P(V.X, V.Y, BaseZ);
                if (CleanFP.Num() > 0 && P.Equals(CleanFP.Last(), 0.5f)) continue;
                if (CleanFP.Num() > 0 && P.Equals(CleanFP[0], 0.5f)) continue;
                CleanFP.Add(P);
        }
        if (CleanFP.Num() < 3) return;
        if (ComputeSignedArea2D(CleanFP) < 0.f) Algo::Reverse(CleanFP);

        // ------------------------------------------------------------------
        // HOLES branch: keep the CGAL CDT cap (flat surface that excludes the
        // inner courtyards). Courtyard buildings are rare; a flat-but-correct
        // hole exclusion is acceptable here.
        // ------------------------------------------------------------------
        if (Holes.Num() > 0)
        {
                TArray<FVector2D> Poly2D;
                for (const FVector& V : CleanFP) Poly2D.Add(FVector2D(V.X, V.Y));

                TArray<int32> Triangles;
                TArray<FVector2D> SteinerPoints;
                // First try a genuinely SLOPED roof from the straight skeleton WITH holes:
                // interior skeleton nodes carry Time (height); triangulate them and keep
                // triangles inside the outer ring AND outside every courtyard.
                {
                        FCGALSkeletonResult HoleSkel = FCGALSkeletonGenerator::GenerateSkeleton(Poly2D, Holes);
                        if (HoleSkel.IsValid() && HoleSkel.Vertices.Num() >= 3)
                        {
                                float hMinX = Poly2D[0].X, hMaxX = Poly2D[0].X, hMinY = Poly2D[0].Y, hMaxY = Poly2D[0].Y;
                                for (const FVector2D& P : Poly2D)
                                {
                                        hMinX = FMath::Min(hMinX, P.X); hMaxX = FMath::Max(hMaxX, P.X);
                                        hMinY = FMath::Min(hMinY, P.Y); hMaxY = FMath::Max(hMaxY, P.Y);
                                }
                                const float RidgeRun = FMath::Max(0.5f * FMath::Min(hMaxX - hMinX, hMaxY - hMinY), 1.f);
                                TArray<FVector2D> HPts; TArray<float> HZs;
                                for (const FCGALSkeletonVertex& SV : HoleSkel.Vertices)
                                {
                                        bool bDup = false;
                                        for (const FVector2D& Q : HPts)
                                                if (FVector2D::DistSquared(Q, SV.Position) < 1.f) { bDup = true; break; }
                                        if (bDup) continue;
                                        HPts.Add(SV.Position);
                                        HZs.Add(BaseZ + RidgeHeight * FMath::Min(1.f, SV.Time / RidgeRun));
                                }
                                TArray<int32> HTris;
                                DelaunayTriangulate2D(HPts, HTris);
                                int32 HEmitted = 0;
                                for (int32 t = 0; t + 2 < HTris.Num(); t += 3)
                                {
                                        const int32 i0 = HTris[t], i1 = HTris[t + 1], i2 = HTris[t + 2];
                                        const FVector2D& P0 = HPts[i0];
                                        const FVector2D& P1 = HPts[i1];
                                        const FVector2D& P2 = HPts[i2];
                                        const FVector2D Cen = (P0 + P1 + P2) / 3.f;
                                        // Reject triangles that bridge the outer concavity or a courtyard. Test
                                        // the centroid AND each edge midpoint (nudged 2% inward) so spanning
                                        // slivers are dropped while legitimate boundary triangles survive.
                                        auto SampleOK = [&](const FVector2D& Q) -> bool
                                        {
                                                if (!PointInPolygon2D(Q, Poly2D)) return false;
                                                for (const TArray<FVector2D>& H : Holes)
                                                        if (PointInPolygon2D(Q, H)) return false;
                                                return true;
                                        };
                                        auto EdgeMidOK = [&](const FVector2D& A, const FVector2D& B) -> bool
                                        {
                                                FVector2D M = (A + B) * 0.5f;
                                                M += (Cen - M) * 0.02f;
                                                return SampleOK(M);
                                        };
                                        if (!SampleOK(Cen)) continue;
                                        if (!EdgeMidOK(P0, P1) || !EdgeMidOK(P1, P2) || !EdgeMidOK(P2, P0)) continue;
                                        AddTriangle(OutRoofData,
                                                FVector(HPts[i0].X, HPts[i0].Y, HZs[i0]),
                                                FVector(HPts[i1].X, HPts[i1].Y, HZs[i1]),
                                                FVector(HPts[i2].X, HPts[i2].Y, HZs[i2]), Color);
                                        HEmitted++;
                                }
                                if (HEmitted > 0) return;
                        }
                }

                // Fallback: flat CDT cap that excludes the courtyards (rare).
                FCGALSkeletonGenerator::TriangulateWithCGAL(Poly2D, Holes, Triangles, SteinerPoints);
                if (Triangles.Num() < 3) { AddPolygonTriangulated(OutRoofData, Footprint, BaseZ, Color); return; }

                TArray<FVector2D> AllPoints; AllPoints.Append(Poly2D); AllPoints.Append(SteinerPoints);
                int32 BaseIdx = OutRoofData.Vertices.Num();
                for (const FVector2D& Pt : AllPoints)
                {
                        OutRoofData.Vertices.Add(FVector(Pt.X, Pt.Y, BaseZ));
                        OutRoofData.Normals.Add(FVector(0.f, 0.f, 1.f));
                        OutRoofData.VertexColors.Add(Color);
                        OutRoofData.Tangents.Add(FProcMeshTangent(FVector(1, 0, 0), false));
                        OutRoofData.UV0.Add(FVector2D(Pt.X / 100.f, Pt.Y / 100.f));
                }
                for (int32 TriIdx : Triangles) OutRoofData.Triangles.Add(BaseIdx + TriIdx);
                return;
        }

        // ------------------------------------------------------------------
        // SOLID footprint: build a genuinely SLOPED hipped roof from concentric
        // inset rings raised toward the spine. This is robust for ANY polygon
        // (rect, L, U, ...) and — unlike a bare CDT — actually produces interior
        // raised geometry, so the roof is visibly pitched (the previous CDT path
        // left every vertex on the boundary at BaseZ, i.e. a flat roof).
        // ------------------------------------------------------------------
        float MinX, MaxX, MinY, MaxY;
        ComputeFootprintBounds(CleanFP, MinX, MaxX, MinY, MaxY);
        const float HalfShort = 0.5f * FMath::Min(MaxX - MinX, MaxY - MinY);
        if (HalfShort < 1.f || RidgeHeight < 1.f)
        {
                AddPolygonTriangulated(OutRoofData, CleanFP, BaseZ, Color);
                return;
        }

        const int32 NumRings = 6; (void)NumRings;

                // ------------------------------------------------------------------
                // Skeleton-driven hipped/pyramidal roof.
                // Each straight-skeleton node carries Time = distance from the eave;
                // lift it to BaseZ + RidgeHeight*(Time/MaxTime). A Delaunay triangulation
                // of all skeleton nodes (boundary Time=0 + interior ridge nodes), filtered
                // to triangles whose centroid is inside the footprint, gives a true sloped
                // roof for ANY polygon (rect, L, U, concave) with no polygon offsetting.
                // ------------------------------------------------------------------
                {
                        TArray<FVector2D> Poly2D;
                        for (const FVector& V : CleanFP) Poly2D.Add(FVector2D(V.X, V.Y));

                        FCGALSkeletonResult Skel = FCGALSkeletonGenerator::GenerateSkeleton(Poly2D);
                        if (Skel.IsValid() && Skel.Vertices.Num() >= 3)
                        {
                                // CONSTANT-PITCH height: rise uniformly from every eave and flatten
                                // into a level ridge once the roof reaches RidgeHeight. Using the
                                // short-side half-width as the horizontal run keeps the slope even,
                                // which avoids the jagged multi-peak look that Time/MaxTime produced.
                                const float RidgeRun = FMath::Max(HalfShort, 1.f);
                                TArray<FVector2D> Pts; TArray<float> Zs;
                                Pts.Reserve(Skel.Vertices.Num()); Zs.Reserve(Skel.Vertices.Num());

                                // Snap any skeleton vertex lying on the footprint outline to
                                // BaseZ exactly. CGAL's exact-kernel Time for outline points
                                // (especially corners) is often a tiny epsilon > 0 instead
                                // of 0, which otherwise lifts that single point and produces
                                // a sharp "bird wing" flap at the corner.
                                auto IsOnOutline = [&](const FVector2D& P) -> bool
                                {
                                        const int32 N = Poly2D.Num();
                                        for (int32 i = 0; i < N; i++)
                                        {
                                                const FVector2D& A = Poly2D[i];
                                                const FVector2D& B = Poly2D[(i + 1) % N];
                                                const FVector2D AB = B - A;
                                                const float LenSq = AB.SizeSquared();
                                                if (LenSq < KINDA_SMALL_NUMBER)
                                                {
                                                        if (FVector2D::DistSquared(P, A) < 1.f) return true;
                                                        continue;
                                                }
                                                const float T = FVector2D::DotProduct(P - A, AB) / LenSq;
                                                const float Tc = FMath::Clamp(T, 0.f, 1.f);
                                                const FVector2D Closest = A + AB * Tc;
                                                if (FVector2D::DistSquared(P, Closest) < 1.f) return true;
                                        }
                                        return false;
                                };

                                for (const FCGALSkeletonVertex& SV : Skel.Vertices)
                                {
                                        bool bDup = false;
                                        for (const FVector2D& Q : Pts)
                                                if (FVector2D::DistSquared(Q, SV.Position) < 1.f) { bDup = true; break; }
                                        if (bDup) continue;
                                        Pts.Add(SV.Position);
                                        Zs.Add(IsOnOutline(SV.Position) ? BaseZ : BaseZ + RidgeHeight * FMath::Min(1.f, SV.Time / RidgeRun));
                                }

                                TArray<int32> Tris;
                                DelaunayTriangulate2D(Pts, Tris);

                                int32 Emitted = 0;
                                for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
                                {
                                        const int32 i0 = Tris[t], i1 = Tris[t + 1], i2 = Tris[t + 2];
                                        const FVector2D& P0 = Pts[i0];
                                        const FVector2D& P1 = Pts[i1];
                                        const FVector2D& P2 = Pts[i2];
                                        const FVector2D Cen = (P0 + P1 + P2) / 3.f;
                                        // Reject triangles that bridge a concave notch. Centroid-only is not
                                        // enough: a thin spanning sliver can have its centroid just inside the
                                        // footprint, producing the diagonal flap / needle-spike artefacts. Also
                                        // require each EDGE midpoint (nudged 2% toward the centroid so legitimate
                                        // boundary edges, whose midpoints lie ON the outline, still pass) inside.
                                        auto EdgeMidInside = [&](const FVector2D& A, const FVector2D& B) -> bool
                                        {
                                                FVector2D M = (A + B) * 0.5f;
                                                M += (Cen - M) * 0.02f;
                                                return PointInPolygon2D(M, Poly2D);
                                        };
                                        if (!PointInPolygon2D(Cen, Poly2D)) continue; // skip concavity spans
                                        if (!EdgeMidInside(P0, P1) || !EdgeMidInside(P1, P2) || !EdgeMidInside(P2, P0)) continue;
                                        AddTriangle(OutRoofData,
                                                FVector(P0.X, P0.Y, Zs[i0]),
                                                FVector(P1.X, P1.Y, Zs[i1]),
                                                FVector(P2.X, P2.Y, Zs[i2]), Color);
                                        Emitted++;
                                }
                                if (Emitted > 0) return;
                        }

                        // Skeleton unavailable/degenerate -> pyramidal CGAL fallback.
                        AddPyramidalRoof(OutRoofData, Footprint, BaseZ, RidgeHeight, Color);
                        return;
                }
        #if 0 // legacy inset-ring hip path, superseded by skeleton triangulation
                const float MaxInset = HalfShort * 0.92f; // nearly collapse to the spine

        TArray<FVector> PrevRing = CleanFP;       // outer ring at eave height
        for (FVector& V : PrevRing) V.Z = BaseZ;

        for (int32 k = 1; k <= NumRings; k++)
        {
                const float T = (float)k / (float)NumRings;
                const float Z = BaseZ + RidgeHeight * T;
                TArray<FVector> Ring = InsetPolygonByEdgeOffset(CleanFP, T * MaxInset);
                for (FVector& V : Ring) V.Z = Z;

                // If the inset ring collapsed or changed vertex count (happens on
                // complex/concave footprints), bridge with a centroid fan instead
                // of mismatched quads that produce twisted "taco" artefacts.
                if (Ring.Num() < 3)
                {
                        FVector Apex = ComputePolygonCentroid(PrevRing);
                        Apex.Z = BaseZ + RidgeHeight;
                        const int32 PN = PrevRing.Num();
                        for (int32 i = 0; i < PN; i++)
                        {
                                const int32 ni = (i + 1) % PN;
                                if (FVector2D::Distance(FVector2D(PrevRing[i].X, PrevRing[i].Y),
                                        FVector2D(PrevRing[ni].X, PrevRing[ni].Y)) < 1.f) continue;
                                AddTriangle(OutRoofData, PrevRing[i], PrevRing[ni], Apex, Color);
                        }
                        PrevRing.Empty();
                        break;
                }

                if (PrevRing.Num() != Ring.Num())
                {
                        // Vertex count changed — fan from centroid to avoid cross-ring twisting
                        FVector MidCen = ComputePolygonCentroid(PrevRing);
                        MidCen.Z = Z;
                        const int32 PN = PrevRing.Num();
                        for (int32 i = 0; i < PN; i++)
                        {
                                const int32 ni = (i + 1) % PN;
                                AddTriangle(OutRoofData, PrevRing[i], PrevRing[ni], MidCen, Color);
                        }
                        PrevRing = MoveTemp(Ring);
                        continue;
                }

                const int32 N = PrevRing.Num();
                for (int32 i = 0; i < N; i++)
                {
                        const int32 ni = (i + 1) % N;
                        // Sloped band quad: outer-lower edge up to inner-higher edge.
                        AddQuad(OutRoofData, PrevRing[i], PrevRing[ni], Ring[ni], Ring[i], Color);
                }
                PrevRing = MoveTemp(Ring);
        }

        // Close the (small) top ring with a flat cap at ridge height.
        if (PrevRing.Num() >= 3)
                AddPolygonTriangulated(OutRoofData, PrevRing, PrevRing[0].Z, Color, true);
#endif
        }
        catch (...)
        {
                UE_LOG(LogTemp, Warning, TEXT("CGAL Skeleton Hipped Roof: Exception caught, using flat fallback"));
                AddPolygonTriangulated(OutRoofData, Footprint, BaseZ, Color);
        }
}

// ============================================================================
// AddSkeletonGabledRoof
// Uses CGAL Straight Skeleton for ridge detection + CGAL CDT for
// robust triangulation.
//
// Algorithm:
// 1. Generate straight skeleton to find the ridge line
// 2. The ridge is the longest bisector edge in the skeleton
// 3. Split polygon along the ridge line
// 4. For each sub-polygon, triangulate with CGAL CDT
// 5. Assign heights: boundary vertices near ridge get raised to ridge height,
//    others stay at BaseZ. This creates the gable walls.
// 6. If skeleton/split fails, fall back to legacy AABB approach
// ============================================================================
void FGeoMeshGenerator::AddSkeletonGabledRoof(
        FMeshData& OutRoofData,
        const TArray<FVector>& Footprint,
        float BaseZ,
        float RidgeHeight,
        const FColor& Color,
        const TArray<TArray<FVector2D>>& Holes,
        float RoofDirection,
        const FString& RoofOrientation)
{
        if (Footprint.Num() < 3) return;

        // A gabled roof around an inner courtyard is not well-defined by the
        // single-ridge split; the straight-skeleton (hipped) roof handles a
        // polygon-with-holes correctly, so defer to it when holes are present.
        if (Holes.Num() > 0)
        {
                AddSkeletonHippedRoof(OutRoofData, Footprint, BaseZ, RidgeHeight, Color, Holes);
                return;
        }

        // The single-ridge split only behaves on SIMPLE CONVEX footprints
        // (rectangular houses). For concave / complex footprints (L-shapes, a
        // basilica nave, ...) it produces broken geometry, so use the robust
        // straight-skeleton roof, which handles any polygon shape cleanly.
        {
                auto IsConvexXY = [](const TArray<FVector>& P) -> bool
                {
                        const int32 N = P.Num();
                        if (N < 4) return true;
                        int32 Sign = 0;
                        for (int32 i = 0; i < N; i++)
                        {
                                const FVector& A = P[i];
                                const FVector& B = P[(i + 1) % N];
                                const FVector& C = P[(i + 2) % N];
                                const float Cross = (B.X - A.X) * (C.Y - B.Y) - (B.Y - A.Y) * (C.X - B.X);
                                if (FMath::Abs(Cross) < 1.f) continue;
                                const int32 S = (Cross > 0.f) ? 1 : -1;
                                if (Sign == 0) Sign = S;
                                else if (S != Sign) return false;
                        }
                        return true;
                };

                if (!IsConvexXY(Footprint))
                {
                        AddSkeletonHippedRoof(OutRoofData, Footprint, BaseZ, RidgeHeight, Color);
                        return;
                }
        }

        try
        {
        // Clean footprint
        TArray<FVector> CleanFP;
        for (const FVector& V : Footprint)
        {
                FVector P(V.X, V.Y, BaseZ);
                if (CleanFP.Num() > 0 && P.Equals(CleanFP.Last(), 0.5f)) continue;
                if (CleanFP.Num() > 0 && P.Equals(CleanFP[0], 0.5f)) continue;
                CleanFP.Add(P);
        }
        if (CleanFP.Num() < 3) return;
        if (ComputeSignedArea2D(CleanFP) < 0.f) Algo::Reverse(CleanFP);

        // Convert to 2D
        TArray<FVector2D> Poly2D;
        for (const FVector& V : CleanFP)
                Poly2D.Add(FVector2D(V.X, V.Y));

        // Find the ridge line: explicit direction > skeleton detection > AABB fallback
        FVector2D RidgeA(0.f, 0.f), RidgeB(0.f, 0.f);
        float RidgeLen = 0.f;
        bool bFoundRidge = false;

        if (RoofDirection >= 0.f)
        {
                // roof:direction is the bearing the slope faces → ridge runs perpendicular
                // RidgeDir = (cos(D), -sin(D)) in X-East / Y-North coord system
                float D_rad = FMath::DegreesToRadians(RoofDirection);
                FVector2D DirectRidgeDir(FMath::Cos(D_rad), -FMath::Sin(D_rad));

                float MinX, MaxX, MinY, MaxY;
                ComputeFootprintBounds(CleanFP, MinX, MaxX, MinY, MaxY);
                float CX = (MinX + MaxX) * 0.5f, CY = (MinY + MaxY) * 0.5f;
                float Extent = FMath::Max(MaxX - MinX, MaxY - MinY) * 1.2f;

                RidgeA = FVector2D(CX, CY) - DirectRidgeDir * Extent;
                RidgeB = FVector2D(CX, CY) + DirectRidgeDir * Extent;
                RidgeLen = FVector2D::Distance(RidgeA, RidgeB);
                bFoundRidge = true;
        }
        else
        {
                // Try to find ridge line from CGAL straight skeleton
                FCGALSkeletonResult Skeleton = FCGALSkeletonGenerator::GenerateSkeleton(Poly2D);

                if (Skeleton.IsValid())
                {
                        for (const FCGALSkeletonEdge& Edge : Skeleton.Edges)
                        {
                                if (!Edge.bIsBisector) continue;
                                if (Edge.VertexA < 0 || Edge.VertexA >= Skeleton.Vertices.Num()) continue;
                                if (Edge.VertexB < 0 || Edge.VertexB >= Skeleton.Vertices.Num()) continue;

                                const FCGALSkeletonVertex& VA = Skeleton.Vertices[Edge.VertexA];
                                const FCGALSkeletonVertex& VB = Skeleton.Vertices[Edge.VertexB];

                                // Only interior-to-interior edges form the ridge
                                if (VA.Time > 0.01f && VB.Time > 0.01f)
                                {
                                        float Len = FVector2D::Distance(VA.Position, VB.Position);
                                        if (Len > RidgeLen)
                                        {
                                                RidgeLen = Len;
                                                RidgeA = VA.Position;
                                                RidgeB = VB.Position;
                                                bFoundRidge = true;
                                        }
                                }
                        }
                }
        }

        // AABB fallback (also handles roof:orientation "along"/"across")
        if (!bFoundRidge)
        {
                float MinX, MaxX, MinY, MaxY;
                ComputeFootprintBounds(CleanFP, MinX, MaxX, MinY, MaxY);
                float CX = (MinX + MaxX) * 0.5f, CY = (MinY + MaxY) * 0.5f;

                bool bLongX = IsLongerAlongX(CleanFP);
                // "across": ridge perpendicular to long axis; "along" (default): along long axis
                bool bAcross = (RoofOrientation.ToLower() == TEXT("across"));

                if (bLongX != bAcross)
                {
                        RidgeA = FVector2D(MinX, CY);
                        RidgeB = FVector2D(MaxX, CY);
                }
                else
                {
                        RidgeA = FVector2D(CX, MinY);
                        RidgeB = FVector2D(CX, MaxY);
                }
        }

        // Compute max distance from ridge line
        float MaxDist = 0.f;
        for (const FVector& V : CleanFP)
        {
                float D = FMath::Abs(SignedDistanceToLine(FVector2D(V.X, V.Y), RidgeA, RidgeB));
                if (D > MaxDist) MaxDist = D;
        }
        if (MaxDist < 1.f) MaxDist = 1.f;

        // Split polygon along ridge line
        FVector2D RidgeDir = RidgeB - RidgeA;
        if (!RidgeDir.IsNearlyZero()) RidgeDir.Normalize();
        FVector2D RidgeMid = (RidgeA + RidgeB) * 0.5f;

        TArray<TArray<FVector2D>> SubPolygons;
        bool bSplitOK = SplitPolygon2D(Poly2D, RidgeMid, RidgeDir, SubPolygons);

        if (!bSplitOK || SubPolygons.Num() < 2)
        {
                // Fallback: Insert the two ridge-boundary intersection points (where the ridge
                // LINE meets the footprint boundary) into Poly2D so CGAL CDT produces vertices
                // at the gable apexes. Without them, every footprint vertex is at MaxDist from
                // the ridge → NormalizedDist=1 → Z=BaseZ for all → flat roof instead of gabled.
                TArray<FVector2D> AugPoly = Poly2D;
                {
                        // Find the (at most 2) edges that straddle the ridge line and compute
                        // their intersection points, inserting them in reverse-index order so
                        // earlier indices stay valid.
                        struct FRidgePt { FVector2D Pt; int32 AfterIdx; };
                        TArray<FRidgePt> RidgePts;
                        for (int32 i = 0; i < Poly2D.Num(); i++)
                        {
                                int32 Next = (i + 1) % Poly2D.Num();
                                float DA = SignedDistanceToLine(Poly2D[i], RidgeA, RidgeB);
                                float DB = SignedDistanceToLine(Poly2D[Next], RidgeA, RidgeB);
                                if (DA * DB < 0.f)
                                {
                                        float T = DA / (DA - DB);
                                        FVector2D P = Poly2D[i] + T * (Poly2D[Next] - Poly2D[i]);
                                        RidgePts.Add({ P, i });
                                }
                        }
                        RidgePts.Sort([](const FRidgePt& A, const FRidgePt& B) { return A.AfterIdx > B.AfterIdx; });
                        for (const FRidgePt& RP : RidgePts)
                                AugPoly.Insert(RP.Pt, RP.AfterIdx + 1);
                }

                TArray<int32> Triangles;
                TArray<FVector2D> SteinerPoints;
                FCGALSkeletonGenerator::TriangulateWithCGAL(AugPoly, Triangles, SteinerPoints);

                if (Triangles.Num() < 3)
                {
                        AddSkeletonHippedRoof(OutRoofData, Footprint, BaseZ, RidgeHeight, Color);
                        return;
                }

                TArray<FVector2D> AllPoints;
                AllPoints.Append(AugPoly);
                AllPoints.Append(SteinerPoints);

                int32 BaseIdx = OutRoofData.Vertices.Num();

                for (int32 i = 0; i < AllPoints.Num(); i++)
                {
                        const FVector2D& Pt = AllPoints[i];
                        float Dist = FMath::Abs(SignedDistanceToLine(Pt, RidgeA, RidgeB));
                        float NormalizedDist = Dist / MaxDist;
                        float VertexZ = BaseZ + RidgeHeight * (1.f - NormalizedDist);

                        OutRoofData.Vertices.Add(FVector(Pt.X, Pt.Y, VertexZ));
                        OutRoofData.Normals.Add(FVector(0.f, 0.f, 1.f));
                        OutRoofData.VertexColors.Add(Color);
                        OutRoofData.Tangents.Add(FProcMeshTangent(FVector(1, 0, 0), false));
                        OutRoofData.UV0.Add(FVector2D(Pt.X / 100.f, Pt.Y / 100.f));
                }

                for (int32 TriIdx : Triangles)
                        OutRoofData.Triangles.Add(BaseIdx + TriIdx);

                // Gable walls: edges that straddle the ridge get a vertical triangle.
                for (int32 i = 0; i < CleanFP.Num(); i++)
                {
                        int32 Next = (i + 1) % CleanFP.Num();
                        const FVector& A = CleanFP[i];
                        const FVector& B = CleanFP[Next];

                        float DistA = SignedDistanceToLine(FVector2D(A.X, A.Y), RidgeA, RidgeB);
                        float DistB = SignedDistanceToLine(FVector2D(B.X, B.Y), RidgeA, RidgeB);

                        if (DistA * DistB < 0.f)
                        {
                                float EdgeDX = B.X - A.X, EdgeDY = B.Y - A.Y;
                                float Den = RidgeDir.X * EdgeDY - RidgeDir.Y * EdgeDX;
                                if (FMath::Abs(Den) > 0.0001f)
                                {
                                        float NumS = RidgeDir.X * (A.Y - RidgeA.Y) - RidgeDir.Y * (A.X - RidgeA.X);
                                        float S = NumS / Den;
                                        FVector2D Apex2D(A.X + S * EdgeDX, A.Y + S * EdgeDY);
                                        AddTriangle(OutRoofData,
                                                FVector(A.X, A.Y, BaseZ),
                                                FVector(B.X, B.Y, BaseZ),
                                                FVector(Apex2D.X, Apex2D.Y, BaseZ + RidgeHeight),
                                                Color);
                                }
                        }
                }

                return;
        }

        // Split succeeded — build two roof halves with gable walls
        for (const TArray<FVector2D>& SubPoly : SubPolygons)
        {
                if (SubPoly.Num() < 3) continue;

                TArray<int32> Triangles;
                TArray<FVector2D> SteinerPoints;
                FCGALSkeletonGenerator::TriangulateWithCGAL(SubPoly, Triangles, SteinerPoints);

                if (Triangles.Num() < 3)
                {
                        // Fall back to ear-clipping
                        TriangulatePolygon2D(SubPoly, Triangles);
                }

                TArray<FVector2D> AllPoints;
                AllPoints.Append(SubPoly);
                AllPoints.Append(SteinerPoints);

                int32 BaseIdx = OutRoofData.Vertices.Num();

                for (const FVector2D& V2D : AllPoints)
                {
                        float Dist = FMath::Abs(SignedDistanceToLine(V2D, RidgeA, RidgeB));
                        float NormalizedDist = Dist / MaxDist;
                        float VertexZ = BaseZ + RidgeHeight * (1.f - NormalizedDist);

                        OutRoofData.Vertices.Add(FVector(V2D.X, V2D.Y, VertexZ));
                        OutRoofData.Normals.Add(FVector(0.f, 0.f, 1.f));
                        OutRoofData.VertexColors.Add(Color);
                        OutRoofData.Tangents.Add(FProcMeshTangent(FVector(1, 0, 0), false));
                        OutRoofData.UV0.Add(FVector2D(V2D.X / 100.f, V2D.Y / 100.f));
                }

                for (int32 TriIdx : Triangles)
                        OutRoofData.Triangles.Add(BaseIdx + TriIdx);
        }

        // Add gable walls at the ridge line ends
        // Find boundary edges that cross the ridge line
        int32 N = CleanFP.Num();
        for (int32 i = 0; i < N; i++)
        {
                int32 Next = (i + 1) % N;
                const FVector& A = CleanFP[i];
                const FVector& B = CleanFP[Next];

                float DistA = SignedDistanceToLine(FVector2D(A.X, A.Y), RidgeA, RidgeB);
                float DistB = SignedDistanceToLine(FVector2D(B.X, B.Y), RidgeA, RidgeB);

                // A boundary edge whose endpoints straddle the ridge line is a
                // GABLE END edge. Only treat it as a gable end when the ridge
                // SEGMENT actually reaches near it (so the infinite ridge line
                // crossing a concave boundary elsewhere doesn't spawn bogus walls).
                if (DistA * DistB < 0.f)
                {
                        float EdgeDX = B.X - A.X;
                        float EdgeDY = B.Y - A.Y;
                        float Den = RidgeDir.X * EdgeDY - RidgeDir.Y * EdgeDX;
                        if (FMath::Abs(Den) > 0.0001f)
                        {
                                float NumS = RidgeDir.X * ((A.Y) - RidgeA.Y) - RidgeDir.Y * ((A.X) - RidgeA.X);
                                float S = NumS / Den;
                                FVector2D IntersectPt(A.X + S * EdgeDX, A.Y + S * EdgeDY);

                                // Convexity is guaranteed (concave buildings go to AddSkeletonHippedRoof).
                                // For a convex polygon cut by any line there are exactly 2 straddling edges,
                                // so no spurious crossings can exist. The old AlongRidge guard was rejecting
                                // valid gable walls when the skeleton-detected ridge was shorter than the
                                // footprint span — removed.

                                // Correct gable wall = single TRIANGLE: the eave edge
                                // (A,B at BaseZ) up to the apex on the ridge.
                                FVector Apex(IntersectPt.X, IntersectPt.Y, BaseZ + RidgeHeight);
                                AddTriangle(OutRoofData,
                                        FVector(A.X, A.Y, BaseZ),
                                        FVector(B.X, B.Y, BaseZ),
                                        Apex,
                                        Color);
                        }
                }
        }
        }
        catch (...)
        {
                UE_LOG(LogTemp, Warning, TEXT("CGAL Skeleton Gabled Roof: Exception caught, using hip fallback"));
                AddSkeletonHippedRoof(OutRoofData, Footprint, BaseZ, RidgeHeight, Color);
        }
}

// ============================================================================
// AddGambrelRoof
// ============================================================================
void FGeoMeshGenerator::AddGambrelRoof(
        FMeshData& OutRoofData,
        const TArray<FVector>& Footprint,
        float BaseZ,
        float RidgeHeight,
        const FColor& Color,
        float RoofDirection)
{
        if (Footprint.Num() < 3) return;

        // Clean footprint
        TArray<FVector> CleanFP;
        for (const FVector& V : Footprint)
        {
                FVector P(V.X, V.Y, BaseZ);
                if (CleanFP.Num() > 0 && P.Equals(CleanFP.Last(), 0.5f)) continue;
                if (CleanFP.Num() > 0 && P.Equals(CleanFP[0], 0.5f)) continue;
                CleanFP.Add(P);
        }
        if (CleanFP.Num() < 3) return;
        if (ComputeSignedArea2D(CleanFP) < 0.f) Algo::Reverse(CleanFP);

        // Convert to 2D for skeleton
        TArray<FVector2D> Poly2D;
        for (const FVector& V : CleanFP)
                Poly2D.Add(FVector2D(V.X, V.Y));

        // Generate straight skeleton
        FCGALSkeletonResult Skeleton = FCGALSkeletonGenerator::GenerateSkeleton(Poly2D);
        if (!Skeleton.IsValid() || Skeleton.MaxTime < 0.01f)
        {
                // Fallback: if skeleton fails, use hipped roof for complex footprints or pyramidal for simple ones
                if (Footprint.Num() > 5)
                        AddSkeletonHippedRoof(OutRoofData, Footprint, BaseZ, RidgeHeight, Color);
                else
                        AddPyramidalRoof(OutRoofData, Footprint, BaseZ, RidgeHeight, Color);
                return;
        }

        const float MaxT = FMath::Max(Skeleton.MaxTime, 1.f);
        const float SplitProgress = 0.3f;   // 30% of max time = break line
        const float EdgeBumpFactor = 0.3f;  // steepness factor

        // ========== اصلاح شده: ZForTime ==========
        auto ZForTime = [&](float T, bool bBottomPart) -> float
        {
                float Progress = T / MaxT;  // 0..1
                if (bBottomPart)
                {
                        // Bottom part: progress 0..SplitProgress
                        float LocalP = FMath::Clamp(Progress / SplitProgress, 0.f, 1.f);
                        float H = LocalP * (1.f + EdgeBumpFactor);
                        H = FMath::Clamp(H, 0.f, 1.f);
                        // Maximum height of bottom part = SplitProgress * RidgeHeight
                        return BaseZ + (SplitProgress * RidgeHeight) * H;
                }
                else
                {
                        // Top part: progress SplitProgress..1
                        float LocalP = (Progress - SplitProgress) / (1.f - SplitProgress);
                        LocalP = FMath::Clamp(LocalP, 0.f, 1.f);
                        float H = SplitProgress + LocalP * (1.f - SplitProgress) * (1.f + EdgeBumpFactor);
                        H = FMath::Clamp(H, 0.f, 1.f);
                        return BaseZ + RidgeHeight * H;
                }
        };
        // =========================================

        // Boundary check: is vertex on the footprint outline?
        auto IsOnBoundary = [&](const FVector2D& P) -> bool
        {
                for (const FVector2D& B : Poly2D)
                        if (FVector2D::DistSquared(P, B) < 1.f) return true;
                return false;
        };

        // Split each skeleton face into bottom and top parts using the split time
        for (const FCGALSkeletonFace& Face : Skeleton.Faces)
        {
                const int32 NF = Face.VertexIndices.Num();
                if (NF < 3) continue;

                // Collect vertices with their times
                TArray<FVector2D> Verts2D;
                TArray<float> Times;
                for (int32 Idx : Face.VertexIndices)
                {
                        if (Idx < 0 || Idx >= Skeleton.Vertices.Num()) continue;
                        const FCGALSkeletonVertex& SV = Skeleton.Vertices[Idx];
                        if (Verts2D.Num() > 0 && FVector2D::DistSquared(Verts2D.Last(), SV.Position) < 1.f) continue;
                        Verts2D.Add(SV.Position);
                        Times.Add(IsOnBoundary(SV.Position) ? 0.f : SV.Time);
                }
                if (Verts2D.Num() < 3) continue;
                // Remove last if same as first
                if (FVector2D::DistSquared(Verts2D[0], Verts2D.Last()) < 1.f)
                {
                        Verts2D.Pop();
                        Times.Pop();
                }

                // Split polygon at time = SplitProgress * MaxT
                TArray<FVector2D> BottomVerts, TopVerts;
                TArray<float> BottomTimes, TopTimes;
                for (int32 i = 0; i < Verts2D.Num(); i++)
                {
                        const FVector2D& A = Verts2D[i];
                        const FVector2D& B = Verts2D[(i + 1) % Verts2D.Num()];
                        float tA = Times[i];
                        float tB = Times[(i + 1) % Verts2D.Num()];
                        float splitT = SplitProgress * MaxT;

                        // Always add A to appropriate list
                        if (tA <= splitT)
                        {
                                BottomVerts.Add(A);
                                BottomTimes.Add(tA);
                        }
                        else
                        {
                                TopVerts.Add(A);
                                TopTimes.Add(tA);
                        }

                        // If edge crosses split time, add split point
                        if ((tA - splitT) * (tB - splitT) < 0.f)
                        {
                                float t = (splitT - tA) / (tB - tA);
                                FVector2D SplitPt = A + (B - A) * t;
                                BottomVerts.Add(SplitPt);
                                BottomTimes.Add(splitT);
                                TopVerts.Add(SplitPt);
                                TopTimes.Add(splitT);
                        }
                }

                // Triangulate bottom part (steep slope)
                if (BottomVerts.Num() >= 3)
                {
                        TArray<int32> BottomTris;
                        TriangulatePolygon2D(BottomVerts, BottomTris);
                        for (int32 t = 0; t + 2 < BottomTris.Num(); t += 3)
                        {
                                int32 i0 = BottomTris[t], i1 = BottomTris[t+1], i2 = BottomTris[t+2];
                                FVector V0(BottomVerts[i0].X, BottomVerts[i0].Y, ZForTime(BottomTimes[i0], true));
                                FVector V1(BottomVerts[i1].X, BottomVerts[i1].Y, ZForTime(BottomTimes[i1], true));
                                FVector V2(BottomVerts[i2].X, BottomVerts[i2].Y, ZForTime(BottomTimes[i2], true));
                                if (FVector::CrossProduct(V1 - V0, V2 - V0).Z < 0.f) Swap(V1, V2);
                                AddTriangle(OutRoofData, V0, V1, V2, Color);
                        }
                }

                // Triangulate top part (shallow slope)
                if (TopVerts.Num() >= 3)
                {
                        TArray<int32> TopTris;
                        TriangulatePolygon2D(TopVerts, TopTris);
                        for (int32 t = 0; t + 2 < TopTris.Num(); t += 3)
                        {
                                int32 i0 = TopTris[t], i1 = TopTris[t+1], i2 = TopTris[t+2];
                                FVector V0(TopVerts[i0].X, TopVerts[i0].Y, ZForTime(TopTimes[i0], false));
                                FVector V1(TopVerts[i1].X, TopVerts[i1].Y, ZForTime(TopTimes[i1], false));
                                FVector V2(TopVerts[i2].X, TopVerts[i2].Y, ZForTime(TopTimes[i2], false));
                                if (FVector::CrossProduct(V1 - V0, V2 - V0).Z < 0.f) Swap(V1, V2);
                                AddTriangle(OutRoofData, V0, V1, V2, Color);
                        }
                }
        }
}

// ============================================================================
// AddSkeletonMansardRoof
// Uses CGAL Straight Skeleton to compute the inner offset curve for the
// mansard break line, instead of the approximate InsetPolygonByEdgeOffset.
//
// Algorithm:
// 1. Generate straight skeleton of the footprint
// 2. Find the "break" vertices: skeleton vertices at time ~60% of MaxTime
// 3. These form the inner offset polygon (mansard break line)
// 4. Lower part: steep slope from boundary to break line
// 5. Upper part: shallow pyramidal roof from break line to apex
// ============================================================================
void FGeoMeshGenerator::AddSkeletonMansardRoof(
        FMeshData& OutRoofData,
        const TArray<FVector>& Footprint,
        float BaseZ,
        float RidgeHeight,
        const FColor& Color)
{
        if (Footprint.Num() < 3) return;

        try
        {

        // Clean footprint
        TArray<FVector> CleanFP;
        for (const FVector& V : Footprint)
        {
                FVector P(V.X, V.Y, BaseZ);
                if (CleanFP.Num() > 0 && P.Equals(CleanFP.Last(), 0.5f)) continue;
                if (CleanFP.Num() > 0 && P.Equals(CleanFP[0], 0.5f)) continue;
                CleanFP.Add(P);
        }
        if (CleanFP.Num() < 3) return;
        if (ComputeSignedArea2D(CleanFP) < 0.f) Algo::Reverse(CleanFP);

        // Convert to 2D
        TArray<FVector2D> Poly2D;
        for (const FVector& V : CleanFP)
                Poly2D.Add(FVector2D(V.X, V.Y));

        // Generate straight skeleton
        FCGALSkeletonResult Skeleton = FCGALSkeletonGenerator::GenerateSkeleton(Poly2D);

        if (!Skeleton.IsValid() || Skeleton.MaxTime < 0.01f)
        {
                UE_LOG(LogTemp, Warning, TEXT("CGAL Skeleton failed for mansard roof, falling back"));
                AddMansardRoof(OutRoofData, Footprint, BaseZ, RidgeHeight, Color);
                return;
        }

        // Find the inner offset polygon at 60% of MaxTime
        // This is the mansard break line where the steep lower slope meets
        // the shallow upper slope
        const float BreakTimeRatio = 0.6f;
        float BreakTime = Skeleton.MaxTime * BreakTimeRatio;
        float LowerH = RidgeHeight * BreakTimeRatio;
        float UpperH = RidgeHeight * (1.f - BreakTimeRatio);
        float MidZ = BaseZ + LowerH;

        // Collect skeleton vertices near the break time to form the inner polygon
        TArray<FVector2D> InnerPoly2D;
        float TimeTolerance = Skeleton.MaxTime * 0.15f; // 15% tolerance

        for (const FCGALSkeletonVertex& SV : Skeleton.Vertices)
        {
                if (SV.Time > 0.01f && FMath::Abs(SV.Time - BreakTime) < TimeTolerance)
                {
                        InnerPoly2D.Add(SV.Position);
                }
        }

        // If we couldn't find enough break vertices, fall back to edge-offset
        if (InnerPoly2D.Num() < 3)
        {
                TArray<FVector> InsetFP = InsetPolygonByEdgeOffset(Footprint, LowerH * 0.8f);
                for (FVector& V : InsetFP) V.Z = 0.f;
                AddExtrudedPolygon(OutRoofData, InsetFP, BaseZ, MidZ, Color, false);
                AddPyramidalRoof(OutRoofData, InsetFP, MidZ, UpperH, Color);
                return;
        }

        // Sort the inner polygon vertices in CCW order
        // Use angle from centroid
        FVector2D Centroid(0.f, 0.f);
        for (const FVector2D& P : InnerPoly2D)
                Centroid += P;
        Centroid /= (float)InnerPoly2D.Num();

        InnerPoly2D.Sort([&Centroid](const FVector2D& A, const FVector2D& B)
        {
                float AngleA = FMath::Atan2(A.Y - Centroid.Y, A.X - Centroid.X);
                float AngleB = FMath::Atan2(B.Y - Centroid.Y, B.X - Centroid.X);
                return AngleA < AngleB;
        });

        // Convert inner polygon to 3D
        TArray<FVector> InnerFP;
        for (const FVector2D& P : InnerPoly2D)
                InnerFP.Add(FVector(P.X, P.Y, 0.f));

        // Lower part: steep slope from boundary (BaseZ) to break line (MidZ)
        AddExtrudedPolygon(OutRoofData, CleanFP, BaseZ, MidZ, Color, false);
        AddExtrudedPolygon(OutRoofData, InnerFP, BaseZ, MidZ, Color, false);

        // Fill the sloped faces between outer and inner polygons
        int32 NumOuter = CleanFP.Num();
        int32 NumInner = InnerFP.Num();

        // For each outer edge, find the closest inner edge and create sloped quad
        for (int32 i = 0; i < NumOuter; i++)
        {
                int32 NextO = (i + 1) % NumOuter;
                FVector O0(CleanFP[i].X, CleanFP[i].Y, BaseZ);
                FVector O1(CleanFP[NextO].X, CleanFP[NextO].Y, BaseZ);

                // Find closest inner edge midpoint
                FVector2D EdgeMid = (FVector2D(CleanFP[i].X, CleanFP[i].Y) +
                        FVector2D(CleanFP[NextO].X, CleanFP[NextO].Y)) * 0.5f;

                int32 BestInner = 0;
                float BestDist = FLT_MAX;
                for (int32 j = 0; j < NumInner; j++)
                {
                        float D = FVector2D::Distance(EdgeMid, InnerPoly2D[j]);
                        if (D < BestDist) { BestDist = D; BestInner = j; }
                }

                int32 NextI = (BestInner + 1) % NumInner;
                FVector I0(InnerFP[BestInner].X, InnerFP[BestInner].Y, MidZ);
                FVector I1(InnerFP[NextI].X, InnerFP[NextI].Y, MidZ);

                AddQuad(OutRoofData, O0, O1, I1, I0, Color);
        }

        // Upper part: pyramidal/hipped roof from break line to apex
        AddPyramidalRoof(OutRoofData, InnerFP, MidZ, UpperH, Color);

        UE_LOG(LogTemp, Verbose,
                TEXT("CGAL Skeleton Mansard Roof: %d inner verts, breakTime=%.1f"),
                InnerPoly2D.Num(), BreakTime);
        }
        catch (...)
        {
                UE_LOG(LogTemp, Warning, TEXT("CGAL Skeleton Mansard Roof: Exception caught, falling back"));
                AddMansardRoof(OutRoofData, Footprint, BaseZ, RidgeHeight, Color);
        }
}

// ============================================================================
// ValidateCDTIndices - Check that all triangle indices are within bounds
// Prevents the 0xFFFFFFFF x 4 = 17GB OOM crash from invalid CGAL CDT output
// ============================================================================
bool FGeoMeshGenerator::ValidateCDTIndices(
        const TArray<int32>& Triangles,
        int32 VertexCount)
{
        for (int32 Idx : Triangles)
        {
                if (Idx < 0 || Idx >= VertexCount)
                {
                        UE_LOG(LogTemp, Error,
                                TEXT("Invalid CDT index: %d (vertex count: %d)"), Idx, VertexCount);
                        return false;
                }
        }
        return true;
}

// ============================================================================
// CleanFootprint - Remove duplicate vertices, ensure CCW winding order
// ============================================================================
TArray<FVector> FGeoMeshGenerator::CleanFootprint(
        const TArray<FVector>& Footprint,
        float BaseZ,
        float DuplicateTolerance)
{
        TArray<FVector> Result;
        for (const FVector& V : Footprint)
        {
                FVector P(V.X, V.Y, BaseZ);
                if (Result.Num() > 0 && P.Equals(Result.Last(), DuplicateTolerance)) continue;
                if (Result.Num() > 0 && P.Equals(Result[0], DuplicateTolerance)) continue;
                Result.Add(P);
        }
        if (Result.Num() >= 3 && ComputeSignedArea2D(Result) < 0.f)
                Algo::Reverse(Result);
        return Result;
}

// ============================================================================
// ============================================================================
// MAIN GENERATION FUNCTIONS
// ============================================================================
// ============================================================================

// ============================================================================
// GenerateRoofMeshes
// ============================================================================
// ----------------------------------------------------------------------------
// IsColorOnlyBuilding - towers, churches and other landmark structures keep
// their plain OSM colour instead of the tiled facade/roof atlas. Their tall or
// ornate shapes (spires, domes, naves) don't suit a repeating window facade.
// ----------------------------------------------------------------------------
static bool IsColorOnlyBuilding(const FGeoBuilding& B)
{
        const FString T = B.BuildingTypeStr.ToLower();
        const FString N = B.Name.ToLower();

        // OSM building= / amenity values that denote churches, places of worship
        // and free-standing towers.
        static const TCHAR* TypeKeys[] = {
                TEXT("church"), TEXT("cathedral"), TEXT("chapel"), TEXT("basilica"),
                TEXT("minster"), TEXT("monastery"), TEXT("mosque"), TEXT("synagogue"),
                TEXT("temple"), TEXT("shrine"), TEXT("religious"), TEXT("place_of_worship"),
                TEXT("tower"), TEXT("bell_tower"), TEXT("belfry"), TEXT("steeple"),
                TEXT("campanile")
        };
        for (const TCHAR* K : TypeKeys)
                if (T.Contains(K)) return true;

        // Common church/tower words in the building name (multilingual).
        static const TCHAR* NameKeys[] = {
                TEXT("church"), TEXT("kirche"), TEXT("cathedral"), TEXT("münster"),
                TEXT("munster"), TEXT("kapelle"), TEXT("basilica"), TEXT("tower"),
                TEXT("turm")
        };
        for (const TCHAR* K : NameKeys)
                if (N.Contains(K)) return true;

        // Spire-like roofs are almost always towers / churches.
        if (B.RoofShape == ERoofShape::Onion || B.RoofShape == ERoofShape::Dome)
                return true;

        return false;
}

// ----------------------------------------------------------------------------
// IsBuildingHasWindows - mirrors streets.gl logic: certain building types
// (garages, greenhouses, silos, barns, etc.) never have windows.
// Also, if the wall height is too short (< 2 m), no windows.
// ----------------------------------------------------------------------------
static bool IsBuildingHasWindows(const FGeoBuilding& B)
{
        const FString T = B.BuildingTypeStr.ToLower();

        // Building types that never have windows (from streets.gl isBuildingHasWindows)
        static const TCHAR* NoWindowTypes[] = {
                TEXT("garage"), TEXT("garages"), TEXT("greenhouse"),
                TEXT("storage_tank"), TEXT("bunker"), TEXT("silo"),
                TEXT("stadium"), TEXT("ship"), TEXT("castle"),
                TEXT("service"), TEXT("digester"), TEXT("water_tower"),
                TEXT("shed"), TEXT("ger"), TEXT("barn"),
                TEXT("slurry_tank"), TEXT("container"), TEXT("carport"),
                TEXT("roof")
        };
        for (const TCHAR* K : NoWindowTypes)
                if (T == K) return false;

        // Color-only buildings (churches, towers, etc.) also have no windows
        if (IsColorOnlyBuilding(B)) return false;

        return true;
}

// ----------------------------------------------------------------------------
// FWindowParams - per-building window parameters derived from building type
// and height, following the streets.gl approach.
//
// Key insight from streets.gl source code (WallsBuilder.ts + VectorAreaHandler.ts):
//   - windowWidth = 4 metres for ALL facade materials (plaster, brick, wood,
//     cementBlock, glass). This means one window every 4 m of wall length.
//   - Window count per wall = round(wallLength / windowWidth), then the
//     window texture tiles exactly windowCount times across the wall.
//   - UV V-axis is scaled by `levels`, so each floor maps to one texture repeat.
//   - If buildingWindows is false: textureIdWindow = textureIdWall (no window
//     texture, just wall texture everywhere).
//   - The window texture (e.g. FacadePlasterWindow) composites the window
//     glass cut-out on top of the wall background — no separate overlay needed.
// ----------------------------------------------------------------------------
struct FWindowParams
{
        float WindowSlotWidth;   // wall length per window slot (cm) — streets.gl: 400
};

// Returns window slot width based on building type and height.
// In streets.gl, windowWidth = 4 m for ALL materials. We use the same
// default but allow slight variation for industrial buildings.
// BuildingType: 0=residential, 1=commercial, 2=industrial, 3=other
// NumLevels: number of floors
// BuildingH: total building height in cm
static FWindowParams GetWindowParams(int32 BuildingType, int32 NumLevels, float BuildingH)
{
        FWindowParams P;
        // streets.gl: windowWidth = 4 m for all facade materials
        P.WindowSlotWidth = 400.f;    // 4 m per window slot (matches streets.gl exactly)

        // Industrial buildings have fewer, more widely-spaced windows
        if (BuildingType == 2)
        {
                P.WindowSlotWidth = 500.f;    // 5 m per window slot (fewer windows)
        }

        return P;
}

// ----------------------------------------------------------------------------
// Texture-atlas slice indices. MUST match the streets-gl ExtrudedTextures order
// and the TA_Building Texture2DArray slice order (files T_00..T_20). The atlas
// material reads the slice from TexCoord[1].R (= UV1.x).
// ----------------------------------------------------------------------------
namespace HiroGeoAtlas
{
        enum
        {
                RoofGeneric1 = 0, RoofGeneric2 = 1, RoofGeneric3 = 2, RoofGeneric4 = 3,
                RoofTiles = 4, RoofMetal = 5, RoofConcrete = 6, RoofThatch = 7,
                RoofEternit = 8, RoofGrass = 9, RoofGlass = 10, RoofTar = 11,
                FacadeGlass = 12, FacadeBrickWall = 13, FacadeBrickWindow = 14,
                FacadePlasterWall = 15, FacadePlasterWindow = 16, FacadeWoodWall = 17,
                FacadeWoodWindow = 18, FacadeBlockWall = 19, FacadeBlockWindow = 20,
        };
}

// FacadeMaterialType: 0=plaster, 1=brick, 2=dark_stone/block, 3=light_stone, 4=wood, 5=glass.
static float FacadeWallSlice(int32 FacadeMaterialType)
{
        switch (FacadeMaterialType)
        {
        case 1:  return (float)HiroGeoAtlas::FacadeBrickWall;   // brick
        case 2:  return (float)HiroGeoAtlas::FacadeBlockWall;   // dark stone -> concrete block
        case 3:  return (float)HiroGeoAtlas::FacadePlasterWall; // light stone -> plaster
        case 4:  return (float)HiroGeoAtlas::FacadeWoodWall;    // wood (streets-gl)
        case 5:  return (float)HiroGeoAtlas::FacadeGlass;       // glass curtain
        default: return (float)HiroGeoAtlas::FacadePlasterWall; // plaster / default
        }
}

// Returns the window-panel atlas slice that matches the facade material.
// Each wall material has a paired window slice (brickWindow, plasterWindow, ...)
// that overlays the window glass cut-out pattern on the correct wall texture.
// Glass curtain-walls (FacadeGlass) have no separate window slice — the glass
// panel itself IS the window, so we fall back to FacadeGlass.
static float FacadeWindowSlice(int32 FacadeMaterialType)
{
        switch (FacadeMaterialType)
        {
        case 1:  return (float)HiroGeoAtlas::FacadeBrickWindow;    // brick + window cutout
        case 2:  return (float)HiroGeoAtlas::FacadeBlockWindow;    // block + window cutout
        case 3:  return (float)HiroGeoAtlas::FacadePlasterWindow;  // plaster + window cutout
        case 4:  return (float)HiroGeoAtlas::FacadeWoodWindow;     // wood + window cutout
        case 5:  return (float)HiroGeoAtlas::FacadeGlass;
        default: return (float)HiroGeoAtlas::FacadePlasterWindow;  // plaster / default
        }
}

// Roof slice from OSM roof:material. Flat default roofs cycle the 4 generic
// textures; non-flat default roofs use concrete (mirrors streets-gl).
static float RoofSlice(const FGeoBuilding& B, int32 BuildingIndex)
{
        const FString M = B.RoofMaterial.ToLower();
        if (M == TEXT("tiles"))    return (float)HiroGeoAtlas::RoofTiles;
        if (M == TEXT("metal"))    return (float)HiroGeoAtlas::RoofMetal;
        if (M == TEXT("concrete")) return (float)HiroGeoAtlas::RoofConcrete;
        if (M == TEXT("thatch"))   return (float)HiroGeoAtlas::RoofThatch;
        if (M == TEXT("eternit"))  return (float)HiroGeoAtlas::RoofEternit;
        if (M == TEXT("grass"))    return (float)HiroGeoAtlas::RoofGrass;
        if (M == TEXT("glass"))    return (float)HiroGeoAtlas::RoofGlass;
        if (M == TEXT("tar"))      return (float)HiroGeoAtlas::RoofTar;

        if (B.RoofShape == ERoofShape::Flat)
        {
                // Churches / landmarks must NOT use the generic roof textures (00-03);
                // give their flat roofs a plain concrete slice instead.
                if (IsColorOnlyBuilding(B))
                        return (float)HiroGeoAtlas::RoofConcrete;

                static const int32 Generic[4] = {
                        HiroGeoAtlas::RoofGeneric1, HiroGeoAtlas::RoofGeneric2,
                        HiroGeoAtlas::RoofGeneric3, HiroGeoAtlas::RoofGeneric4 };
                return (float)Generic[((BuildingIndex % 4) + 4) % 4];
        }
        return (float)HiroGeoAtlas::RoofConcrete;
}

// Stamp UV1.x = Slice for every vertex appended since `Start` (UV1.y unused).
// We use SetNum (not SetNumZeroed) so that atlas slices written by previous
// buildings earlier in the same shared buffer are never overwritten with zero.
static void FillSliceUV1(FGeoMeshGenerator::FMeshData& D, int32 Start, float Slice)
{
        if (D.Vertices.Num() <= Start) return;
        // Grow UV1 to match Vertices without zeroing existing entries.
        if (D.UV1.Num() < D.Vertices.Num())
                D.UV1.SetNum(D.Vertices.Num());
        for (int32 i = Start; i < D.Vertices.Num(); ++i)
                D.UV1[i] = FVector2D(Slice, 0.f);
}

// Emit a single OUTWARD-facing vertical quad (corner order BL, BR, TR, TL) using
// the SAME triangle winding + normal convention as AddExtrudedPolygon's walls.
// AddQuad() winds the opposite way, so vertical AddQuad faces (e.g. windows) end
// up back-face culled / lit from the inside. Window & curtain-glass faces must
// use this helper so they read correctly from the street. Normals are baked here
// (outward), so the glass section must NOT be run through RecomputeNormals().
//
// UV parameters (U0,U1,V0,V1) default to wall-coordinate space for backward
// compatibility. When using streets.gl window-texture mode, pass:
//   U0 = 0, U1 = windowCount  (one texture repeat per window slot)
//   V0 = 0, V1 = levels       (one texture repeat per floor)
static void AddOutwardWallQuad(
        FGeoMeshGenerator::FMeshData& MD,
        const FVector& BL, const FVector& BR,
        const FVector& TR, const FVector& TL,
        const FColor& Color,
        float U0 = -1.f, float U1 = -1.f,
        float V0 = -1.f, float V1 = -1.f)
{
        FVector EdgeDir = BR - BL;
        FVector OutN(EdgeDir.Y, -EdgeDir.X, 0.f);
        if (!OutN.IsNearlyZero()) OutN.Normalize(); else OutN = FVector(0, 0, 1);
        FVector Tang = EdgeDir; if (!Tang.IsNearlyZero()) Tang.Normalize(); else Tang = FVector(1, 0, 0);

        const int32 BI = MD.Vertices.Num();
        MD.Vertices.Add(BL); MD.Vertices.Add(BR); MD.Vertices.Add(TR); MD.Vertices.Add(TL);
        // Same winding as AddExtrudedPolygon: (0,2,1) + (0,3,2).
        MD.Triangles.Add(BI + 0); MD.Triangles.Add(BI + 2); MD.Triangles.Add(BI + 1);
        MD.Triangles.Add(BI + 0); MD.Triangles.Add(BI + 3); MD.Triangles.Add(BI + 2);

        // If UV params are negative, use wall-coordinate space (metres) —
        // this preserves backward compatibility with existing callers.
        if (U0 < 0.f)
        {
                const float WW = FVector2D::Distance(FVector2D(BL.X, BL.Y), FVector2D(BR.X, BR.Y)) / 100.f;
                MD.UV0.Add(FVector2D(0.f, BL.Z / 100.f)); MD.UV0.Add(FVector2D(WW, BR.Z / 100.f));
                MD.UV0.Add(FVector2D(WW, TR.Z / 100.f)); MD.UV0.Add(FVector2D(0.f, TL.Z / 100.f));
        }
        else
        {
                // Explicit UV (streets.gl window-texture mode)
                MD.UV0.Add(FVector2D(U0, V0)); MD.UV0.Add(FVector2D(U1, V0));
                MD.UV0.Add(FVector2D(U1, V1)); MD.UV0.Add(FVector2D(U0, V1));
        }
        for (int32 v = 0; v < 4; v++)
        {
                MD.Normals.Add(OutN);
                MD.VertexColors.Add(Color);
                MD.Tangents.Add(FProcMeshTangent(Tang, false));
        }
}

// ----------------------------------------------------------------------------
// AddBoxFace — a single quad with an EXPLICIT normal (used by the 3D frame bars).
// V0,V1,V2,V3 are in order that produces the correct face with the given Normal.
// ----------------------------------------------------------------------------
static void AddBoxFace(
        FGeoMeshGenerator::FMeshData& MD,
        const FVector& V0, const FVector& V1,
        const FVector& V2, const FVector& V3,
        const FVector& Normal, const FColor& Color)
{
        FVector Tang = (V1 - V0);
        if (!Tang.IsNearlyZero()) Tang.Normalize(); else Tang = FVector(1, 0, 0);

        const int32 BI = MD.Vertices.Num();
        MD.Vertices.Add(V0); MD.Vertices.Add(V1); MD.Vertices.Add(V2); MD.Vertices.Add(V3);
        MD.Triangles.Add(BI + 0); MD.Triangles.Add(BI + 1); MD.Triangles.Add(BI + 2);
        MD.Triangles.Add(BI + 0); MD.Triangles.Add(BI + 2); MD.Triangles.Add(BI + 3);

        // UV0: horizontal distance in metres, vertical in metres
        const float W = FVector2D::Distance(FVector2D(V0.X, V0.Y), FVector2D(V1.X, V1.Y)) / 100.f;
        const float H0 = V0.Z / 100.f, H1 = V1.Z / 100.f, H2 = V2.Z / 100.f, H3 = V3.Z / 100.f;
        MD.UV0.Add(FVector2D(0.f, H0));
        MD.UV0.Add(FVector2D(W,  H1));
        MD.UV0.Add(FVector2D(W,  H2));
        MD.UV0.Add(FVector2D(0.f, H3));

        for (int32 v = 0; v < 4; v++)
        {
                MD.Normals.Add(Normal);
                MD.VertexColors.Add(Color);
                MD.Tangents.Add(FProcMeshTangent(Tang, false));
        }
}

// ----------------------------------------------------------------------------
// AddFrameBar3D — build a 3D rectangular bar for a window frame.
//
// BBL, BBR, BTR, BTL are the 4 corners of the back face (against the wall /
// glass surface), in the SAME order as AddOutwardWallQuad (BL, BR, TR, TL).
// Protrusion is the vector from back to front (direction + magnitude = depth).
// The bar has 5 visible faces (front, top, bottom, left-cap, right-cap).
// The back face is skipped — it sits against the wall or is hidden by glass.
// ----------------------------------------------------------------------------
static void AddFrameBar3D(
        FGeoMeshGenerator::FMeshData& MD,
        const FVector& BBL, const FVector& BBR,
        const FVector& BTR, const FVector& BTL,
        const FVector& Protrusion,
        const FColor& Color)
{
        // Front face corners
        FVector FBL = BBL + Protrusion;
        FVector FBR = BBR + Protrusion;
        FVector FTR = BTR + Protrusion;
        FVector FTL = BTL + Protrusion;

        // Normal for front face
        FVector FrontN = Protrusion;
        if (!FrontN.IsNearlyZero()) FrontN.Normalize();
        else FrontN = FVector(0, 0, 1);

        // Edge direction (along the bar length, from left to right)
        FVector EdgeDir = BBR - BBL;
        FVector SideN(EdgeDir.Y, -EdgeDir.X, 0.f);   // outward perpendicular in XY
        if (!SideN.IsNearlyZero()) SideN.Normalize();
        else SideN = FVector(1, 0, 0);
        FVector InwardN = -SideN;                      // toward glass opening

        // 1) Front face (facing the street)
        AddBoxFace(MD, FBL, FBR, FTR, FTL, FrontN, Color);

        // 2) Top face (if bar has height, the top face is visible)
        AddBoxFace(MD, BTL, BTR, FTR, FTL, FVector(0, 0, 1), Color);

        // 3) Bottom face
        AddBoxFace(MD, BBL, FBL, FBR, BBR, FVector(0, 0, -1), Color);

        // 4) Inner face (facing the glass opening — the reveal)
        AddBoxFace(MD, BBR, BTR, FTR, FBR, InwardN, Color);

        // 5) Outer face (facing away from the glass — the outer reveal)
        AddBoxFace(MD, BBL, FBL, FTL, BTL, SideN, Color);
}

void FGeoMeshGenerator::GenerateRoofMeshes(
        const TArray<FGeoBuilding>& Buildings,
        FMeshData& OutRoofData,
        FMeshData& OutRoofColorData,
        const TArray<float>* BuildingZOffsets)
{
        OutRoofData.Reset();
        OutRoofColorData.Reset();

        int32 RoofCounter = 0;

        const float FoundationHeight = 50.0f;
        const float DefaultRoofPitch = 1.0f;

        for (int32 BIdx = 0; BIdx < Buildings.Num(); BIdx++)
        {
                const FGeoBuilding& Building = Buildings[BIdx];
                const float ZOff = (BuildingZOffsets && BuildingZOffsets->IsValidIndex(BIdx))
                        ? (*BuildingZOffsets)[BIdx] : 0.f;
                if (Building.Nodes.Num() < 3) continue;
                // Outline covered by building:parts: parts define the roof, skip outline.
                if (Building.bSuppressOutline) continue;
                // Part whose flat roof cap is hidden by another part sitting on top.
                if (Building.bSuppressRoof) continue;

                const int32 RoofIndex = RoofCounter++;

                TArray<FVector> LocalFootprint;
                for (const FVector2D& N : Building.Nodes)
                        LocalFootprint.Add(FVector(N.X * ScaleFactor, -N.Y * ScaleFactor, 0.f));

                float BuildingH = (float)(Building.Height * (double)ScaleFactor);
                if (BuildingH < 100.f) BuildingH = 300.f;
                float MinH = (float)(Building.MinHeight * (double)ScaleFactor);

                FColor RoofFColor = LinearToFColor(Building.RoofColor);

                float RoofMinX, RoofMaxX, RoofMinY, RoofMaxY;
                ComputeFootprintBounds(LocalFootprint, RoofMinX, RoofMaxX, RoofMinY, RoofMaxY);
                const float HalfShort = FMath::Min(RoofMaxX - RoofMinX, RoofMaxY - RoofMinY) * 0.5f;

                // --- Roof height -------------------------------------------------
                // bRoofHeightExplicit: the roof's vertical extent came straight from
                // OSM (roof:height / roof:levels). In that case OSM's `height` is the
                // ABSOLUTE TOTAL height INCLUDING the roof, so the wall top must be
                // lowered to (height - roof:height) and the roof fills the top RoofH.
                // When no explicit roof height exists we SYNTHESISE a pitch; then
                // OSM's `height` is the eaves/wall height and the roof is added on
                // top (no lowering), matching the historical behaviour.
                bool bRoofHeightExplicit = (Building.RoofHeight > 0.0) || (Building.RoofLevels > 0);
                float RoofH = 0.f;
                if (Building.RoofHeight > 0.0)
                {
                        RoofH = (float)(Building.RoofHeight * (double)ScaleFactor);
                }
                else if (Building.RoofLevels > 0)
                {
                        RoofH = Building.RoofLevels * (float)(FOSMTagHelpers::LevelHeightMeters * (double)ScaleFactor);
                }
                else
                {
                        RoofH = FMath::Max(50.f, HalfShort * DefaultRoofPitch);
                }

                // Prominence floor for PITCHED roofs: most OSM buildings carry no
                // roof:height, so the loader hands us a weak ~3 m default that makes
                // gabled/hipped roofs read almost flat. Give pitched shapes a pitch
                // proportional to the building's short side (~30°), matching the
                // streets.gl look, while leaving tall explicit roofs untouched.
                switch (Building.RoofShape)
                {
                case ERoofShape::Gabled:
                case ERoofShape::Saltbox:
                case ERoofShape::QuadrupleSaltbox:
                case ERoofShape::CrossGabled:
                case ERoofShape::Hipped:
                case ERoofShape::HalfHipped:
                case ERoofShape::Pyramidal:
                case ERoofShape::Mansard:
                case ERoofShape::Gambrel:
                        if (HalfShort > 1.f && !bRoofHeightExplicit)
                                RoofH = FMath::Max(RoofH, HalfShort * 2.0f);
                        break;
                case ERoofShape::Dome:
                        // Hemisphere ≈ radius tall.
                        if (HalfShort > 1.f && !bRoofHeightExplicit)
                                RoofH = FMath::Max(RoofH, HalfShort * 2.0f);
                        break;
                case ERoofShape::Onion:
                        // Onion height grows SUB-linearly with width: a narrow tower bulb
                        // reads tall and slender, while a wide bulb stays proportionally
                        // shorter (a pure ratio makes wide domes balloon and narrow ones
                        // look stubby). Linear floor calibrated on real domes — a ~1.4 m
                        // radius bulb → ~3.6 m tall, a ~3.1 m radius bulb → ~4.5 m. The
                        // constant is expressed in ScaleFactor units so it stays correct
                        // if the world scale changes.
                        if (HalfShort > 1.f && !bRoofHeightExplicit)
                                RoofH = FMath::Max(RoofH, 2.82f * ScaleFactor + 0.535f * HalfShort);
                        break;
                default:
                        break;
                }

                // For building:parts, Height and MinHeight are ABSOLUTE from ground.
                // EffBase = the start of this part's walls; AbsTop = absolute peak.
                // For plain buildings, Height is relative: AbsTop = EffBase + Height.
                float EffBase, AbsTop;
                if (Building.bIsBuildingPart)
                {
                        EffBase = (MinH > 10.f) ? MinH : FoundationHeight;
                        AbsTop = FMath::Max(BuildingH, EffBase + 10.f);
                }
                else
                {
                        EffBase = FMath::Max(MinH, FoundationHeight);
                        AbsTop = EffBase + BuildingH;
                }

                // Onion/dome turret: drop its column down to the surrounding roofline so
                // the dome rises out of the adjacent pitched roof instead of standing on
                // a full-height column. Only when a pitched neighbour shares an edge.
                if (Building.RoofShape == ERoofShape::Onion || Building.RoofShape == ERoofShape::Dome)
                {
                        float Eave = DomeNestleEave(Building, Buildings, ScaleFactor, AbsTop, EffBase);
                        if (Eave > 0.f)
                                AbsTop = Eave;
                }

                // Round (barrel) roof without an explicit roof:height: the semicircle
                // must sit WITHIN the building's total height (walls lowered by the arch
                // radius), exactly like an explicit roof height — otherwise the barrel
                // floats on top of full-height walls. Derive the rise from the footprint
                // (in local units here) and treat it as roof-inside-height.
               bool bRoofInsideHeight = bRoofHeightExplicit;
                // Round بدون explicit: barrel rise را محاسبه می‌کنیم
                if (Building.RoofShape == ERoofShape::Round && !bRoofHeightExplicit)
                {
                float Rise = ComputeBarrelRise(LocalFootprint, Building.RoofOrientation);
                if (Rise > 1.f) { RoofH = Rise; bRoofInsideHeight = true; }
                }
                // ← اضافه کن: تمام pitched shapes با explicit roof height
                // باید WallTopZ را پایین بکشند، نه فقط Round
                if (bRoofHeightExplicit && RoofShapeHasEave(Building.RoofShape))
                {
                bRoofInsideHeight = true;
                }
                // Skillion shoulder handling:
                //  • If it touches an onion/dome turret, slope its low edge all the way
                //    down to the shared complex roofline so it ATTACHES to the lowered
                //    dome column (stretches toward the dome) instead of floating above it.
                //  • Otherwise just soften any over-steep small skillion.
                if (Building.RoofShape == ERoofShape::Skillion)
                {
                        float AttachEave = IsAdjacentToOnionDome(Building, Buildings)
                                ? DomeNestleEave(Building, Buildings, ScaleFactor, AbsTop, EffBase) : -1.f;
                        if (AttachEave > 0.f)
                                RoofH = AbsTop - AttachEave;
                        else
                                RoofH = GentleSkillionRise(LocalFootprint, (float)Building.RoofDirection, RoofH);
                }

                // WallTopZ: where walls end and the roof begins. With an explicit roof
                // height OSM's height already includes the roof, so walls stop at
                // AbsTop - RoofH and the roof occupies the top RoofH (peak == AbsTop).
                float WallTopZ = AbsTop;
                if (bRoofInsideHeight)
                {
                        WallTopZ = FMath::Max(EffBase + 10.f, AbsTop - RoofH);
                        RoofH = AbsTop - WallTopZ; // never poke above the absolute top
                }

                // Inner courtyard rings (holes), in the same scaled 2D space as
                // the footprint, so the roof can exclude them.
                TArray<TArray<FVector2D>> HoleRings2D;
                for (const TArray<FVector2D>& Hole : Building.Holes)
                {
                        if (Hole.Num() < 3) continue;
                        TArray<FVector2D> Ring;
                        Ring.Reserve(Hole.Num());
                        for (const FVector2D& H : Hole)
                                Ring.Add(FVector2D(H.X * ScaleFactor, -H.Y * ScaleFactor));
                        HoleRings2D.Add(MoveTemp(Ring));
                }

                // RoofDirection: -1 if not set in OSM (auto-detect from AABB/skeleton)
                float RoofDir = (float)Building.RoofDirection; // -1 = not specified

                // Flat roofs of ordinary buildings get the textured atlas; every
                // other roof (pitched, domed, ...) and any tower/church roof keeps
                // its plain OSM colour. Route to the matching mesh section.
                //
                // QuadrupleSaltbox without an explicit roof:height is almost always
                // a mapper error on flat commercial/retail buildings (e.g. Way 85765746).
                // Downgrade to Flat so we don't generate a pitched skeleton roof on
                // a building that is clearly intended to have no visible pitch.
                ERoofShape EffectiveRoofShape = Building.RoofShape;
                if (EffectiveRoofShape == ERoofShape::QuadrupleSaltbox && !bRoofHeightExplicit)
                        EffectiveRoofShape = ERoofShape::Flat;

                const bool bColorOnly = IsColorOnlyBuilding(Building);
                const bool bAtlasRoof = (EffectiveRoofShape == ERoofShape::Flat) && !bColorOnly;
                FMeshData& RoofTarget = bAtlasRoof ? OutRoofData : OutRoofColorData;

                const int32 VertsBefore = RoofTarget.Vertices.Num();
                const int32 TrisBefore = RoofTarget.Triangles.Num();
                AddRoofGeometry(RoofTarget, EffectiveRoofShape, LocalFootprint, WallTopZ, RoofH, RoofFColor,
                        HoleRings2D, RoofDir, Building.RoofOrientation, LinearToFColor(Building.WallColor));

                // Safety net (fixes "some roofs are missing"): if the shape-specific
                // builder degenerated and emitted no triangles, the building would be
                // left open at the top. Guarantee a closed roof by capping it with a
                // robust flat CDT roof flush with the wall top.
                if (RoofTarget.Triangles.Num() == TrisBefore)
                {
                        if (RoofTarget.Vertices.Num() > VertsBefore)
                        {
                                RoofTarget.Vertices.SetNum(VertsBefore);
                                RoofTarget.Normals.SetNum(VertsBefore);
                                RoofTarget.VertexColors.SetNum(VertsBefore);
                                RoofTarget.Tangents.SetNum(VertsBefore);
                                RoofTarget.UV0.SetNum(VertsBefore);
                        }
                        AddRoofGeometry(RoofTarget, ERoofShape::Flat, LocalFootprint, WallTopZ, RoofH, RoofFColor,
                                HoleRings2D, RoofDir, Building.RoofOrientation);
                }

                // Stamp the chosen roof atlas slice into UV1 for this building.
                FillSliceUV1(RoofTarget, VertsBefore, RoofSlice(Building, RoofIndex));

                // Keep the roof flush with this building's (possibly terrain-shifted)
                // walls — see ComputeBuildingTerrainOffsets / GenerateBuildingMeshes.
                ShiftZRange(RoofTarget, VertsBefore, ZOff);
        }

        RecomputeNormals(OutRoofData);
        RecomputeTangents(OutRoofData);
        RecomputeNormals(OutRoofColorData);
        RecomputeTangents(OutRoofColorData);
}

// ============================================================================
// ComputeBuildingTerrainOffsets
// ============================================================================
TArray<float> FGeoMeshGenerator::ComputeBuildingTerrainOffsets(
        const TArray<FGeoBuilding>& Buildings, float InScaleFactor,
        const FHiroGeoHeightmap* DemHeightmap, double OriginLatitude, double OriginLongitude,
        const TArray<FElevationSample>* ElevationSamples)
{
        TArray<float> Offsets;
        Offsets.SetNumZeroed(Buildings.Num());

        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const bool bHaveSamples = ElevationSamples && ElevationSamples->Num() >= 3;
        if (!bHaveDem && !bHaveSamples)
                return Offsets;

        const double RefElev = bHaveDem
                ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : ComputeElevationSamplesRefElev(*ElevationSamples);

        for (int32 i = 0; i < Buildings.Num(); i++)
        {
                const FGeoBuilding& B = Buildings[i];
                if (B.Nodes.Num() == 0)
                        continue;

                // streets-gl: terrainMinHeight = min elevation under footprint vertices
                // (not centroid — on slopes the centroid can sit above/below the base).
                Offsets[i] = bHaveDem
                        ? SampleDemMinOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude,
                                RefElev, InScaleFactor, B.Nodes)
                        : SampleElevationSamplesMinOffsetUE(*ElevationSamples, RefElev,
                                InScaleFactor, B.Nodes);
        }

        return Offsets;
}

// ============================================================================
// GenerateBuildingMeshes (with windows, facade materials, etc.)
// ============================================================================
// OutWindowData receives the wall-inset window panels (brick_window, plaster_window,
// wood_window, block_window slices). They are separate from OutGlassData so the
// material can sample the matching *_Window atlas slice (which composites the window
// cut-out on top of the wall texture) rather than the plain glass slice.
void FGeoMeshGenerator::GenerateBuildingMeshes(
        const TArray<FGeoBuilding>& Buildings,
        FMeshData& OutWallData, FMeshData& OutGlassData,
        FMeshData& OutBrickData, FMeshData& OutDarkStoneData, FMeshData& OutLightStoneData,
        FMeshData& OutColorWallData, FMeshData& OutWindowData,
        FMeshData& OutWindowFrameData, FMeshData& OutWindowGlassData,
        const TArray<float>* BuildingZOffsets, const TArray<float>* BuildingFoundationDepths)
{
        OutWallData.Reset(); OutGlassData.Reset();
        OutBrickData.Reset(); OutDarkStoneData.Reset(); OutLightStoneData.Reset();
        OutColorWallData.Reset(); OutWindowData.Reset();
        OutWindowFrameData.Reset(); OutWindowGlassData.Reset();

        // Glass: a desaturated, slightly dark blue-grey reads as real window glass
        // at city scale (the old saturated sky-blue tinted whole facades blue).
        FColor WallColor(180, 170, 160), GlassColor(92, 110, 130, 255), FoundationColor(120, 115, 110);

        const float FoundationHeight = 50.f;

        for (int32 BIdx = 0; BIdx < Buildings.Num(); BIdx++)
        {
                const FGeoBuilding& Building = Buildings[BIdx];
                const float ZOff = (BuildingZOffsets && BuildingZOffsets->IsValidIndex(BIdx))
                        ? (*BuildingZOffsets)[BIdx] : 0.f;
                if (Building.Nodes.Num() < 3) continue;
                // Outline covered by building:parts: parts define the walls, skip outline.
                if (Building.bSuppressOutline) continue;

                TArray<FVector> LocalFootprint;
                for (const FVector2D& N : Building.Nodes)
                        LocalFootprint.Add(FVector(N.X * ScaleFactor, -N.Y * ScaleFactor, 0.f));

                float BuildingH = (float)(Building.Height * (double)ScaleFactor);
                // streets-gl fallback: 1 level × 4 m when height is missing/tiny.
                if (BuildingH < 100.f) BuildingH = (float)(FOSMTagHelpers::LevelHeightMeters * (double)ScaleFactor);

                float MinH = (float)(Building.MinHeight * (double)ScaleFactor);
                float WallBase = MinH;

                int32 NumLevels = Building.Levels > 0 ? Building.Levels
                        : FMath::Max(1, FMath::RoundToInt(BuildingH / (float)(FOSMTagHelpers::LevelHeightMeters * (double)ScaleFactor)));
                float LevelH = BuildingH / (float)NumLevels;

                // Per-building diagnostics: see exactly what heights OSM produced.
                // Heights are printed in METERS (BuildingH is in UE units = m * ScaleFactor).
                UE_LOG(LogTemp, Log,
                        TEXT("HiroGeo[Bld] '%s' part=%d type=%d roofShape=%d  Height=%.1fm MinHeight=%.1fm Levels=%d (NumLevels=%d) RoofHeight=%.1fm"),
                        *Building.Name,
                        Building.bIsBuildingPart ? 1 : 0,
                        Building.BuildingType,
                        (int32)Building.RoofShape,
                        BuildingH / ScaleFactor,
                        MinH / ScaleFactor,
                        Building.Levels,
                        NumLevels,
                        (float)Building.RoofHeight);

                // Only add a foundation slab when the building (or part) genuinely
                // starts at ground level (MinH < 10 cm in UE units).
                // For building:parts that sit elevated above ground (MinH > 0, e.g.
                // the towers of the Frauenkirche), skipping the foundation prevents
                // the spurious solid panel that would fill the gap between the two towers.
                // Towers / churches keep their plain OSM colour (no facade atlas).
                const bool bColorOnly = IsColorOnlyBuilding(Building);

                // Capture per-array vertex counts so we can stamp this building's
                // atlas slice into UV1 for exactly the geometry it adds.
                const int32 UV1_Wall  = OutWallData.Vertices.Num();
                const int32 UV1_Brick = OutBrickData.Vertices.Num();
                const int32 UV1_Dark  = OutDarkStoneData.Vertices.Num();
                const int32 UV1_Light = OutLightStoneData.Vertices.Num();
                const int32 UV1_Color = OutColorWallData.Vertices.Num();
                const int32 UV1_Glass  = OutGlassData.Vertices.Num();
                // OutWindowData: wall-inset window panels with material-matched slice.
                const int32 UV1_Window = OutWindowData.Vertices.Num();
                // 3D window frame + glass: frame uses the wall slice, glass uses FacadeGlass.
                const int32 UV1_Frame = OutWindowFrameData.Vertices.Num();
                const int32 UV1_WGlass = OutWindowGlassData.Vertices.Num();

                if (MinH < 10.f)
                        AddExtrudedPolygon(bColorOnly ? OutColorWallData : OutWallData,
                                LocalFootprint, BuildingFoundationDepths && BuildingFoundationDepths->IsValidIndex(BIdx)
                                        ? -(*BuildingFoundationDepths)[BIdx] : 0.f,
                                FoundationHeight, FoundationColor, false);

                float EffBase, WallTop;
                // In OSM, Height is ALWAYS the absolute height from ground level
                // (for both building outlines and building:parts).
                // MinHeight is the absolute height where the walls start (e.g. the
                // base of a tower sitting on top of a podium).
                // So: EffBase = max(MinH, FoundationHeight), WallTop = Height (absolute).
                // For plain buildings (not parts) that carry no MinHeight, the old
                // relative formula (EffBase + Height) was used — keep that for
                // non-part buildings so single-outline buildings still look correct.
                if (Building.bIsBuildingPart)
                {
                        // building:part — both Height and MinHeight are absolute from ground.
                        // Use MinH as-is (even if 0) so the part starts at the right level.
                        EffBase = (MinH > 10.f) ? MinH : FoundationHeight;
                        WallTop = FMath::Max(BuildingH, EffBase + 10.f);
                }
                else
                {
                        // Plain building outline — Height is relative to ground.
                        EffBase = FMath::Max(WallBase, FoundationHeight);
                        WallTop = EffBase + BuildingH;
                }

                // When the roof height is EXPLICIT in OSM (roof:height / roof:levels),
                // OSM's `height` is the absolute TOTAL including the roof. The walls
                // must therefore stop at (top - roof:height) so the pitched/domed
                // roof generated in GenerateRoofMeshes occupies the top RoofH instead
                // of being stacked on top of full-height walls (which made the
                // Frauenkirche nave read as 90 m instead of 60 m). This mirrors the
                // WallTopZ computation in GenerateRoofMeshes exactly.
                if ((Building.RoofHeight > 0.0) || (Building.RoofLevels > 0))
                {
                        float ExplicitRoofH = (Building.RoofHeight > 0.0)
                                ? (float)(Building.RoofHeight * (double)ScaleFactor)
                                : Building.RoofLevels * (float)(FOSMTagHelpers::LevelHeightMeters * (double)ScaleFactor);
                        // Match GenerateRoofMeshes for skillions: a shoulder touching an
                        // onion/dome slopes down to the shared roofline so it attaches to
                        // the dome; otherwise soften over-steep small skillions.
                        if (Building.RoofShape == ERoofShape::Skillion)
                        {
                                float AttachEave = IsAdjacentToOnionDome(Building, Buildings)
                                        ? DomeNestleEave(Building, Buildings, ScaleFactor, WallTop, EffBase) : -1.f;
                                if (AttachEave > 0.f)
                                        ExplicitRoofH = WallTop - AttachEave;
                                else
                                        ExplicitRoofH = GentleSkillionRise(LocalFootprint, (float)Building.RoofDirection, ExplicitRoofH);
                        }
                        WallTop = FMath::Max(EffBase + 10.f, WallTop - ExplicitRoofH);
                }
                else if (Building.RoofShape == ERoofShape::Round)
                {
                        // Mirror GenerateRoofMeshes: a barrel roof with no explicit
                        // roof:height sits within the total height, so lower the walls by
                        // the arch radius. Without this the walls would reach full height
                        // and the semicircular roof would float above them.
                        float Rise = ComputeBarrelRise(LocalFootprint, Building.RoofOrientation);
                        if (Rise > 1.f)
                                WallTop = FMath::Max(EffBase + 10.f, WallTop - Rise);
                }

                // Onion/dome turret: drop the column to the adjacent roofline (mirror of
                // GenerateRoofMeshes) so the wall and the dome above it stay in sync.
                if (Building.RoofShape == ERoofShape::Onion || Building.RoofShape == ERoofShape::Dome)
                {
                        float Eave = DomeNestleEave(Building, Buildings, ScaleFactor, WallTop, EffBase);
                        if (Eave > 0.f)
                                WallTop = Eave;
                }

                // Recompute level height based on actual wall extent
                float ActualWallH = WallTop - EffBase;
                if (ActualWallH > 1.f)
                {
                        NumLevels = FMath::Max(1, NumLevels);
                        LevelH = ActualWallH / (float)NumLevels;
                }

                // Determine facade material from FacadeMaterialType
                // 5=glass → OutGlassData (streets-gl curtain facade); wood→brick section + wood UV1.
                const bool bGlassFacade = (!bColorOnly && Building.FacadeMaterialType == 5);
                FMeshData& FacadeData = bColorOnly ? OutColorWallData
                        : (Building.FacadeMaterialType == 1) ? OutBrickData
                        : (Building.FacadeMaterialType == 2) ? OutDarkStoneData
                        : (Building.FacadeMaterialType == 3) ? OutLightStoneData
                        : (Building.FacadeMaterialType == 4) ? OutBrickData
                        : bGlassFacade ? OutGlassData
                        : OutWallData;
                // Use the OSM building:colour (Building.WallColor) as the vertex
                // colour for the whole facade, so the vertex-color material shows
                // the real per-building colour (streets.gl style).
                FColor FacadeColor = bGlassFacade ? GlassColor : LinearToFColor(Building.WallColor);

                // Glass curtain-wall treatment is only appropriate for modern
                // commercial high-rises. Gate it on building type so that tall
                // HISTORIC towers (churches, cathedrals like the Frauenkirche,
                // BuildingType 0/2/3) are NOT rendered as glass skyscrapers.
                // (BuildingType: 0=residential, 1=commercial, 2=industrial, 3=other)
                // Explicit building:material=glass always uses the glass path above.
                bool bTall = (NumLevels >= 21) && (Building.BuildingType == 1) && !bGlassFacade;

                if (bTall)
                {
                        for (int32 Lv = 0; Lv < NumLevels; Lv++)
                        {
                                float LB = EffBase + Lv * LevelH;
                                if (Lv == NumLevels - 1)
                                {
                                        AddExtrudedPolygon(FacadeData, LocalFootprint, LB, LB + LevelH, FacadeColor, false);
                                }
                                else
                                {
                                        AddExtrudedPolygon(FacadeData, LocalFootprint, LB, LB + LevelH * 0.6f, FacadeColor, false);
                                        AddExtrudedPolygon(OutGlassData, LocalFootprint, LB + LevelH * 0.6f, LB + LevelH * 0.95f, GlassColor, false);
                                        AddExtrudedPolygon(OutWallData, LocalFootprint, LB + LevelH * 0.95f, LB + LevelH, WallColor, false);
                                }
                        }
                }
                else
                {
                        // ----------------------------------------------------------------
                        // streets.gl-style wall + window system
                        // ----------------------------------------------------------------
                        // Key insight from streets.gl source (WallsBuilder.ts):
                        //   - windowWidth = 4 m for ALL facade materials
                        //   - windowCount = round(edgeLength / 4)
                        //   - The ENTIRE wall face uses either textureIdWindow or textureIdWall
                        //   - The window texture already contains both the window glass and
                        //     the surrounding wall material — no separate window overlay needed
                        //   - UV: U goes 0..windowCount (one texture repeat per slot),
                        //         V goes 0..levels (one repeat per floor)
                        //   - If buildingWindows=false: use wall texture instead of window texture
                        // ----------------------------------------------------------------
                        const bool bHasWindows = !bGlassFacade
                                && IsBuildingHasWindows(Building)
                                && ((WallTop - EffBase) >= 200.f);
                        const bool bSkillion = (Building.RoofShape == ERoofShape::Skillion);

                        if (bHasWindows)
                        {
                                // Windowed buildings: per-edge wall quads with window texture.
                                // The window texture (FacadePlasterWindow, FacadeBrickWindow, etc.)
                                // already composites the window glass on the wall background.
                                FWindowParams WP = GetWindowParams(
                                        Building.BuildingType, NumLevels, BuildingH);

                                TArray<FVector> WinFootprint = LocalFootprint;
                                if (ComputeSignedArea2D(WinFootprint) < 0.f)
                                        Algo::Reverse(WinFootprint);

                                // Skillion roofs: compute per-vertex top heights
                                TArray<float> TopZ;
                                if (bSkillion)
                                {
                                        float SkillionRoofH = (Building.RoofHeight > 0.0)
                                                ? (float)(Building.RoofHeight * (double)ScaleFactor) : 0.f;
                                        float SkillDir = (float)Building.RoofDirection;
                                        float AttachEave = IsAdjacentToOnionDome(Building, Buildings)
                                                ? DomeNestleEave(Building, Buildings, ScaleFactor, EffBase + BuildingH, EffBase) : -1.f;
                                        if (AttachEave > 0.f)
                                                SkillionRoofH = BuildingH - AttachEave;
                                        else
                                                SkillionRoofH = GentleSkillionRise(LocalFootprint, SkillDir, SkillionRoofH);

                                        // Replicate AddSkillionExtrudedPolygon height calculation
                                        TopZ.SetNum(WinFootprint.Num());
                                        if (SkillDir >= 0.f)
                                        {
                                                float D_rad = FMath::DegreesToRadians(SkillDir);
                                                FVector2D SlopeDir(-FMath::Sin(D_rad), FMath::Cos(D_rad));
                                                float MinProj = FLT_MAX, MaxProj = -FLT_MAX;
                                                for (const FVector& V : WinFootprint)
                                                {
                                                        float Proj = V.X * SlopeDir.X + V.Y * SlopeDir.Y;
                                                        MinProj = FMath::Min(MinProj, Proj);
                                                        MaxProj = FMath::Max(MaxProj, Proj);
                                                }
                                                float Range = FMath::Max(MaxProj - MinProj, 1.f);
                                                for (int32 i = 0; i < WinFootprint.Num(); i++)
                                                {
                                                        float Proj = WinFootprint[i].X * SlopeDir.X + WinFootprint[i].Y * SlopeDir.Y;
                                                        float T = (Proj - MinProj) / Range;
                                                        TopZ[i] = WallTop + FMath::Lerp(0.f, SkillionRoofH, T);
                                                }
                                        }
                                        else
                                        {
                                                for (int32 i = 0; i < WinFootprint.Num(); i++)
                                                        TopZ[i] = WallTop;
                                        }
                                }

                                // Emit per-edge wall quads with window UV mapping
                                for (int32 i = 0; i < WinFootprint.Num(); i++)
                                {
                                        int32 Next = (i + 1) % WinFootprint.Num();
                                        FVector P0(WinFootprint[i].X, WinFootprint[i].Y, 0.f);
                                        FVector P1(WinFootprint[Next].X, WinFootprint[Next].Y, 0.f);
                                        FVector ED = P1 - P0; float EL = ED.Size2D(); if (EL < 1.f) continue;

                                        // Window count for this edge (streets.gl: round(length / windowWidth))
                                        float EdgeLengthM = EL / 100.f;
                                        float WindowSlotM = WP.WindowSlotWidth / 100.f;
                                        int32 WindowCount = FMath::Max(1, FMath::RoundToInt(EdgeLengthM / WindowSlotM));

                                        // UV mapping (streets.gl style):
                                        // U: 0 → WindowCount (one window texture repeat per slot)
                                        // V: 0 → NumLevels (one floor texture repeat per level)
                                        float U0 = 0.f, U1 = (float)WindowCount;
                                        float VBot = 0.f, VTop = (float)NumLevels;

                                        if (bSkillion)
                                        {
                                                // Per-vertex top heights for skillion
                                                FVector BL(P0.X, P0.Y, EffBase);
                                                FVector BR(P1.X, P1.Y, EffBase);
                                                FVector TR(P1.X, P1.Y, TopZ[Next]);
                                                FVector TL(P0.X, P0.Y, TopZ[i]);
                                                // For skillion, V varies per vertex based on height
                                                float V0i = (TopZ[i] - EffBase) / (WallTop - EffBase) * (float)NumLevels;
                                                float V0n = (TopZ[Next] - EffBase) / (WallTop - EffBase) * (float)NumLevels;
                                                // Custom UV with per-vertex V
                                                int32 BI = OutWindowData.Vertices.Num();
                                                OutWindowData.Vertices.Add(BL); OutWindowData.Vertices.Add(BR);
                                                OutWindowData.Vertices.Add(TR); OutWindowData.Vertices.Add(TL);
                                                OutWindowData.Triangles.Add(BI + 0); OutWindowData.Triangles.Add(BI + 2); OutWindowData.Triangles.Add(BI + 1);
                                                OutWindowData.Triangles.Add(BI + 0); OutWindowData.Triangles.Add(BI + 3); OutWindowData.Triangles.Add(BI + 2);
                                                ED.Normalize();
                                                FVector OutN(ED.Y, -ED.X, 0.f);
                                                FVector Tang = ED; if (!Tang.IsNearlyZero()) Tang.Normalize(); else Tang = FVector(1, 0, 0);
                                                OutWindowData.UV0.Add(FVector2D(U0, VBot)); OutWindowData.UV0.Add(FVector2D(U1, VBot));
                                                OutWindowData.UV0.Add(FVector2D(U1, V0n)); OutWindowData.UV0.Add(FVector2D(U0, V0i));
                                                for (int32 v = 0; v < 4; v++)
                                                {
                                                        OutWindowData.Normals.Add(OutN);
                                                        OutWindowData.VertexColors.Add(FacadeColor);
                                                        OutWindowData.Tangents.Add(FProcMeshTangent(Tang, false));
                                                }
                                        }
                                        else
                                        {
                                                FVector BL(P0.X, P0.Y, EffBase);
                                                FVector BR(P1.X, P1.Y, EffBase);
                                                FVector TR(P1.X, P1.Y, WallTop);
                                                FVector TL(P0.X, P0.Y, WallTop);
                                                AddOutwardWallQuad(OutWindowData, BL, BR, TR, TL, FacadeColor,
                                                        U0, U1, VBot, VTop);
                                        }
                                }

                                // Inner courtyard walls (holes) with window texture
                                for (const TArray<FVector2D>& Hole : Building.Holes)
                                {
                                        if (Hole.Num() < 3) continue;
                                        TArray<FVector> HoleFP;
                                        HoleFP.Reserve(Hole.Num());
                                        for (const FVector2D& H : Hole)
                                                HoleFP.Add(FVector(H.X * ScaleFactor, -H.Y * ScaleFactor, 0.f));
                                        // Reverse hole footprint so normals face inward
                                        if (ComputeSignedArea2D(HoleFP) > 0.f)
                                                Algo::Reverse(HoleFP);
                                        for (int32 i = 0; i < HoleFP.Num(); i++)
                                        {
                                                int32 Next = (i + 1) % HoleFP.Num();
                                                FVector P0(HoleFP[i].X, HoleFP[i].Y, 0.f);
                                                FVector P1(HoleFP[Next].X, HoleFP[Next].Y, 0.f);
                                                float EL = FVector2D::Distance(FVector2D(P0.X, P0.Y), FVector2D(P1.X, P1.Y));
                                                if (EL < 1.f) continue;
                                                float EdgeLengthM = EL / 100.f;
                                                float WindowSlotM = WP.WindowSlotWidth / 100.f;
                                                int32 WindowCount = FMath::Max(1, FMath::RoundToInt(EdgeLengthM / WindowSlotM));
                                                FVector BL(P0.X, P0.Y, EffBase);
                                                FVector BR(P1.X, P1.Y, EffBase);
                                                FVector TR(P1.X, P1.Y, WallTop);
                                                FVector TL(P0.X, P0.Y, WallTop);
                                                AddOutwardWallQuad(OutWindowData, BL, BR, TR, TL, FacadeColor,
                                                        0.f, (float)WindowCount, 0.f, (float)NumLevels);
                                        }
                                }
                        }
                        else
                        {
                                // Non-windowed buildings: simple extruded polygon with wall texture
                                // (garages, greenhouses, silos, churches, towers, etc.)
                                if (bSkillion)
                                {
                                        float SkillionRoofH = (Building.RoofHeight > 0.0)
                                                ? (float)(Building.RoofHeight * (double)ScaleFactor) : 0.f;
                                        float SkillDir = (float)Building.RoofDirection;
                                        float AttachEave = IsAdjacentToOnionDome(Building, Buildings)
                                                ? DomeNestleEave(Building, Buildings, ScaleFactor, EffBase + BuildingH, EffBase) : -1.f;
                                        if (AttachEave > 0.f)
                                                SkillionRoofH = BuildingH - AttachEave;
                                        else
                                                SkillionRoofH = GentleSkillionRise(LocalFootprint, SkillDir, SkillionRoofH);
                                        AddSkillionExtrudedPolygon(FacadeData, LocalFootprint,
                                                EffBase, WallTop, SkillionRoofH, SkillDir, FacadeColor);
                                }
                                else
                                {
                                        AddExtrudedPolygon(FacadeData, LocalFootprint, EffBase, WallTop, FacadeColor, false);
                                }

                                // Inner courtyard walls
                                for (const TArray<FVector2D>& Hole : Building.Holes)
                                {
                                        if (Hole.Num() < 3) continue;
                                        TArray<FVector> HoleFP;
                                        HoleFP.Reserve(Hole.Num());
                                        for (const FVector2D& H : Hole)
                                                HoleFP.Add(FVector(H.X * ScaleFactor, -H.Y * ScaleFactor, 0.f));
                                        AddExtrudedPolygon(FacadeData, HoleFP, EffBase, WallTop, FacadeColor, false);
                                }
                        }
                }

                // Stamp the chosen facade atlas slice into UV1 for this building.
                // WallSlice  → solid wall panel  (FacadeBrickWall, FacadePlasterWall, …)
                // WindowSlice→ window cut-out panel that matches the wall material
                //              (FacadeBrickWindow, FacadePlasterWindow, …)
                // FacadeGlass→ full glass-curtain panel for tall commercial towers
                const float WallSlice   = FacadeWallSlice(Building.FacadeMaterialType);
                const float WindowSlice = FacadeWindowSlice(Building.FacadeMaterialType);
                FillSliceUV1(OutWallData,       UV1_Wall,   WallSlice);
                FillSliceUV1(OutBrickData,      UV1_Brick,  WallSlice);
                FillSliceUV1(OutDarkStoneData,  UV1_Dark,   WallSlice);
                FillSliceUV1(OutLightStoneData, UV1_Light,  WallSlice);
                FillSliceUV1(OutColorWallData,  UV1_Color,  WallSlice);
                FillSliceUV1(OutGlassData,      UV1_Glass,  (float)HiroGeoAtlas::FacadeGlass);
                FillSliceUV1(OutWindowData,     UV1_Window, WindowSlice);
                FillSliceUV1(OutWindowFrameData,UV1_Frame,  WallSlice);    // frame uses wall texture
                FillSliceUV1(OutWindowGlassData,UV1_WGlass, (float)HiroGeoAtlas::FacadeGlass); // glass pane

                // Sit this building's whole foundation/walls/windows on the terrain
                // height sampled under its footprint — see ComputeBuildingTerrainOffsets.
                ShiftZRange(OutWallData,        UV1_Wall,   ZOff);
                ShiftZRange(OutBrickData,       UV1_Brick,  ZOff);
                ShiftZRange(OutDarkStoneData,   UV1_Dark,   ZOff);
                ShiftZRange(OutLightStoneData,  UV1_Light,  ZOff);
                ShiftZRange(OutColorWallData,   UV1_Color,  ZOff);
                ShiftZRange(OutGlassData,       UV1_Glass,  ZOff);
                ShiftZRange(OutWindowData,      UV1_Window, ZOff);
                ShiftZRange(OutWindowFrameData, UV1_Frame,  ZOff);
                ShiftZRange(OutWindowGlassData, UV1_WGlass, ZOff);
        }
        // NOTE: glass normals are already baked outward by AddOutwardWallQuad and
        // AddExtrudedPolygon. Do NOT run RecomputeNormals here — it flips face
        // normals (it exists for roofs) and would light the glass from the inside.
}

// ============================================================================
// GenerateRoadMeshes
// ============================================================================
void FGeoMeshGenerator::GenerateRoadMeshes(
        const TArray<FGeoRoad>& Roads,
        FMeshData& OutRoadData, FMeshData& OutSidewalkData,
        const FHiroGeoHeightmap* DemHeightmap, double OriginLatitude, double OriginLongitude,
        const TArray<FElevationSample>* ElevationSamples)
{
        OutRoadData.Reset(); OutSidewalkData.Reset();
        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const bool bHaveSamples = ElevationSamples && ElevationSamples->Num() >= 3;
        const bool bDeform = bHaveDem || bHaveSamples;
        const double RoadRefElev = bHaveDem ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : (bHaveSamples ? ComputeElevationSamplesRefElev(*ElevationSamples) : 0.0);
        const FColor RoadColor(60, 60, 60), SidewalkColor(180, 175, 170);
        // Layer the surfaces at distinct, small heights above the ground so the
        // road, sidewalk and curb never sit coplanar with the ground or with
        // each other (the old coplanar stack at Z~0 caused the z-fighting
        // "messy lines"). Mirrors how streets.gl draws roads as a flat ribbon
        // just above terrain.
        const float SidewalkZ = 12.f;   // sidewalk top
        const float RoadZ     = 4.f;    // road surface
        const float CurbZ     = SidewalkZ; // curb rises from road to sidewalk

        // Emit a flat road quad with proper along/across UVs (U across the width
        // 0..1, V along the length) and a texture-atlas slice in UV1.x. Normals
        // are recomputed afterwards, tangent runs along the road.
        auto AddRoadQuad = [](FMeshData& MD,
                const FVector& V0, const FVector& V1, const FVector& V2, const FVector& V3,
                const FColor& Col,
                const FVector2D& T0, const FVector2D& T1, const FVector2D& T2, const FVector2D& T3,
                float Slice)
        {
                const int32 BI = MD.Vertices.Num();
                MD.Vertices.Add(V0); MD.Vertices.Add(V1); MD.Vertices.Add(V2); MD.Vertices.Add(V3);
                MD.Triangles.Add(BI + 0); MD.Triangles.Add(BI + 1); MD.Triangles.Add(BI + 2);
                MD.Triangles.Add(BI + 0); MD.Triangles.Add(BI + 2); MD.Triangles.Add(BI + 3);
                FVector Tang = (V3 - V0).GetSafeNormal();
                if (Tang.IsNearlyZero()) Tang = FVector(1, 0, 0);
                MD.UV0.Add(T0); MD.UV0.Add(T1); MD.UV0.Add(T2); MD.UV0.Add(T3);
                for (int32 k = 0; k < 4; k++)
                {
                        MD.Normals.Add(FVector(0, 0, 1));
                        MD.VertexColors.Add(Col);
                        MD.Tangents.Add(FProcMeshTangent(Tang, false));
                        MD.UV1.Add(FVector2D(Slice, 0.f));
                }
        };

        struct FJunctionInfo
        {
                float MaxHW = 0.f;
                TArray<TPair<FVector2D, float>> Arms;
                TArray<FVector> Mouths;
        };
        TMap<FIntVector, FJunctionInfo> Junctions;
        auto NeedsJunctionSurface = [](const FJunctionInfo& Junction)
        {
                if (Junction.Arms.Num() != 2)
                        return Junction.Arms.Num() > 2;

                const auto& First = Junction.Arms[0];
                const auto& Second = Junction.Arms[1];
                return FVector2D::DotProduct(First.Key, Second.Key) > -0.995
                        || !FMath::IsNearlyEqual(First.Value, Second.Value, 1.f);
        };
        const float JunctionSnap = 25.f; // UE units (~25cm) endpoint-grouping tolerance
        auto JKey = [JunctionSnap](const FVector2D& P, int32 Layer) -> FIntVector
        {
                return FIntVector(FMath::RoundToInt(P.X / JunctionSnap), FMath::RoundToInt(P.Y / JunctionSnap), Layer);
        };
        auto IsVehicleRoad = [](const FString& HWType)
        {
                return !(
                        HWType == TEXT("footway")   || HWType == TEXT("path")    ||
                        HWType == TEXT("cycleway")  || HWType == TEXT("steps")   ||
                        HWType == TEXT("pedestrian")|| HWType == TEXT("track")   ||
                        HWType == TEXT("bridleway") || HWType == TEXT("corridor")||
                        HWType == TEXT("platform")  || HWType == TEXT("construction") ||
                        HWType == TEXT("proposed")  || HWType == TEXT("runway") ||
                        HWType == TEXT("taxiway")   || HWType.IsEmpty());
        };

        // Split only at original shared vertices, before adding terrain samples.
        // Interior bends on a single way retain their continuous ribbon.
        TMap<FIntVector, int32> VertexUses;
        auto RoadKey = [&](const FVector2D& Point, int32 Layer)
        {
                return JKey(FVector2D(Point.X * ScaleFactor, -Point.Y * ScaleFactor), Layer);
        };
        for (const FGeoRoad& Road : Roads)
        {
                if (Road.Points.Num() < 2 || !IsVehicleRoad(Road.RoadTypeStr.ToLower())) continue;
                for (const FVector2D& Point : Road.Points)
                        ++VertexUses.FindOrAdd(RoadKey(Point, Road.Layer));
        }

        TArray<FGeoRoad> Sections;
        for (const FGeoRoad& Road : Roads)
        {
                if (Road.Points.Num() < 2) continue;
                if (!IsVehicleRoad(Road.RoadTypeStr.ToLower()) || Road.Layer != 0)
                {
                        Sections.Add(Road);
                        continue;
                }

                int32 Start = 0;
                for (int32 End = 1; End < Road.Points.Num(); ++End)
                {
                        if (End != Road.Points.Num() - 1 && VertexUses.FindRef(RoadKey(Road.Points[End], Road.Layer)) < 2)
                                continue;
                        FGeoRoad& Section = Sections.Add_GetRef(Road);
                        Section.Points.Reset(End - Start + 1);
                        Section.Points.Append(Road.Points.GetData() + Start, End - Start + 1);
                        Start = End;
                }
        }

        for (const FGeoRoad& Road : Sections)
        {
                const TArray<FVector2D> RPts = ResamplePolylineForTerrain(Road.Points, 5.f, false);
                const int32 N = RPts.Num();
                if (N < 2) continue;
                const bool bVehicleRoad = IsVehicleRoad(Road.RoadTypeStr.ToLower());
                if (!bVehicleRoad) continue;

                const float RW = Road.Width > 0.f ? Road.Width * ScaleFactor : 600.f;
                const float HW = RW * 0.5f;
                TArray<FVector2D> Pts;
                Pts.Reserve(N);
                for (int32 i = 0; i < N; i++)
                        Pts.Add(FVector2D(RPts[i].X * ScaleFactor, -RPts[i].Y * ScaleFactor));

                auto AddArm = [&](const FVector2D& At, const FVector2D& Into)
                {
                        const FVector2D Dir = (Into - At).GetSafeNormal();
                        if (Dir.IsNearlyZero()) return;
                        FJunctionInfo& J = Junctions.FindOrAdd(JKey(At, Road.Layer));
                        J.MaxHW = FMath::Max(J.MaxHW, HW);
                        J.Arms.Emplace(Dir, HW);
                };
                AddArm(Pts[0], Pts[1]);
                AddArm(Pts[N - 1], Pts[N - 2]);
        }

        for (const FGeoRoad& Road : Sections)
        {
                const TArray<FVector2D> RPts = ResamplePolylineForTerrain(Road.Points, 5.f, /*bClosed*/ false);
                const int32 N = RPts.Num();
                if (N < 2) continue;

                const float RW = Road.Width > 0.f ? Road.Width * ScaleFactor : 600.f;
                const float HW = RW * 0.5f;          // half road width

                // Pedestrian ways (footway/path/cycleway/steps/pedestrian/...) are not
                // vehicle roads: no flanking sidewalk, drawn as pavement, and laid a
                // bit LOWER than vehicle roads so a footway crossing a road is covered
                // by the road instead of cutting a light strip across the asphalt.
                const FString HWType = Road.RoadTypeStr.ToLower();
                const bool bVehicleRoad = IsVehicleRoad(HWType);

                // streets-gl: sidewalk ribbons ONLY when sidewalk=* is tagged.
                // Motorways/trunk never get pedestrian sidewalks even if mistagged.
                const bool bMotorNoWalk =
                        HWType == TEXT("motorway") || HWType == TEXT("motorway_link") ||
                        HWType == TEXT("trunk")    || HWType == TEXT("trunk_link")    ||
                        HWType == TEXT("raceway")  || HWType == TEXT("runway") || HWType == TEXT("taxiway");
                const ERoadSide SWSide = (bVehicleRoad && !bMotorNoWalk) ? Road.SidewalkSide : ERoadSide::None;
                const ERoadSide CWSide = (bVehicleRoad && !bMotorNoWalk) ? Road.CyclewaySide : ERoadSide::None;
                const bool bSWLeft  = (SWSide == ERoadSide::Left  || SWSide == ERoadSide::Both);
                const bool bSWRight = (SWSide == ERoadSide::Right || SWSide == ERoadSide::Both);
                const bool bCWLeft  = (CWSide == ERoadSide::Left  || CWSide == ERoadSide::Both);
                const bool bCWRight = (CWSide == ERoadSide::Right || CWSide == ERoadSide::Both);

                if (Road.Width >= 10.f)
                {
                        UE_LOG(LogTemp, Log, TEXT("HiroGeo[ZDiag] Road name=\"%s\" highway=%s width=%.1fm layer=%d sidewalk=%d cycleway=%d"),
                                *Road.Name, *Road.RoadTypeStr, Road.Width, Road.Layer, (int32)SWSide, (int32)CWSide);
                }
                const float SurfZ = bVehicleRoad ? RoadZ : (RoadZ - 2.5f);

                // streets-gl side-ribbon widths (meters → UE via ScaleFactor).
                const float CyclewayWidthM = 2.f;
                const float SidewalkWidthM = 2.f;
                const float CWExtra = CyclewayWidthM * ScaleFactor;
                const float SWExtra = SidewalkWidthM * ScaleFactor;
                // Cycleway outer half-width = road half + cycleway strip (streets-gl).
                const float OH_CW = HW + CWExtra;

                // Pre-convert points to UE 2D space.
                TArray<FVector2D> Pts;
                Pts.Reserve(N);
                for (int32 i = 0; i < N; i++)
                        Pts.Add(FVector2D(RPts[i].X * ScaleFactor, -RPts[i].Y * ScaleFactor));

                // Per-point terrain Z offset (UE units) — sampled individually so the
                // road/sidewalk ribbon follows rising/falling terrain along its length
                // instead of sitting on one flat plane. All zero when no DEM is set.
                TArray<float> TZ;
                TZ.SetNumZeroed(N);
                if (bDeform && !SampleGroundZ)
                {
                        for (int32 i = 0; i < N; i++)
                        {
                                TZ[i] = bHaveDem
                                        ? SampleDemOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude,
                                                RoadRefElev, ScaleFactor, RPts[i].X, RPts[i].Y)
                                        : SampleElevationSamplesOffsetUE(*ElevationSamples, RoadRefElev,
                                                ScaleFactor, RPts[i].X, RPts[i].Y);
                        }
                }
                // Grade separation: streets.gl uses z-index (draw order), not metres.
                // UE still needs a small real Z delta so overlapping grade-separated
                // meshes don't Z-fight. 1.0 m/layer is enough for separation; with DEM
                // the real bridge clearance comes from terrain. Abrupt full-height
                // jumps at bridge ends looked like steep ramps over flat water, so
                // ease layer height in/out over ~25 m at each end of the way.
                const float LayerStepZ = 100.f; // UE units = 1.0 m (was 3.0 m)
                const float LayerRampDist = 2500.f; // 25 m ease at each end
                TArray<float> AlongDist;
                AlongDist.SetNum(N);
                AlongDist[0] = 0.f;
                for (int32 i = 1; i < N; i++)
                        AlongDist[i] = AlongDist[i - 1] + FVector2D::Distance(Pts[i - 1], Pts[i]);
                const float TotalLen = AlongDist[N - 1];
                if (Road.Layer != 0)
                {
                        for (int32 i = 0; i < N; i++)
                        {
                                float Fade = 1.f;
                                if (TotalLen > 1.f)
                                {
                                        if (TotalLen > LayerRampDist * 2.f)
                                        {
                                                const float FromStart = AlongDist[i];
                                                const float FromEnd = TotalLen - AlongDist[i];
                                                if (FromStart < LayerRampDist)
                                                        Fade = FromStart / LayerRampDist;
                                                else if (FromEnd < LayerRampDist)
                                                        Fade = FromEnd / LayerRampDist;
                                        }
                                        else
                                        {
                                                // Short bridge/way: smooth hump so ends meet approaches.
                                                Fade = FMath::Sin(PI * AlongDist[i] / TotalLen);
                                        }
                                        Fade = Fade * Fade * (3.f - 2.f * Fade); // smoothstep
                                }
                                TZ[i] += (float)Road.Layer * LayerStepZ * Fade;
                        }
                }

                // streets-gl RoadBuilder-style joins: each segment uses its own edge
                // perpendicular (no shared exploding miter). Interior vertices get a
                // bevel / miter fan so ribbons stay constant-width without overlapping
                // rectangular strips at bends.
                auto EdgeNormal = [](const FVector2D& A, const FVector2D& B) -> FVector2D
                {
                        FVector2D D = (B - A).GetSafeNormal();
                        return FVector2D(D.Y, -D.X); // right-hand perpendicular
                };

                // streets-gl RoadGraph: pull ribbon ends back into multi-arm junctions
                // so lane markings stop at the intersection polygon (trimmedEnd).
                FJunctionInfo* StartJunction = nullptr;
                FJunctionInfo* EndJunction = nullptr;
                auto TrimEnd = [&](int32 EndIdx, int32 TowardIdx, FJunctionInfo*& Junction)
                {
                        const FIntVector K = JKey(Pts[EndIdx], Road.Layer);
                        FJunctionInfo* J = Junctions.Find(K);
                        if (!J || !NeedsJunctionSurface(*J)) return;
                        Junction = J;
                        const float Want = J->MaxHW;
                        FVector2D Delta = Pts[TowardIdx] - Pts[EndIdx];
                        const float Len = Delta.Size();
                        if (Len < 50.f) return;
                        const float Dist = FMath::Min(Want, Len * 0.45f);
                        const float T = Dist / Len;
                        Pts[EndIdx] = FMath::Lerp(Pts[EndIdx], Pts[TowardIdx], T);
                        TZ[EndIdx] = FMath::Lerp(TZ[EndIdx], TZ[TowardIdx], T);
                        AlongDist[EndIdx] = FMath::Lerp(AlongDist[EndIdx], AlongDist[TowardIdx], T);
                };
                if (bVehicleRoad)
                {
                        TrimEnd(0, 1, StartJunction);
                        TrimEnd(N - 1, N - 2, EndJunction);
                }

                // Cumulative V along the road. Use a FIXED real-world tile length
                // (~12 m) instead of one-tile-per-road-width, so lane markings keep a
                // consistent, less-dense scale on every road regardless of width.
                const float RoadTileLen = 1200.f; // 12 m per texture tile
                TArray<float> VAlong; VAlong.SetNum(N); VAlong[0] = 0.f;
                for (int32 i = 1; i < N; i++)
                        VAlong[i] = VAlong[i - 1] + FVector2D::Distance(Pts[i - 1], Pts[i]) / RoadTileLen;

                // Texture slice: 0 = marked road (primary/secondary/tertiary/highway),
                // 1 = unmarked road (residential/service/...), 2 = pavement (footways).
                const bool bMarked = Road.bIsHighway ||
                        Road.RoadType == 1 || Road.RoadType == 2 || Road.RoadType == 3;
                const float RoadSlice = !bVehicleRoad ? 2.f : (bMarked ? 0.f : 1.f);
                const FColor CycleColor(40, 40, 45);
                const float CycleZ = SurfZ + 1.f;

                auto OffsetAt = [](const FVector2D& P, const FVector2D& Nrm, float Half) -> TPair<FVector2D, FVector2D>
                {
                        return TPair<FVector2D, FVector2D>(P + Nrm * Half, P - Nrm * Half);
                };

                auto RoadVertex = [&](const FVector2D& P, float Z)
                {
                        return FVector(P.X, P.Y, SampleGroundZ
                                ? SampleGroundZ(P) + 5.0 + Z : Z);
                };

                auto AddMouth = [&](FJunctionInfo* Junction, int32 End, int32 Neighbor)
                {
                        if (!Junction) return;
                        const FVector2D Normal = EdgeNormal(Pts[End], Pts[Neighbor]);
                        Junction->Mouths.Add(RoadVertex(Pts[End] + Normal * HW, SurfZ + TZ[End]));
                        Junction->Mouths.Add(RoadVertex(Pts[End] - Normal * HW, SurfZ + TZ[End]));
                };
                AddMouth(StartJunction, 0, 1);
                AddMouth(EndJunction, N - 1, N - 2);

                auto EmitRibbonSeg = [&](FMeshData& MD, const FVector2D& L0, const FVector2D& R0,
                        const FVector2D& L1, const FVector2D& R1, float Z0, float Z1,
                        float V0, float V1, const FColor& Col, float Slice)
                {
                        AddRoadQuad(MD,
                                RoadVertex(R0, Z0), RoadVertex(L0, Z0),
                                RoadVertex(L1, Z1), RoadVertex(R1, Z1),
                                Col,
                                FVector2D(0.f, V0), FVector2D(1.f, V0),
                                FVector2D(1.f, V1), FVector2D(0.f, V1),
                                Slice);
                };

                // The inside edges already overlap at a bend. Fill only the
                // outside wedge so the join does not cross the two ribbons.
                auto EmitJoin = [&](const FVector2D& P, const FVector2D& NPrev,
                        const FVector2D& NNext, float Z, float V)
                {
                        const double Turn = FVector2D::CrossProduct(NPrev, NNext);
                        if (FMath::Abs(Turn) <= UE_DOUBLE_SMALL_NUMBER) return;
                        const double Side = Turn > 0.0 ? 1.0 : -1.0;
                        FVector2D A = P + NPrev * (HW * Side);
                        FVector2D B = P + NNext * (HW * Side);
                        if (Turn < 0.0) Swap(A, B);

                        const int32 Start = OutRoadData.Vertices.Num();
                        AddTriangle(OutRoadData, RoadVertex(P, Z), RoadVertex(A, Z),
                                RoadVertex(B, Z), RoadColor);
                        OutRoadData.UV0[Start] = FVector2D(0.5, V);
                        const double EdgeU = Turn > 0.0 ? 1.0 : 0.0;
                        OutRoadData.UV0[Start + 1] = FVector2D(EdgeU, V);
                        OutRoadData.UV0[Start + 2] = FVector2D(EdgeU, V + FVector2D::Distance(A, B) / RoadTileLen);
                        for (int32 Corner = 0; Corner < 3; ++Corner)
                                OutRoadData.UV1.Emplace(bVehicleRoad ? 1.f : RoadSlice, 0.f);
                        const bool bLeft = Side > 0.0;
                        const bool bCycleway = bLeft ? bCWLeft : bCWRight;
                        const bool bSidewalk = bLeft ? bSWLeft : bSWRight;
                        auto JoinEdges = [&](float Inner, float Outer)
                        {
                                const FVector2D First = Turn > 0.0 ? NPrev : NNext;
                                const FVector2D Second = Turn > 0.0 ? NNext : NPrev;
                                return TArray<FVector2D, TInlineAllocator<4>>{
                                        P + First * (Inner * Side), P + First * (Outer * Side),
                                        P + Second * (Outer * Side), P + Second * (Inner * Side)};
                        };
                        const float TerrainZ = Z - SurfZ;
                        if (bCycleway)
                        {
                                const auto E = JoinEdges(HW, OH_CW);
                                const float Top = CycleZ + TerrainZ;
                                AddRoadQuad(OutRoadData, RoadVertex(E[0], Top), RoadVertex(E[1], Top),
                                        RoadVertex(E[2], Top), RoadVertex(E[3], Top),
                                        CycleColor, {0.f, V}, {1.f, V}, {1.f, V + 0.1f}, {0.f, V + 0.1f}, 2.f);
                        }
                        if (bSidewalk)
                        {
                                const float Inner = bCycleway ? OH_CW : HW;
                                const auto E = JoinEdges(Inner, Inner + SWExtra);
                                const float Top = SidewalkZ + TerrainZ;
                                AddQuad(OutSidewalkData, RoadVertex(E[0], Top), RoadVertex(E[1], Top),
                                        RoadVertex(E[2], Top), RoadVertex(E[3], Top), SidewalkColor);
                                AddQuad(OutSidewalkData, RoadVertex(E[0], RoadZ + TerrainZ),
                                        RoadVertex(E[0], Top), RoadVertex(E[3], Top),
                                        RoadVertex(E[3], RoadZ + TerrainZ), SidewalkColor);
                        }
                };

                for (int32 i = 0; i < N - 1; i++)
                {
                        const FVector2D Nrm = EdgeNormal(Pts[i], Pts[i + 1]);
                        if (Nrm.IsNearlyZero()) continue;

                        const auto Road0 = OffsetAt(Pts[i], Nrm, HW);
                        const auto Road1 = OffsetAt(Pts[i + 1], Nrm, HW);
                        const float Z0 = SurfZ + TZ[i];
                        const float Z1 = SurfZ + TZ[i + 1];

                        EmitRibbonSeg(OutRoadData, Road0.Key, Road0.Value, Road1.Key, Road1.Value,
                                Z0, Z1, VAlong[i], VAlong[i + 1], RoadColor, RoadSlice);

                        if (bCWLeft)
                        {
                                const auto A0 = OffsetAt(Pts[i], Nrm, OH_CW);
                                const auto A1 = OffsetAt(Pts[i + 1], Nrm, OH_CW);
                                EmitRibbonSeg(OutRoadData, A0.Key, Road0.Key, A1.Key, Road1.Key,
                                        CycleZ + TZ[i], CycleZ + TZ[i + 1], VAlong[i], VAlong[i + 1], CycleColor, 2.f);
                        }
                        if (bCWRight)
                        {
                                const auto A0 = OffsetAt(Pts[i], Nrm, OH_CW);
                                const auto A1 = OffsetAt(Pts[i + 1], Nrm, OH_CW);
                                EmitRibbonSeg(OutRoadData, Road0.Value, A0.Value, Road1.Value, A1.Value,
                                        CycleZ + TZ[i], CycleZ + TZ[i + 1], VAlong[i], VAlong[i + 1], CycleColor, 2.f);
                        }

                        const float OH_L = HW + SWExtra + (bCWLeft  ? CWExtra : 0.f);
                        const float OH_R = HW + SWExtra + (bCWRight ? CWExtra : 0.f);
                        if (bSWLeft)
                        {
                                const float InnerHalf = bCWLeft ? OH_CW : HW;
                                const auto Inner0 = OffsetAt(Pts[i], Nrm, InnerHalf);
                                const auto Inner1 = OffsetAt(Pts[i + 1], Nrm, InnerHalf);
                                const auto Outer0 = OffsetAt(Pts[i], Nrm, OH_L);
                                const auto Outer1 = OffsetAt(Pts[i + 1], Nrm, OH_L);
                                AddQuad(OutSidewalkData,
                                        RoadVertex(Inner0.Key, SidewalkZ + TZ[i]),
                                        RoadVertex(Outer0.Key, SidewalkZ + TZ[i]),
                                        RoadVertex(Outer1.Key, SidewalkZ + TZ[i + 1]),
                                        RoadVertex(Inner1.Key, SidewalkZ + TZ[i + 1]),
                                        SidewalkColor);
                                AddQuad(OutSidewalkData,
                                        RoadVertex(Inner0.Key, RoadZ + TZ[i]),
                                        RoadVertex(Inner0.Key, CurbZ + TZ[i]),
                                        RoadVertex(Inner1.Key, CurbZ + TZ[i + 1]),
                                        RoadVertex(Inner1.Key, RoadZ + TZ[i + 1]),
                                        SidewalkColor);
                        }
                        if (bSWRight)
                        {
                                const float InnerHalf = bCWRight ? OH_CW : HW;
                                const auto Inner0 = OffsetAt(Pts[i], Nrm, InnerHalf);
                                const auto Inner1 = OffsetAt(Pts[i + 1], Nrm, InnerHalf);
                                const auto Outer0 = OffsetAt(Pts[i], Nrm, OH_R);
                                const auto Outer1 = OffsetAt(Pts[i + 1], Nrm, OH_R);
                                AddQuad(OutSidewalkData,
                                        RoadVertex(Outer0.Value, SidewalkZ + TZ[i]),
                                        RoadVertex(Inner0.Value, SidewalkZ + TZ[i]),
                                        RoadVertex(Inner1.Value, SidewalkZ + TZ[i + 1]),
                                        RoadVertex(Outer1.Value, SidewalkZ + TZ[i + 1]),
                                        SidewalkColor);
                                AddQuad(OutSidewalkData,
                                        RoadVertex(Inner0.Value, CurbZ + TZ[i]),
                                        RoadVertex(Inner0.Value, RoadZ + TZ[i]),
                                        RoadVertex(Inner1.Value, RoadZ + TZ[i + 1]),
                                        RoadVertex(Inner1.Value, CurbZ + TZ[i + 1]),
                                        SidewalkColor);
                        }

                        // Join previous segment to this one at vertex i (skip first).
                        if (i > 0)
                        {
                                const FVector2D NPrev = EdgeNormal(Pts[i - 1], Pts[i]);
                                if (!NPrev.IsNearlyZero() && FVector2D::DotProduct(NPrev, Nrm) < 0.999f)
                                {
                                        EmitJoin(Pts[i], NPrev, Nrm, SurfZ + TZ[i], VAlong[i]);
                                }
                        }
                }
        }

        // Build the intersection footprint from the actual trimmed ribbon edges.
        for (auto& Pair : Junctions)
        {
                auto& Mouths = Pair.Value.Mouths;
                if (Mouths.Num() < 3) continue;
                Mouths.Sort([](const FVector& A, const FVector& B)
                {
                        return A.X == B.X ? A.Y < B.Y : A.X < B.X;
                });
                TArray<FVector> Hull;
                auto AppendHullPoint = [&](const FVector& Point, int32 ChainStart)
                {
                        while (Hull.Num() >= ChainStart + 2)
                        {
                                const FVector& A = Hull[Hull.Num() - 2];
                                const FVector& B = Hull.Last();
                                if (FVector::CrossProduct(B - A, Point - B).Z > 0.0) break;
                                Hull.Pop(EAllowShrinking::No);
                        }
                        Hull.Add(Point);
                };
                for (const FVector& Point : Mouths) AppendHullPoint(Point, 0);
                const int32 UpperStart = Hull.Num() - 1;
                for (int32 Index = Mouths.Num() - 2; Index >= 0; --Index)
                        AppendHullPoint(Mouths[Index], UpperStart);
                Hull.Pop(EAllowShrinking::No);

                for (int32 Index = 1; Index + 1 < Hull.Num(); ++Index)
                {
                        AddTriangle(OutRoadData, Hull[0], Hull[Index], Hull[Index + 1], RoadColor);
                        for (int32 Corner = 0; Corner < 3; ++Corner)
                                OutRoadData.UV1.Emplace(1.f, 0.f);
                }
        }
        // Normals already point up from the CCW winding above; recompute cleanly
        // instead of blindly flipping (the old FlipNormals inverted the ribbon).
        // Sidewalks are all pavement -> surface-atlas slice 2.
        FillSliceUV1(OutSidewalkData, 0, 2.f);

        RecomputeNormals(OutRoadData);
        RecomputeNormals(OutSidewalkData);
        ProjectOntoGround(OutRoadData);
        ProjectOntoGround(OutSidewalkData);
}

// ============================================================================
// GenerateWaterMeshes
// ============================================================================
void FGeoMeshGenerator::GenerateWaterMeshes(
        const TArray<FGeoWater>& WaterBodies, FMeshData& OutWaterData,
        const FHiroGeoHeightmap* DemHeightmap, double OriginLatitude, double OriginLongitude,
        const TArray<FElevationSample>* ElevationSamples)
{
        OutWaterData.Reset();
        FColor WC(30, 100, 200);
        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const bool bHaveSamples = ElevationSamples && ElevationSamples->Num() >= 3;
        const double WaterRefElev = bHaveDem ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : (bHaveSamples ? ComputeElevationSamplesRefElev(*ElevationSamples) : 0.0);
        // Sit water slightly below adjacent ground (streets-gl z-order), not a blind
        // global -0.5 m that buries lakes under the terrain mesh on slopes.
        const float WaterSinkZ = -15.f;

        for (const FGeoWater& Water : WaterBodies)
        {
                // -------------------------------------------------------------
                // Open river/stream/canal line: build a width-buffered RIBBON,
                // not a filled polygon. Each vertex is individually sampled
                // against the DEM (like roads) so the water surface follows
                // the terrain slope along its length instead of being one
                // single flat plane cutting through hills.
                // -------------------------------------------------------------
                if (!Water.bIsClosed)
                {
                        if (Water.Points.Num() < 2) continue;
                        const TArray<FVector2D> RPts = ResamplePolylineForTerrain(Water.Points, 10.f, /*bClosed*/ false);
                        const int32 N = RPts.Num();
                        if (N < 2) continue;

                        const float HW = FMath::Max(Water.Width, 1.f) * ScaleFactor * 0.5f;

                        TArray<FVector2D> Pts;
                        Pts.Reserve(N);
                        for (int32 i = 0; i < N; i++)
                                Pts.Add(FVector2D(RPts[i].X * ScaleFactor, -RPts[i].Y * ScaleFactor));

                        TArray<float> TZ;
                        TZ.SetNumZeroed(N);
                        if (bHaveDem || bHaveSamples)
                        {
                                for (int32 i = 0; i < N; i++)
                                {
                                        TZ[i] = bHaveDem
                                                ? SampleDemOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude,
                                                        WaterRefElev, ScaleFactor, RPts[i].X, RPts[i].Y)
                                                : SampleElevationSamplesOffsetUE(*ElevationSamples, WaterRefElev,
                                                        ScaleFactor, RPts[i].X, RPts[i].Y);
                                }
                        }

                        // Mitered per-vertex offset (same construction as road ribbons)
                        // so the constant-width strip stays parallel-sided through bends.
                        auto EdgeNormal = [](const FVector2D& A, const FVector2D& B) -> FVector2D
                        {
                                FVector2D D = (B - A).GetSafeNormal();
                                return FVector2D(D.Y, -D.X);
                        };

                        const int32 BI = OutWaterData.Vertices.Num();
                        for (int32 i = 0; i < N; i++)
                        {
                                const FVector2D nIn  = (i > 0)     ? EdgeNormal(Pts[i - 1], Pts[i]) : FVector2D::ZeroVector;
                                const FVector2D nOut = (i < N - 1) ? EdgeNormal(Pts[i], Pts[i + 1]) : FVector2D::ZeroVector;
                                const FVector2D Fallback = nIn.IsNearlyZero() ? nOut : nIn;
                                FVector2D m = (nIn.IsNearlyZero() || nOut.IsNearlyZero()) ? Fallback : (nIn + nOut).GetSafeNormal();
                                if (m.IsNearlyZero()) m = Fallback;
                                const float CosHalf = FMath::Max(FVector2D::DotProduct(m, Fallback), 0.3f);
                                const float MiterLen = HW / CosHalf;

                                const FVector2D Left  = Pts[i] + m * MiterLen;
                                const FVector2D Right = Pts[i] - m * MiterLen;
                                const double LeftZ = SampleGroundZ ? SampleGroundZ(Left) + 2.0 : TZ[i] + WaterSinkZ;
                                const double RightZ = SampleGroundZ ? SampleGroundZ(Right) + 2.0 : TZ[i] + WaterSinkZ;
                                OutWaterData.Vertices.Add(FVector(Left.X, Left.Y, LeftZ));
                                OutWaterData.Vertices.Add(FVector(Right.X, Right.Y, RightZ));
                                for (int32 k = 0; k < 2; k++)
                                {
                                        OutWaterData.Normals.Add(FVector(0.f, 0.f, 1.f));
                                        OutWaterData.VertexColors.Add(WC);
                                        OutWaterData.Tangents.Add(FProcMeshTangent(FVector(1.f, 0.f, 0.f), false));
                                        OutWaterData.UV1.Add(FVector2D::ZeroVector);
                                }
                                OutWaterData.UV0.Add(FVector2D(0.f, (float)i));
                                OutWaterData.UV0.Add(FVector2D(1.f, (float)i));
                        }
                        for (int32 i = 0; i < N - 1; i++)
                        {
                                const int32 L0 = BI + i * 2, R0 = L0 + 1, L1 = L0 + 2, R1 = L0 + 3;
                                OutWaterData.Triangles.Add(L0); OutWaterData.Triangles.Add(R0); OutWaterData.Triangles.Add(R1);
                                OutWaterData.Triangles.Add(L0); OutWaterData.Triangles.Add(R1); OutWaterData.Triangles.Add(L1);
                        }
                        continue;
                }

                // -------------------------------------------------------------
                // Closed lake / pond / sea / reservoir: original filled-polygon
                // path, one flat elevation offset sampled at the footprint
                // centroid (a still-water surface is level).
                // -------------------------------------------------------------
                if (Water.Points.Num() < 3) continue;
                const TArray<FVector2D> RPts = ResamplePolylineForTerrain(Water.Points, 10.f, /*bClosed*/ true);
                TArray<FVector> LP;
                for (const FVector2D& P : RPts)
                        LP.Add(FVector(P.X * ScaleFactor, -P.Y * ScaleFactor, 0.f));

                const int32 StartIdx = OutWaterData.Vertices.Num();
                if (Water.Holes.Num() > 0)
                {
                        TArray<FVector2D> Outer;
                        for (const FVector& P : LP) Outer.Add(FVector2D(P.X, P.Y));
                        TArray<TArray<FVector2D>> Holes;
                        for (const TArray<FVector2D>& SourceHole : Water.Holes)
                        {
                                TArray<FVector2D>& Hole = Holes.AddDefaulted_GetRef();
                                for (const FVector2D& P : SourceHole)
                                        Hole.Add(FVector2D(P.X * ScaleFactor, -P.Y * ScaleFactor));
                        }
                        if (!AddPolygonWithHolesTriangulated(OutWaterData, Outer, Holes, WaterSinkZ, WC))
                        {
                                UE_LOG(LogTemp, Warning, TEXT("HiroGeo: skipped water polygon '%s' because hole-aware triangulation failed"), *Water.Name);
                                continue;
                        }
                }
                else
                {
                        AddPolygonTriangulated(OutWaterData, LP, WaterSinkZ, WC);
                }

                if (SampleGroundZ)
                {
                        for (int32 Index = StartIdx; Index < OutWaterData.Vertices.Num(); ++Index)
                        {
                                FVector& Vertex = OutWaterData.Vertices[Index];
                                Vertex.Z = SampleGroundZ(FVector2D(Vertex.X, Vertex.Y)) + 2.0;
                        }
                }
                else if (bHaveDem || bHaveSamples)
                {
                        // Still-water level: min DEM under the shoreline ring (same idea
                        // as building terrainMinHeight). Centroid sampling on slopes put
                        // the whole lake above/below the surrounding ground mesh.
                        const float ZOff = bHaveDem
                                ? SampleDemMinOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude,
                                        WaterRefElev, ScaleFactor, RPts)
                                : SampleElevationSamplesMinOffsetUE(*ElevationSamples, WaterRefElev,
                                        ScaleFactor, RPts);
                        ShiftZRange(OutWaterData, StartIdx, ZOff);
                }
        }

        if (SampleGroundZ && SampleGroundCells > 0)
        {
                for (int32 Index = 0; Index < OutWaterData.Vertices.Num(); ++Index)
                {
                        const FVector& Vertex = OutWaterData.Vertices[Index];
                        OutWaterData.UV0[Index] = FVector2D(Vertex.X / 100.0, Vertex.Y / 100.0);
                }
                ProjectOntoGround(OutWaterData);
        }
}

void FGeoMeshGenerator::ProjectOntoGround(FMeshData& Mesh, const TBitArray<>* Eligible)
{
        if (!SampleGroundZ || SampleGroundCells <= 0 || !SampleGroundBounds.bIsValid)
                return;

        // Split each surface triangle at terrain edges so its interior follows
        // the same piecewise planar surface as its vertices.
        const FVector2D Origin = SampleGroundBounds.Min;
        const FVector2D Step = SampleGroundBounds.GetSize() / SampleGroundCells;
        using FClipPolygon = TArray<FVector2D, TInlineAllocator<8>>;
        auto Clip = [](const FClipPolygon& Input, const FVector2D& A, const FVector2D& B)
        {
                FClipPolygon Output;
                if (Input.IsEmpty()) return Output;
                FVector2D Previous = Input.Last();
                double PreviousSide = FVector2D::CrossProduct(B - A, Previous - A);
                for (const FVector2D& Current : Input)
                {
                        const double CurrentSide = FVector2D::CrossProduct(B - A, Current - A);
                        if ((CurrentSide >= 0.0) != (PreviousSide >= 0.0))
                                Output.Add(FMath::Lerp(Previous, Current, PreviousSide / (PreviousSide - CurrentSide)));
                        if (CurrentSide >= 0.0) Output.Add(Current);
                        Previous = Current;
                        PreviousSide = CurrentSide;
                }
                return Output;
        };
        auto Cell = [this](double Offset, double Size)
        {
                return FMath::Clamp(FMath::FloorToInt(Offset / Size), 0, SampleGroundCells - 1);
        };
        const TArray<int32> Input = MoveTemp(Mesh.Triangles);
        for (int32 Index = 0; Index + 2 < Input.Num(); Index += 3)
        {
                const int32 I0 = Input[Index], I1 = Input[Index + 1], I2 = Input[Index + 2];
                const FVector A = Mesh.Vertices[I0], B = Mesh.Vertices[I1], C = Mesh.Vertices[I2];
                const FVector2D P0(A.X, A.Y), P1(B.X, B.Y), P2(C.X, C.Y);
                const double Area = FVector2D::CrossProduct(P1 - P0, P2 - P0);
                if ((Eligible && !(*Eligible)[Index / 3]) || FMath::Abs(Area) <= UE_DOUBLE_SMALL_NUMBER)
                {
                        Mesh.Triangles.Append({I0, I1, I2});
                        continue;
                }
                const FClipPolygon Triangle{P0, P1, P2};
                FBox2D Bounds(ForceInit);
                for (const FVector2D& Point : Triangle) Bounds += Point;
                const double Z0 = A.Z - SampleGroundZ(P0);
                const double Z1 = B.Z - SampleGroundZ(P1);
                const double Z2 = C.Z - SampleGroundZ(P2);
                const bool bHasUV1 = Mesh.UV1.Num() == Mesh.Vertices.Num();
                auto AppendVertex = [&](const FVector2D& P)
                {
                        const double U = FVector2D::CrossProduct(P - P0, P2 - P0) / Area;
                        const double V = FVector2D::CrossProduct(P1 - P0, P - P0) / Area;
                        const double W = 1.0 - U - V;
                        const FVector2D UV = Mesh.UV0[I0] * W + Mesh.UV0[I1] * U + Mesh.UV0[I2] * V;
                        const FVector Normal = Mesh.Normals[I0] * W + Mesh.Normals[I1] * U + Mesh.Normals[I2] * V;
                        const FColor Color = Mesh.VertexColors[I0];
                        const FProcMeshTangent Tangent = Mesh.Tangents[I0];
                        if (bHasUV1)
                        {
                                const FVector2D UV1 = Mesh.UV1[I0];
                                Mesh.UV1.Add(UV1);
                        }
                        Mesh.Triangles.Add(Mesh.Vertices.Num());
                        Mesh.Vertices.Emplace(P.X, P.Y, SampleGroundZ(P) + Z0 * W + Z1 * U + Z2 * V);
                        Mesh.UV0.Add(UV);
                        Mesh.Normals.Add(Normal.GetSafeNormal());
                        Mesh.VertexColors.Add(Color);
                        Mesh.Tangents.Add(Tangent);
                };
                for (int32 Y = Cell(Bounds.Min.Y - Origin.Y, Step.Y); Y <= Cell(Bounds.Max.Y - Origin.Y, Step.Y); ++Y)
                {
                        for (int32 X = Cell(Bounds.Min.X - Origin.X, Step.X); X <= Cell(Bounds.Max.X - Origin.X, Step.X); ++X)
                        {
                                const FVector2D P00 = Origin + FVector2D(X * Step.X, Y * Step.Y);
                                const FVector2D P10 = P00 + FVector2D(Step.X, 0);
                                const FVector2D P01 = P00 + FVector2D(0, Step.Y);
                                const FVector2D P11 = P00 + Step;
                                const FVector2D Faces[2][3] = {{P00, P10, P11}, {P00, P11, P01}};
                                for (const auto& Face : Faces)
                                {
                                        const FClipPolygon Piece = Clip(Clip(Clip(Triangle, Face[0], Face[1]),
                                                Face[1], Face[2]), Face[2], Face[0]);
                                        for (int32 Point = 1; Point + 1 < Piece.Num(); ++Point)
                                        {
                                                if (FMath::Abs(FVector2D::CrossProduct(Piece[Point] - Piece[0],
                                                        Piece[Point + 1] - Piece[0])) <= UE_DOUBLE_SMALL_NUMBER)
                                                        continue;
                                                AppendVertex(Piece[0]);
                                                AppendVertex(Piece[Point]);
                                                AppendVertex(Piece[Point + 1]);
                                        }
                                }
                        }
                }
        }
}

// ============================================================================
// GenerateGroundMesh
// ============================================================================
void FGeoMeshGenerator::GenerateGroundMesh(
        const TArray<FGeoBuilding>& Buildings, const TArray<FGeoRoad>& Roads,
        const TArray<FGeoWater>& WaterBodies, const TArray<FGeoVegetation>& Vegetations,
        const TArray<FElevationSample>& ElevationSamples,
        FMeshData& OutGroundData, float GroundOffset,
        bool bUseTerrainElevation, int32 TerrainGridSubdivisions,
        const FHiroGeoHeightmap* DemHeightmap, double OriginLatitude, double OriginLongitude,
        bool bHaveGridBounds, float GridMinX, float GridMaxX, float GridMinY, float GridMaxY)
{
        OutGroundData.Reset();
        float MinX, MaxX, MinY, MaxY;
        float Pad;
        if (bHaveGridBounds)
        {
                // اندازه‌ی دقیق سلول گرید انتخاب‌شده (مثلاً ۵۰۰×۵۰۰ متر) را به‌جای
                // bounding-box داده‌های OSM استفاده می‌کنیم؛ آن باندینگ‌باکس می‌تواند به‌خاطر
                // یک جاده/پارک که فقط لبه‌اش داخل سلول است، از اندازه‌ی واقعی سلول بزرگ‌تر
                // شود. وقتی مرز دقیق گرید را داریم، هیچ پدینگ اضافه‌ای لازم نیست.
                MinX = GridMinX; MaxX = GridMaxX; MinY = GridMinY; MaxY = GridMaxY;
                Pad = 0.f;
        }
        else
        {
                ComputeDataBounds(Buildings, Roads, WaterBodies, Vegetations, MinX, MaxX, MinY, MaxY);
                if (MinX >= MaxX || MinY >= MaxY) { MinX = -5000.f; MaxX = 5000.f; MinY = -5000.f; MaxY = 5000.f; }
                Pad = 5000.f;
        }

        float UMinX = MinX * ScaleFactor - Pad, UMaxX = MaxX * ScaleFactor + Pad;
        // ---------------------------------------------------------------------------
        // Y-axis mirror fix:
        // Every other generator (buildings line ~3863, roads line ~4353, railways
        // line ~4648, OSM trees line ~4785) uses UE_Y = -LocalY * ScaleFactor.
        // ComputeDataBounds() returns raw local-meter Y (Min/MaxY), so the UE-Y
        // range must be mirrored AND swapped: UE_Y_max corresponds to local Y_min
        // and vice-versa. Without this, the ground plane lands in +Y while every
        // building sits in -Y, producing the visible "ground floating to one side
        // and shifted" defect.
        // ---------------------------------------------------------------------------
        float UMinY = -MaxY * ScaleFactor - Pad, UMaxY = -MinY * ScaleFactor + Pad;

        const bool bHaveDem = (DemHeightmap != nullptr) && DemHeightmap->IsValid();
        const bool bDeformTerrain = bUseTerrainElevation && (bHaveDem || ElevationSamples.Num() >= 3);
        double RefElev = 0.0;
        if (bDeformTerrain)
        {
                if (bHaveDem)
                {
                        // Use the DEM elevation under the data's local origin as "ground
                        // zero" so the OSM-derived geometry sits at roughly Z≈0 locally,
                        // matching how the OSM-"ele"-tag path picks a reference below.
                        RefElev = DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude);
                }
                else
                {
                        RefElev = ComputeElevationSamplesRefElev(ElevationSamples);
                }
        }

        // Same local-meter projection as FOSMLoader::LatLonToLocal, inverted, so
        // DEM samples line up exactly with the rest of the OSM-derived geometry
        // (which is expressed in local meters relative to OriginLatitude/Longitude).
        const double EarthRadiusM = 6371000.0;
        const double CosOriginLat = FMath::Cos(OriginLatitude * UE_PI / 180.0);
        auto LocalMetersToLatLon = [&](double LocalXM, double LocalYM, double& OutLat, double& OutLon)
        {
                OutLat = OriginLatitude + (LocalYM / EarthRadiusM) * 180.0 / UE_PI;
                OutLon = OriginLongitude + (LocalXM / (EarthRadiusM * FMath::Max(CosOriginLat, 0.0001))) * 180.0 / UE_PI;
        };

        auto SampleElevUE = [&](float LocalXM, float LocalYM) -> float
        {
                if (!bDeformTerrain) return GroundOffset - 5.f;

                if (bHaveDem)
                {
                        double Lat, Lon;
                        LocalMetersToLatLon((double)LocalXM, (double)LocalYM, Lat, Lon);
                        const float ElevM = DemHeightmap->SampleAtLatLon(Lat, Lon) - (float)RefElev;
                        return ElevM * ScaleFactor + GroundOffset - 5.f;
                }

                return SampleElevationSamplesOffsetUE(ElevationSamples, RefElev, ScaleFactor, LocalXM, LocalYM)
                        + GroundOffset - 5.f;
        };

        if (!bDeformTerrain)
        {
                const float Z = GroundOffset - 5.f;
                AddQuad(OutGroundData,
                        FVector(UMinX, UMinY, Z),
                        FVector(UMaxX, UMinY, Z),
                        FVector(UMaxX, UMaxY, Z),
                        FVector(UMinX, UMaxY, Z),
                        FColor(146, 143, 138));

                const int32 US = OutGroundData.UV0.Num() - 4;
                const float TX = (UMaxX - UMinX) / 1000.f, TY = (UMaxY - UMinY) / 1000.f;
                OutGroundData.UV0[US + 0] = FVector2D(0, 0);
                OutGroundData.UV0[US + 1] = FVector2D(TX, 0);
                OutGroundData.UV0[US + 2] = FVector2D(TX, TY);
                OutGroundData.UV0[US + 3] = FVector2D(0, TY);
                FillSliceUV1(OutGroundData, 0, 2.f);
                FlipNormals(OutGroundData);
                return;
        }

        const int32 NX = FMath::Clamp(TerrainGridSubdivisions, 8, 256);
        const int32 NY = NX;
        const float StepX = (UMaxX - UMinX) / NX;
        const float StepY = (UMaxY - UMinY) / NY;

        auto GridZ = [&](int32 Ix, int32 Iy) -> float
        {
                const float UX = UMinX + StepX * Ix;
                const float UY = UMinY + StepY * Iy;
                const float LocalXM = UX / ScaleFactor;
                const float LocalYM = -UY / ScaleFactor;
                return SampleElevUE(LocalXM, LocalYM);
        };

        for (int32 Iy = 0; Iy <= NY; ++Iy)
        {
                for (int32 Ix = 0; Ix <= NX; ++Ix)
                {
                        OutGroundData.Vertices.Emplace(UMinX + StepX * Ix, UMinY + StepY * Iy, GridZ(Ix, Iy));
                        OutGroundData.VertexColors.Emplace(146, 143, 138);
                        OutGroundData.Tangents.Emplace(FVector(1, 0, 0), false);
                }
        }
        OutGroundData.UV0.SetNum(OutGroundData.Vertices.Num());

        const int32 RowStride = NX + 1;
        for (int32 Iy = 0; Iy < NY; Iy++)
        {
                for (int32 Ix = 0; Ix < NX; Ix++)
                {
                        const int32 I00 = Iy * RowStride + Ix;
                        const int32 I10 = I00 + 1;
                        const int32 I01 = I00 + RowStride;
                        const int32 I11 = I01 + 1;
                        OutGroundData.Triangles.Append({I00, I10, I11, I00, I11, I01});
                }
        }

        const float TX = (UMaxX - UMinX) / 1000.f, TY = (UMaxY - UMinY) / 1000.f;
        for (int32 i = 0; i < OutGroundData.UV0.Num(); i++)
        {
                const FVector& V = OutGroundData.Vertices[i];
                OutGroundData.UV0[i] = FVector2D(
                        (V.X - UMinX) / FMath::Max(UMaxX - UMinX, 1.f) * TX,
                        (V.Y - UMinY) / FMath::Max(UMaxY - UMinY, 1.f) * TY);
        }
        FillSliceUV1(OutGroundData, 0, 2.f);
        RecomputeNormals(OutGroundData);
}

// ============================================================================
// GenerateRailwayMeshes — ballast bed + twin rails (streets.gl railway parity)
// ============================================================================
void FGeoMeshGenerator::GenerateRailwayMeshes(
        const TArray<FGeoRailway>& Railways,
        FMeshData& OutBallastData,
        FMeshData& OutRailData,
        const FHiroGeoHeightmap* DemHeightmap, double OriginLatitude, double OriginLongitude,
        const TArray<FElevationSample>* ElevationSamples)
{
        OutBallastData.Reset();
        OutRailData.Reset();

        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const bool bHaveSamples = ElevationSamples && ElevationSamples->Num() >= 3;
        const bool bDeform = bHaveDem || bHaveSamples;
        const double RailRefElev = bHaveDem ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : (bHaveSamples ? ComputeElevationSamplesRefElev(*ElevationSamples) : 0.0);

        const FColor BallastColor(95, 88, 78);
        const FColor RailColor(45, 45, 48);
        const float BallastZ = 2.5f;
        const float RailZ = 5.f;

        auto AddRailQuad = [](FMeshData& MD,
                const FVector& V0, const FVector& V1, const FVector& V2, const FVector& V3,
                const FColor& Col, float Slice)
        {
                const int32 BI = MD.Vertices.Num();
                MD.Vertices.Add(V0); MD.Vertices.Add(V1); MD.Vertices.Add(V2); MD.Vertices.Add(V3);
                MD.Triangles.Add(BI + 0); MD.Triangles.Add(BI + 1); MD.Triangles.Add(BI + 2);
                MD.Triangles.Add(BI + 0); MD.Triangles.Add(BI + 2); MD.Triangles.Add(BI + 3);
                const FVector Tang = (V3 - V0).GetSafeNormal();
                for (int32 k = 0; k < 4; k++)
                {
                        MD.Normals.Add(FVector(0, 0, 1));
                        MD.VertexColors.Add(Col);
                        MD.Tangents.Add(FProcMeshTangent(Tang.IsNearlyZero() ? FVector(1, 0, 0) : Tang, false));
                        MD.UV0.Add(FVector2D((k == 1 || k == 2) ? 1.f : 0.f, (k >= 2) ? 1.f : 0.f));
                        MD.UV1.Add(FVector2D(Slice, 0.f));
                }
        };

        for (const FGeoRailway& Railway : Railways)
        {
                const TArray<FVector2D> RPts = ResamplePolylineForTerrain(Railway.Points, 10.f, /*bClosed*/ false);
                const int32 N = RPts.Num();
                if (N < 2) continue;

                const int32 Tracks = FMath::Max(1, Railway.Tracks);
                const float GaugeM = Railway.Gauge > 0.f ? Railway.Gauge : 1435.f;
                const float GaugeUE = (GaugeM / 1000.f) * ScaleFactor;
                const float BallastHalfW = FMath::Max(250.f, GaugeUE * 0.55f + 120.f * Tracks);
                const float RailHalfW = 8.f;

                TArray<FVector2D> Pts;
                Pts.Reserve(N);
                for (int32 i = 0; i < N; i++)
                        Pts.Add(FVector2D(RPts[i].X * ScaleFactor, -RPts[i].Y * ScaleFactor));

                // Per-point terrain Z offset (UE units) — sampled individually so the
                // ballast bed/rails follow rising/falling terrain along their length
                // instead of sitting on one flat plane (the old hardcoded BallastZ/
                // RailZ made tracks float over hills or sink into dips on anything
                // but perfectly flat ground). All zero when no DEM/samples are set.
                TArray<float> TZ;
                TZ.SetNumZeroed(N);
                if (bDeform && !SampleGroundZ)
                {
                        for (int32 i = 0; i < N; i++)
                        {
                                TZ[i] = bHaveDem
                                        ? SampleDemOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude,
                                                RailRefElev, ScaleFactor, RPts[i].X, RPts[i].Y)
                                        : SampleElevationSamplesOffsetUE(*ElevationSamples, RailRefElev,
                                                ScaleFactor, RPts[i].X, RPts[i].Y);
                        }
                }

                TArray<FVector2D> Miter;
                Miter.SetNum(N);
                auto EdgeNormal = [](const FVector2D& A, const FVector2D& B) -> FVector2D
                {
                        const FVector2D D = (B - A).GetSafeNormal();
                        return FVector2D(D.Y, -D.X);
                };
                for (int32 i = 0; i < N; i++)
                {
                        const FVector2D nIn  = (i > 0)     ? EdgeNormal(Pts[i - 1], Pts[i]) : FVector2D::ZeroVector;
                        const FVector2D nOut = (i < N - 1) ? EdgeNormal(Pts[i], Pts[i + 1]) : FVector2D::ZeroVector;
                        FVector2D m;
                        if (nIn.IsNearlyZero())      m = nOut;
                        else if (nOut.IsNearlyZero()) m = nIn;
                        else
                        {
                                m = (nIn + nOut).GetSafeNormal();
                                const float c = FVector2D::DotProduct(m, nOut);
                                m *= (FMath::Abs(c) > 0.2f) ? (1.f / c) : 5.f;
                        }
                        if (m.IsNearlyZero()) m = FVector2D(1, 0);
                        Miter[i] = m;
                }

                auto RailVertex = [&](const FVector2D& Point, float Height)
                {
                        return FVector(Point, SampleGroundZ ? SampleGroundZ(Point) + 5.0 + Height : Height);
                };
                TArray<FVector> BallastL, BallastR;
                BallastL.SetNum(N); BallastR.SetNum(N);
                TArray<FVector> RailL_L, RailR_L, RailL_R, RailR_R;
                RailL_L.SetNum(N); RailR_L.SetNum(N); RailL_R.SetNum(N); RailR_R.SetNum(N);
                for (int32 i = 0; i < N; i++)
                {
                        const FVector2D P = Pts[i];
                        const FVector2D M = Miter[i];
                        const FVector2D Across = M.GetSafeNormal();
                        BallastL[i] = RailVertex(P + M * BallastHalfW, BallastZ + TZ[i]);
                        BallastR[i] = RailVertex(P - M * BallastHalfW, BallastZ + TZ[i]);

                        const FVector2D CL = P + Across * (GaugeUE * 0.5f);
                        const FVector2D CR = P - Across * (GaugeUE * 0.5f);
                        RailL_L[i] = RailVertex(CL + M * RailHalfW, RailZ + TZ[i]);
                        RailR_L[i] = RailVertex(CL - M * RailHalfW, RailZ + TZ[i]);
                        RailL_R[i] = RailVertex(CR + M * RailHalfW, RailZ + TZ[i]);
                        RailR_R[i] = RailVertex(CR - M * RailHalfW, RailZ + TZ[i]);
                }

                for (int32 i = 0; i < N - 1; i++)
                {
                        AddRailQuad(OutBallastData, BallastR[i], BallastL[i], BallastL[i + 1], BallastR[i + 1], BallastColor, 1.f);
                        AddRailQuad(OutRailData, RailR_L[i], RailL_L[i], RailL_L[i + 1], RailR_L[i + 1], RailColor, 0.f);
                        AddRailQuad(OutRailData, RailR_R[i], RailL_R[i], RailL_R[i + 1], RailR_R[i + 1], RailColor, 0.f);
                }
        }

        ProjectOntoGround(OutBallastData);
        ProjectOntoGround(OutRailData);
        RecomputeNormals(OutBallastData);
        RecomputeNormals(OutRailData);
}

// ============================================================================
// GenerateFenceMeshes — barrier=fence|wall|hedge (streets-gl FenceBuilder)
// ============================================================================
void FGeoMeshGenerator::GenerateFenceMeshes(
        const TArray<FGeoFence>& Fences,
        FMeshData& OutFenceData,
        const FHiroGeoHeightmap* DemHeightmap, double OriginLatitude, double OriginLongitude,
        const TArray<FElevationSample>* ElevationSamples)
{
        OutFenceData.Reset();
        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const bool bHaveSamples = ElevationSamples && ElevationSamples->Num() >= 3;
        const bool bDeform = bHaveDem || bHaveSamples;
        const double RefElev = bHaveDem ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : (bHaveSamples ? ComputeElevationSamplesRefElev(*ElevationSamples) : 0.0);

        for (const FGeoFence& Fence : Fences)
        {
                if (Fence.Points.Num() < 2) continue;

                FColor Col(160, 160, 160);
                if (Fence.Kind == EFenceKind::Hedge) Col = FColor(40, 100, 40);
                else if (Fence.Material == TEXT("wood")) Col = FColor(120, 80, 40);
                else if (Fence.Material == TEXT("chain_link")) Col = FColor(140, 145, 150);
                else if (Fence.Material == TEXT("stone")) Col = FColor(130, 125, 115);
                else if (Fence.Material == TEXT("concrete")) Col = FColor(150, 150, 145);
                else Col = FColor(90, 95, 100); // metal

                const float H = Fence.Height * ScaleFactor;
                const float MinH = Fence.MinHeight * ScaleFactor;
                const float HalfT = FMath::Max(Fence.Thickness * ScaleFactor * 0.5f, 2.f);

                for (int32 i = 0; i + 1 < Fence.Points.Num(); i++)
                {
                        const FVector2D A(Fence.Points[i].X * ScaleFactor, -Fence.Points[i].Y * ScaleFactor);
                        const FVector2D B(Fence.Points[i + 1].X * ScaleFactor, -Fence.Points[i + 1].Y * ScaleFactor);
                        FVector2D Dir = (B - A).GetSafeNormal();
                        if (Dir.IsNearlyZero()) continue;
                        const FVector2D N(Dir.Y, -Dir.X);

                        auto SampleZ = [&](const FVector2D& LocalM) -> float
                        {
                                if (!bDeform) return 0.f;
                                return bHaveDem
                                        ? SampleDemOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude, RefElev, ScaleFactor, LocalM.X, LocalM.Y)
                                        : SampleElevationSamplesOffsetUE(*ElevationSamples, RefElev, ScaleFactor, LocalM.X, LocalM.Y);
                        };
                        const float ZA = SampleZ(Fence.Points[i]);
                        const float ZB = SampleZ(Fence.Points[i + 1]);

                        // Thin vertical panel (double-sided) — FenceBuilder style.
                        // For walls/hedges, extrude a small thickness.
                        if (Fence.Kind == EFenceKind::Fence)
                        {
                                const FVector V0(A.X, A.Y, MinH + ZA);
                                const FVector V1(B.X, B.Y, MinH + ZB);
                                const FVector V2(B.X, B.Y, MinH + H + ZB);
                                const FVector V3(A.X, A.Y, MinH + H + ZA);
                                AddDoubleSidedQuad(OutFenceData, V0, V1, V2, V3, Col);
                        }
                        else
                        {
                                const FVector2D AL = A + N * HalfT, AR = A - N * HalfT;
                                const FVector2D BL = B + N * HalfT, BR = B - N * HalfT;
                                // Four side faces + top
                                AddQuad(OutFenceData,
                                        FVector(AL.X, AL.Y, MinH + ZA), FVector(BL.X, BL.Y, MinH + ZB),
                                        FVector(BL.X, BL.Y, MinH + H + ZB), FVector(AL.X, AL.Y, MinH + H + ZA), Col);
                                AddQuad(OutFenceData,
                                        FVector(BR.X, BR.Y, MinH + ZB), FVector(AR.X, AR.Y, MinH + ZA),
                                        FVector(AR.X, AR.Y, MinH + H + ZA), FVector(BR.X, BR.Y, MinH + H + ZB), Col);
                                AddQuad(OutFenceData,
                                        FVector(AL.X, AL.Y, MinH + H + ZA), FVector(BL.X, BL.Y, MinH + H + ZB),
                                        FVector(BR.X, BR.Y, MinH + H + ZB), FVector(AR.X, AR.Y, MinH + H + ZA), Col);
                        }
                }
        }

        RecomputeNormals(OutFenceData);
}

// ============================================================================
// GeneratePowerLineMeshes — streets-gl WireGroupBuilder socket attachment
// ============================================================================
void FGeoMeshGenerator::GeneratePowerLineMeshes(
        const TArray<FGeoPowerLine>& PowerLines,
        FMeshData& OutWireData,
        const FHiroGeoHeightmap* DemHeightmap, double OriginLatitude, double OriginLongitude,
        const TArray<FElevationSample>* ElevationSamples,
        TArray<FGeoModelInstance>* PoleModels)
{
        OutWireData.Reset();
        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const bool bHaveSamples = ElevationSamples && ElevationSamples->Num() >= 3;
        const bool bDeform = bHaveDem || bHaveSamples;
        const double RefElev = bHaveDem ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : (bHaveSamples ? ComputeElevationSamplesRefElev(*ElevationSamples) : 0.0);
        const FColor WireCol(30, 30, 30);

        // streets-gl WireSocketConfig (Blender Y-up → local X / height Z in meters).
        struct FWireSocket { float LocalX; float HeightM; };
        static const FWireSocket TowerSocks[] = {
                {-8.66f, 20.26f}, {-6.71f, 27.77f}, {7.00f, 27.77f}, {8.95f, 20.26f}, {0.14f, 20.26f}
        };
        static const FWireSocket PoleSocks[] = {
                {-1.00f, 9.01f}, {-0.50f, 9.01f}, {0.50f, 9.01f}, {1.00f, 9.01f}
        };
        static const FWireSocket GroundSocks[] = {
                {-1.00f, 0.f}, {-0.50f, 0.f}, {0.50f, 0.f}, {1.00f, 0.f}, {0.f, 0.f}
        };

        struct FPoleAnchor
        {
                FVector2D Loc;
                bool bTower = false;
                int32 ModelIndex = INDEX_NONE;
                FVector2D DirAccum = FVector2D::ZeroVector;
                int32 DirCount = 0;
        };
        TArray<FPoleAnchor> Anchors;
        if (PoleModels)
        {
                for (int32 mi = 0; mi < PoleModels->Num(); mi++)
                {
                        const FGeoModelInstance& M = (*PoleModels)[mi];
                        const FString T = M.ModelType.ToLower();
                        if (T == TEXT("utility_pole") || T == TEXT("transmission_tower"))
                        {
                                FPoleAnchor A;
                                A.Loc = M.Location;
                                A.bTower = (T == TEXT("transmission_tower"));
                                A.ModelIndex = mi;
                                Anchors.Add(A);
                        }
                }
        }

        auto FindAnchor = [&](const FVector2D& P, int32& OutIdx) -> bool
        {
                OutIdx = INDEX_NONE;
                const float MaxDistSq = 25.f * 25.f;
                float Best = MaxDistSq;
                for (int32 i = 0; i < Anchors.Num(); i++)
                {
                        const float D = FVector2D::DistSquared(P, Anchors[i].Loc);
                        if (D < Best) { Best = D; OutIdx = i; }
                }
                return OutIdx != INDEX_NONE;
        };

        auto SampleZ = [&](const FVector2D& LocalM) -> float
        {
                if (!bDeform) return 0.f;
                return bHaveDem
                        ? SampleDemOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude, RefElev, ScaleFactor, LocalM.X, LocalM.Y)
                        : SampleElevationSamplesOffsetUE(*ElevationSamples, RefElev, ScaleFactor, LocalM.X, LocalM.Y);
        };

        auto RotateSock = [](float LocalX, float RotRad) -> FVector2D
        {
                // Rotate socket lateral offset around vertical (streets-gl Y-axis rot).
                const float C = FMath::Cos(RotRad);
                const float S = FMath::Sin(RotRad);
                return FVector2D(LocalX * C, LocalX * S);
        };

        for (const FGeoPowerLine& PL : PowerLines)
        {
                if (PL.Points.Num() < 2) continue;

                const bool bPreferTower = !PL.bIsMinor;
                TArray<FVector2D> Snapped = PL.Points;
                TArray<int32> AnchorAt;
                AnchorAt.SetNum(Snapped.Num());
                for (int32 i = 0; i < Snapped.Num(); i++)
                {
                        int32 Ai = INDEX_NONE;
                        if (FindAnchor(PL.Points[i], Ai))
                        {
                                Snapped[i] = Anchors[Ai].Loc;
                                AnchorAt[i] = Ai;
                        }
                        else
                        {
                                AnchorAt[i] = INDEX_NONE;
                        }
                }

                // Accumulate wire directions at each pole for rotation (PowerlineNode.updateRotation).
                for (int32 i = 0; i + 1 < Snapped.Num(); i++)
                {
                        FVector2D Dir = (Snapped[i + 1] - Snapped[i]).GetSafeNormal();
                        if (Dir.IsNearlyZero()) continue;
                        if (AnchorAt[i] != INDEX_NONE)
                        {
                                Anchors[AnchorAt[i]].DirAccum += Dir;
                                Anchors[AnchorAt[i]].DirCount++;
                        }
                        if (AnchorAt[i + 1] != INDEX_NONE)
                        {
                                Anchors[AnchorAt[i + 1]].DirAccum += -Dir;
                                Anchors[AnchorAt[i + 1]].DirCount++;
                        }
                }

                auto NodeRot = [&](int32 VertIdx) -> float
                {
                        const int32 Ai = AnchorAt[VertIdx];
                        if (Ai == INDEX_NONE || Anchors[Ai].DirCount == 0)
                        {
                                // Fallback: along the polyline at this vertex.
                                FVector2D D = FVector2D::ZeroVector;
                                if (VertIdx + 1 < Snapped.Num()) D += (Snapped[VertIdx + 1] - Snapped[VertIdx]);
                                if (VertIdx > 0) D += (Snapped[VertIdx] - Snapped[VertIdx - 1]);
                                D = D.GetSafeNormal();
                                return D.IsNearlyZero() ? 0.f : FMath::Atan2(D.Y, D.X);
                        }
                        FVector2D Avg = Anchors[Ai].DirAccum.GetSafeNormal();
                        if (Avg.IsNearlyZero()) return 0.f;
                        return FMath::Atan2(Avg.Y, Avg.X);
                };

                auto IsTowerNode = [&](int32 VertIdx) -> bool
                {
                        if (AnchorAt[VertIdx] != INDEX_NONE) return Anchors[AnchorAt[VertIdx]].bTower;
                        return bPreferTower;
                };

                for (int32 i = 0; i + 1 < Snapped.Num(); i++)
                {
                        const FVector2D ALoc = Snapped[i];
                        const FVector2D BLoc = Snapped[i + 1];
                        FVector2D SegDir = (BLoc - ALoc).GetSafeNormal();
                        if (SegDir.IsNearlyZero()) continue;

                        float RotA = NodeRot(i);
                        float RotB = NodeRot(i + 1);
                        // streets-gl: flip end rotation if opposing the start.
                        {
                                const FVector2D UA(FMath::Cos(RotA), FMath::Sin(RotA));
                                const FVector2D UB(FMath::Cos(RotB), FMath::Sin(RotB));
                                if (FVector2D::DotProduct(UA, UB) < 0.f) RotB += PI;
                        }

                        const bool bTowerA = IsTowerNode(i);
                        const bool bTowerB = IsTowerNode(i + 1);
                        const bool bGroundA = (AnchorAt[i] == INDEX_NONE);
                        const bool bGroundB = (AnchorAt[i + 1] == INDEX_NONE);
                        if (bGroundA && bGroundB) continue; // streets-gl skips ground↔ground

                        const FWireSocket* SocksA = bGroundA ? GroundSocks : (bTowerA ? TowerSocks : PoleSocks);
                        const FWireSocket* SocksB = bGroundB ? GroundSocks : (bTowerB ? TowerSocks : PoleSocks);
                        const int32 NumA = bGroundA ? 5 : (bTowerA ? 5 : 4);
                        const int32 NumB = bGroundB ? 5 : (bTowerB ? 5 : 4);
                        const int32 NumCond = FMath::Min(NumA, NumB);

                        const float HalfW = 3.f;
                        const float SagFrac = 0.04f;
                        const FVector2D AUE(ALoc.X * ScaleFactor, -ALoc.Y * ScaleFactor);
                        const FVector2D BUE(BLoc.X * ScaleFactor, -BLoc.Y * ScaleFactor);
                        const float SegLen = FVector2D::Distance(AUE, BUE);
                        const int32 Segs = FMath::Clamp(FMath::CeilToInt(SegLen / 400.f), 2, 16);
                        const FVector2D N(SegDir.Y, -SegDir.X); // unused lateral fallback

                        for (int32 c = 0; c < NumCond; c++)
                        {
                                const FVector2D OffA = RotateSock(SocksA[c].LocalX, RotA);
                                const FVector2D OffB = RotateSock(SocksB[c].LocalX, RotB);
                                const FVector2D FromM = ALoc + OffA;
                                const FVector2D ToM   = BLoc + OffB;
                                const FVector2D FromUE(FromM.X * ScaleFactor, -FromM.Y * ScaleFactor);
                                const FVector2D ToUE(ToM.X * ScaleFactor, -ToM.Y * ScaleFactor);
                                FVector2D WireDir = (ToUE - FromUE).GetSafeNormal();
                                if (WireDir.IsNearlyZero()) WireDir = FVector2D(1, 0);
                                const FVector2D WireN(WireDir.Y, -WireDir.X);

                                const float ZA = SampleZ(FromM) + SocksA[c].HeightM * ScaleFactor;
                                const float ZB = SampleZ(ToM) + SocksB[c].HeightM * ScaleFactor;

                                TArray<FVector> Left, Right;
                                Left.SetNum(Segs + 1); Right.SetNum(Segs + 1);
                                for (int32 s = 0; s <= Segs; s++)
                                {
                                        const float T = (float)s / (float)Segs;
                                        const FVector2D P = FMath::Lerp(FromUE, ToUE, T);
                                        const float Sag = 4.f * SagFrac * SegLen * T * (1.f - T);
                                        const float Z = FMath::Lerp(ZA, ZB, T) - Sag;
                                        Left[s]  = FVector(P + WireN * HalfW, Z);
                                        Right[s] = FVector(P - WireN * HalfW, Z);
                                }
                                for (int32 s = 0; s < Segs; s++)
                                        AddDoubleSidedQuad(OutWireData, Right[s], Left[s], Left[s + 1], Right[s + 1], WireCol);
                        }
                        (void)N;
                }
        }

        // Orient pole/tower models to face average wire direction (PowerlineNode.rotation).
        if (PoleModels)
        {
                for (const FPoleAnchor& A : Anchors)
                {
                        if (A.ModelIndex == INDEX_NONE || A.DirCount == 0) continue;
                        FGeoModelInstance& M = (*PoleModels)[A.ModelIndex];
                        if (M.RotationDeg >= 0.f) continue; // respect explicit OSM direction
                        FVector2D Avg = A.DirAccum.GetSafeNormal();
                        if (Avg.IsNearlyZero()) continue;
                        // OSM/UE yaw: convert atan2 local-Y/X to degrees for actor spawn.
                        M.RotationDeg = FMath::RadiansToDegrees(FMath::Atan2(Avg.Y, Avg.X));
                }
        }

        RecomputeNormals(OutWireData);
}

// ============================================================================
// GeneratePitchMarkings — white line overlays for leisure=pitch (streets-gl textures)
// ============================================================================
void FGeoMeshGenerator::GeneratePitchMarkings(
        const TArray<FGeoVegetation>& Vegetations,
        FMeshData& OutMarkingData,
        const FHiroGeoHeightmap* DemHeightmap, double OriginLatitude, double OriginLongitude,
        const TArray<FElevationSample>* ElevationSamples)
{
        OutMarkingData.Reset();
        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const bool bHaveSamples = ElevationSamples && ElevationSamples->Num() >= 3;
        const bool bDeform = bHaveDem || bHaveSamples;
        const double RefElev = bHaveDem ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : (bHaveSamples ? ComputeElevationSamplesRefElev(*ElevationSamples) : 0.0);
        const FColor LineCol(240, 240, 240);
        const float LineZ = 14.f; // slightly above pitch surface
        const float HalfW = 8.f;  // ~8 cm line

        auto SampleZ = [&](float LocalXM, float LocalYM) -> float
        {
                if (!bDeform) return 0.f;
                return bHaveDem
                        ? SampleDemOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude, RefElev, ScaleFactor, LocalXM, LocalYM)
                        : SampleElevationSamplesOffsetUE(*ElevationSamples, RefElev, ScaleFactor, LocalXM, LocalYM);
        };

        auto AddMarkLine = [&](FVector2D A, FVector2D B)
        {
                const float LocalAX = A.X, LocalAY = A.Y, LocalBX = B.X, LocalBY = B.Y;
                A = FVector2D(A.X * ScaleFactor, -A.Y * ScaleFactor);
                B = FVector2D(B.X * ScaleFactor, -B.Y * ScaleFactor);
                FVector2D Dir = (B - A).GetSafeNormal();
                if (Dir.IsNearlyZero()) return;
                const FVector2D N(Dir.Y, -Dir.X);
                const float Z0 = SampleZ(LocalAX, LocalAY) + LineZ;
                const float Z1 = SampleZ(LocalBX, LocalBY) + LineZ;
                AddDoubleSidedQuad(OutMarkingData,
                        FVector(A - N * HalfW, Z0), FVector(A + N * HalfW, Z0),
                        FVector(B + N * HalfW, Z1), FVector(B - N * HalfW, Z1), LineCol);
        };

        for (const FGeoVegetation& Veg : Vegetations)
        {
                if (Veg.Points.Num() < 3) continue;
                const FString VT = Veg.VegType.ToLower();
                const bool bIsPitch = VT.Contains(TEXT("pitch")) || VT == TEXT("football")
                        || VT == TEXT("basketball") || VT == TEXT("tennis") || VT == TEXT("soccer");
                if (!bIsPitch) continue;

                // OBB-ish axis-aligned bounds in local meters for marking layout.
                float MinX = FLT_MAX, MaxX = -FLT_MAX, MinY = FLT_MAX, MaxY = -FLT_MAX;
                for (const FVector2D& P : Veg.Points)
                {
                        MinX = FMath::Min(MinX, P.X); MaxX = FMath::Max(MaxX, P.X);
                        MinY = FMath::Min(MinY, P.Y); MaxY = FMath::Max(MaxY, P.Y);
                }
                const float CX = (MinX + MaxX) * 0.5f;
                const float CY = (MinY + MaxY) * 0.5f;
                const float W = MaxX - MinX;
                const float H = MaxY - MinY;
                if (W < 2.f || H < 2.f) continue;

                // Outer rectangle (all pitch types).
                AddMarkLine(FVector2D(MinX, MinY), FVector2D(MaxX, MinY));
                AddMarkLine(FVector2D(MaxX, MinY), FVector2D(MaxX, MaxY));
                AddMarkLine(FVector2D(MaxX, MaxY), FVector2D(MinX, MaxY));
                AddMarkLine(FVector2D(MinX, MaxY), FVector2D(MinX, MinY));

                if (VT.Contains(TEXT("football")) || VT.Contains(TEXT("soccer")) || VT == TEXT("pitch"))
                {
                        // Halfway line + centre mark cross.
                        AddMarkLine(FVector2D(CX, MinY), FVector2D(CX, MaxY));
                        const float Cross = FMath::Min(W, H) * 0.04f;
                        AddMarkLine(FVector2D(CX - Cross, CY), FVector2D(CX + Cross, CY));
                        AddMarkLine(FVector2D(CX, CY - Cross), FVector2D(CX, CY + Cross));
                        // Penalty boxes (approx 16% / 40% of length/width).
                        const float BoxD = W * 0.16f;
                        const float BoxH = H * 0.44f;
                        AddMarkLine(FVector2D(MinX + BoxD, CY - BoxH * 0.5f), FVector2D(MinX + BoxD, CY + BoxH * 0.5f));
                        AddMarkLine(FVector2D(MinX, CY - BoxH * 0.5f), FVector2D(MinX + BoxD, CY - BoxH * 0.5f));
                        AddMarkLine(FVector2D(MinX, CY + BoxH * 0.5f), FVector2D(MinX + BoxD, CY + BoxH * 0.5f));
                        AddMarkLine(FVector2D(MaxX - BoxD, CY - BoxH * 0.5f), FVector2D(MaxX - BoxD, CY + BoxH * 0.5f));
                        AddMarkLine(FVector2D(MaxX, CY - BoxH * 0.5f), FVector2D(MaxX - BoxD, CY - BoxH * 0.5f));
                        AddMarkLine(FVector2D(MaxX, CY + BoxH * 0.5f), FVector2D(MaxX - BoxD, CY + BoxH * 0.5f));
                }
                else if (VT.Contains(TEXT("basketball")))
                {
                        AddMarkLine(FVector2D(CX, MinY), FVector2D(CX, MaxY));
                        const float Key = W * 0.15f;
                        AddMarkLine(FVector2D(MinX + Key, CY - H * 0.2f), FVector2D(MinX + Key, CY + H * 0.2f));
                        AddMarkLine(FVector2D(MaxX - Key, CY - H * 0.2f), FVector2D(MaxX - Key, CY + H * 0.2f));
                }
                else if (VT.Contains(TEXT("tennis")))
                {
                        AddMarkLine(FVector2D(CX, MinY), FVector2D(CX, MaxY));
                        const float Service = H * 0.25f;
                        AddMarkLine(FVector2D(MinX, CY - Service), FVector2D(MaxX, CY - Service));
                        AddMarkLine(FVector2D(MinX, CY + Service), FVector2D(MaxX, CY + Service));
                }
        }

        RecomputeNormals(OutMarkingData);
}

// ============================================================================
// GeneratePOIMarkers — coloured pins for amenity/shop/tourism POIs
// ============================================================================
void FGeoMeshGenerator::GeneratePOIMarkers(
        const TArray<FGeoPOI>& POIs,
        FMeshData& OutPOIData,
        float MarkerRadius,
        float MarkerHeight)
{
        OutPOIData.Reset();
        if (POIs.Num() == 0) return;

        const int32 Sides = 8;
        const float R = FMath::Max(MarkerRadius, 20.f);
        const float H = FMath::Max(MarkerHeight, 50.f);
        const float MarkerOffset = 8.f;

        for (const FGeoPOI& POI : POIs)
        {
                const FColor Col = FGeoPOIHelpers::MarkerColorForPOI(POI).ToFColor(true);
                const float CX = POI.Location.X * ScaleFactor;
                const float CY = -POI.Location.Y * ScaleFactor;
                const double BaseZ = (SampleGroundZ ? SampleGroundZ(FVector2D(CX, CY)) : 0.0) + MarkerOffset;

                const int32 BaseIdx = OutPOIData.Vertices.Num();
                OutPOIData.Vertices.Add(FVector(CX, CY, BaseZ));
                OutPOIData.VertexColors.Add(Col);
                OutPOIData.Normals.Add(FVector::UpVector);
                OutPOIData.UV0.Add(FVector2D(0.5f, 0.5f));
                OutPOIData.UV1.Add(FVector2D::ZeroVector);
                OutPOIData.Tangents.Add(FProcMeshTangent(FVector(1, 0, 0), false));

                for (int32 s = 0; s <= Sides; s++)
                {
                        const float A = (2.f * PI * s) / Sides;
                        const FVector2D Dir(FMath::Cos(A), FMath::Sin(A));
                        OutPOIData.Vertices.Add(FVector(CX + Dir.X * R, CY + Dir.Y * R, BaseZ));
                        OutPOIData.VertexColors.Add(Col);
                        OutPOIData.Normals.Add(FVector::UpVector);
                        OutPOIData.UV0.Add(FVector2D(0.5f + Dir.X * 0.5f, 0.5f + Dir.Y * 0.5f));
                        OutPOIData.UV1.Add(FVector2D::ZeroVector);
                        OutPOIData.Tangents.Add(FProcMeshTangent(FVector(1, 0, 0), false));
                }

                const int32 TopIdx = OutPOIData.Vertices.Num();
                OutPOIData.Vertices.Add(FVector(CX, CY, BaseZ + H));
                OutPOIData.VertexColors.Add(Col);
                OutPOIData.Normals.Add(FVector::UpVector);
                OutPOIData.UV0.Add(FVector2D(0.5f, 1.f));
                OutPOIData.UV1.Add(FVector2D::ZeroVector);
                OutPOIData.Tangents.Add(FProcMeshTangent(FVector(1, 0, 0), false));

                for (int32 s = 0; s < Sides; s++)
                {
                        const int32 A = BaseIdx + 1 + s;
                        const int32 B = BaseIdx + 1 + s + 1;
                        OutPOIData.Triangles.Add(BaseIdx);
                        OutPOIData.Triangles.Add(B);
                        OutPOIData.Triangles.Add(A);
                        OutPOIData.Triangles.Add(TopIdx);
                        OutPOIData.Triangles.Add(A);
                        OutPOIData.Triangles.Add(B);
                }
        }

        RecomputeNormals(OutPOIData);
}

// ============================================================================
// AppendOSMTreeTransforms — place instanced trees at natural=tree nodes
// ============================================================================
void FGeoMeshGenerator::AppendOSMTreeTransforms(
        const TArray<FGeoTree>& Trees,
        TArray<FTransform>& OutTreeTransforms,
        float InScaleFactor,
        float GroundOffset,
        const FHiroGeoHeightmap* DemHeightmap,
        double OriginLatitude, double OriginLongitude,
        const TArray<FElevationSample>* ElevationSamples)
{
        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const bool bHaveSamples = ElevationSamples && ElevationSamples->Num() >= 3;
        const double TreeRefElev = bHaveDem ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : (bHaveSamples ? ComputeElevationSamplesRefElev(*ElevationSamples) : 0.0);

        for (const FGeoTree& Tree : Trees)
        {
                const float WX = Tree.Location.X * InScaleFactor;
                const float WY = -Tree.Location.Y * InScaleFactor;
                const float H = Tree.Height > 0.f ? Tree.Height : 10.f;
                const float S = FMath::Clamp(H / 10.f, 0.5f, 2.5f);
                const int32 Hash = FMath::Abs((int32)(Tree.Location.X * 17.3f + Tree.Location.Y * 11.1f));
                const float Yaw = (float)(Hash % 360);

                // Tree.Location is already in raw local meters (same convention as
                // FGeoWater/FGeoVegetation footprint centroids), so it can be sampled
                // directly — no UE-unit/Y-flip conversion needed here.
                float ZOff = 0.f;
                if (bHaveDem)
                        ZOff = SampleDemOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude,
                                TreeRefElev, InScaleFactor, Tree.Location.X, Tree.Location.Y);
                else if (bHaveSamples)
                        ZOff = SampleElevationSamplesOffsetUE(*ElevationSamples,
                                TreeRefElev, InScaleFactor, Tree.Location.X, Tree.Location.Y);

                OutTreeTransforms.Add(FTransform(
                        FRotator(0.f, Yaw, 0.f),
                        FVector(WX, WY, GroundOffset + 50.f + ZOff),
                        FVector(S)));
        }
}

// ============================================================================
// GenerateVegetationMeshes
// ============================================================================
void FGeoMeshGenerator::GenerateVegetationMeshes(
        const TArray<FGeoVegetation>& Vegetations, FMeshData& OutVegetationData,
        const FHiroGeoHeightmap* DemHeightmap, double OriginLatitude, double OriginLongitude,
        const TArray<FElevationSample>* ElevationSamples)
{
        OutVegetationData.Reset();
        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const bool bHaveSamples = ElevationSamples && ElevationSamples->Num() >= 3;
        const double VegRefElev = bHaveDem ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : (bHaveSamples ? ComputeElevationSamplesRefElev(*ElevationSamples) : 0.0);

        for (const FGeoVegetation& Veg : Vegetations)
        {
                if (Veg.Points.Num() < 3) continue;
                const TArray<FVector2D> RPts = ResamplePolylineForTerrain(Veg.Points, 10.f, /*bClosed*/ true);
                TArray<FVector> LP;
                for (const FVector2D& P : RPts)
                        LP.Add(FVector(P.X * ScaleFactor, -P.Y * ScaleFactor, 0.f));
                // Use the per-area colour from OSM (green for grass/forest, brown for
                // construction/brownfield) instead of a single hard-coded green, so bare
                // ground does not read as a lawn.
                FColor VC = Veg.Color.ToFColor(false);
                const int32 VStart = OutVegetationData.Vertices.Num();
                if (Veg.Holes.Num() > 0)
                {
                        TArray<FVector2D> Outer;
                        for (const FVector& P : LP) Outer.Add(FVector2D(P.X, P.Y));
                        TArray<TArray<FVector2D>> Holes;
                        for (const TArray<FVector2D>& SourceHole : Veg.Holes)
                        {
                                TArray<FVector2D>& Hole = Holes.AddDefaulted_GetRef();
                                for (const FVector2D& P : SourceHole)
                                        Hole.Add(FVector2D(P.X * ScaleFactor, -P.Y * ScaleFactor));
                        }
                        if (!AddPolygonWithHolesTriangulated(OutVegetationData, Outer, Holes, 5.f, VC))
                        {
                                UE_LOG(LogTemp, Warning, TEXT("HiroGeo: skipped vegetation polygon '%s' because hole-aware triangulation failed"), *Veg.Name);
                                continue;
                        }
                }
                else
                {
                        AddPolygonTriangulated(OutVegetationData, LP, 5.f, VC);
                }
                // Per-area surface-atlas slice: bare construction/brownfield/landfill
                // ground uses the same paved slice (2) as sidewalks (M_Area is shared
                // between sidewalk + vegetation), everything else stays grass (3).
                const bool bBareGround = (Veg.VegType == TEXT("construction")
                        || Veg.VegType == TEXT("brownfield") || Veg.VegType == TEXT("landfill")
                        || Veg.VegType == TEXT("sand") || Veg.VegType == TEXT("beach")
                        || Veg.VegType == TEXT("rock") || Veg.VegType == TEXT("bare_rock")
                        || Veg.VegType == TEXT("scree") || Veg.VegType == TEXT("farmland"));
                FillSliceUV1(OutVegetationData, VStart, bBareGround ? 2.f : 3.f);

                if (SampleGroundZ || bHaveDem || bHaveSamples)
                {
                        for (int32 Index = VStart; Index < OutVegetationData.Vertices.Num(); ++Index)
                        {
                                FVector& Vertex = OutVegetationData.Vertices[Index];
                                const double LocalX = Vertex.X / ScaleFactor;
                                const double LocalY = -Vertex.Y / ScaleFactor;
                                Vertex.Z += SampleGroundZ ? SampleGroundZ(FVector2D(Vertex.X, Vertex.Y))
                                        : bHaveDem
                                        ? SampleDemOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude,
                                                VegRefElev, ScaleFactor, LocalX, LocalY)
                                        : SampleElevationSamplesOffsetUE(*ElevationSamples, VegRefElev,
                                                ScaleFactor, LocalX, LocalY);
                        }
                }
        }
        RecomputeNormals(OutVegetationData);
        ProjectOntoGround(OutVegetationData);
}

// ============================================================================
// GenerateVegetationTransforms
// ============================================================================
void FGeoMeshGenerator::GenerateVegetationTransforms(
        const TArray<FGeoVegetation>& Vegetations,
        const TArray<FGeoBuilding>& Buildings,
        const TArray<FGeoRoad>& Roads,
        TArray<FTransform>& OutTreeTransforms,
        TArray<FTransform>& OutGrassTransforms,
        float TreeDensityScale, float GrassDensityScale,
        const FHiroGeoHeightmap* DemHeightmap, double OriginLatitude, double OriginLongitude,
        const TArray<FElevationSample>* ElevationSamples)
{
        OutTreeTransforms.Empty(); OutGrassTransforms.Empty();
        TreeDensityScale = FMath::Max(TreeDensityScale, 0.01f);
        GrassDensityScale = FMath::Max(GrassDensityScale, 0.01f);

        const float VegetationBaseZ = 50.f;

        const bool bHaveDem = DemHeightmap && DemHeightmap->IsValid();
        const bool bHaveSamples = ElevationSamples && ElevationSamples->Num() >= 3;
        const double VegXfRefElev = bHaveDem ? DemHeightmap->SampleAtLatLon(OriginLatitude, OriginLongitude)
                : (bHaveSamples ? ComputeElevationSamplesRefElev(*ElevationSamples) : 0.0);

        // Sample the terrain under a scattered instance, given its position in UE
        // units (world X/Y as already used for PX/PY below). Falls back to a flat
        // VegetationBaseZ when no DEM/sample data is available.
        auto SampleVegZ = [&](float UEX, float UEY) -> float
        {
                if (!bHaveDem && !bHaveSamples) return VegetationBaseZ;
                const float LocalXM = UEX / ScaleFactor;
                const float LocalYM = -UEY / ScaleFactor;
                const float ZOff = bHaveDem
                        ? SampleDemOffsetUE(DemHeightmap, OriginLatitude, OriginLongitude,
                                VegXfRefElev, ScaleFactor, LocalXM, LocalYM)
                        : SampleElevationSamplesOffsetUE(*ElevationSamples, VegXfRefElev,
                                ScaleFactor, LocalXM, LocalYM);
                return VegetationBaseZ + ZOff;
        };

        // Build building polygon list for collision check
        TArray<TArray<FVector2D>> BldgPolys;
        for (const FGeoBuilding& B : Buildings)
        {
                TArray<FVector2D> Poly;
                for (const FVector2D& N : B.Nodes) Poly.Add(FVector2D(N.X * ScaleFactor, -N.Y * ScaleFactor));
                BldgPolys.Add(Poly);
        }

        // Build road segment list for collision check
        struct FRS { FVector2D A, B; float HW; };
        TArray<FRS> RoadSegs;
        for (const FGeoRoad& R : Roads)
        {
                if (R.Points.Num() < 2) continue;
                float HW = (R.Width > 0.f ? R.Width * ScaleFactor : 600.f) * 0.5f;
                for (int32 i = 0; i < R.Points.Num() - 1; i++)
                        RoadSegs.Add({ FVector2D(R.Points[i].X * ScaleFactor, -R.Points[i].Y * ScaleFactor),
                                FVector2D(R.Points[i + 1].X * ScaleFactor, -R.Points[i + 1].Y * ScaleFactor), HW });
        }

        // Point-in-polygon test
        auto InPoly = [](const FVector2D& P, const TArray<FVector2D>& Poly) -> bool
        {
                int32 N = Poly.Num(); bool Inside = false;
                for (int32 i = 0, j = N - 1; i < N; j = i++)
                        if (((Poly[i].Y > P.Y) != (Poly[j].Y > P.Y)) && (P.X < (Poly[j].X - Poly[i].X) * (P.Y - Poly[i].Y) / (Poly[j].Y - Poly[i].Y) + Poly[i].X))
                                Inside = !Inside;
                return Inside;
        };

        auto InAnyBldg = [&](const FVector2D& P) -> bool { for (const auto& Poly : BldgPolys) if (InPoly(P, Poly)) return true; return false; };

        auto OnRoad = [&](const FVector2D& P) -> bool
        {
                for (const FRS& S : RoadSegs)
                {
                        FVector2D AB = S.B - S.A, AP = P - S.A;
                        float L = AB.Size(); if (L < 1.f) continue;
                        float T = FMath::Clamp(FVector2D::DotProduct(AP, AB) / (L * L), 0.f, 1.f);
                        if (FVector2D::Distance(P, S.A + AB * T) < S.HW) return true;
                }
                return false;
        };

        for (const FGeoVegetation& Veg : Vegetations)
        {
                if (Veg.Points.Num() < 3) continue;
                // Bare-ground / non-forest areas — never scatter grass blades or trees over them.
                if (Veg.VegType == TEXT("construction") || Veg.VegType == TEXT("brownfield")
                        || Veg.VegType == TEXT("landfill") || Veg.VegType == TEXT("sand")
                        || Veg.VegType == TEXT("beach") || Veg.VegType == TEXT("rock")
                        || Veg.VegType == TEXT("bare_rock") || Veg.VegType == TEXT("scree")
                        || Veg.VegType == TEXT("farmland") || Veg.VegType == TEXT("pitch")
                        || Veg.VegType == TEXT("playground"))
                        continue;
                float MnX = FLT_MAX, MxX = -FLT_MAX, MnY = FLT_MAX, MxY = -FLT_MAX;
                TArray<FVector2D> VP;
                for (const FVector2D& P : Veg.Points)
                {
                        float UX = P.X * ScaleFactor, UY = -P.Y * ScaleFactor;
                        VP.Add(FVector2D(UX, UY));
                        if (UX < MnX) MnX = UX; if (UX > MxX) MxX = UX;
                        if (UY < MnY) MnY = UY; if (UY > MxY) MxY = UY;
                }
                TArray<TArray<FVector2D>> VPHoles;
                for (const TArray<FVector2D>& SourceHole : Veg.Holes)
                {
                        TArray<FVector2D>& Hole = VPHoles.AddDefaulted_GetRef();
                        for (const FVector2D& P : SourceHole)
                                Hole.Add(FVector2D(P.X * ScaleFactor, -P.Y * ScaleFactor));
                }
                auto InVegetationHole = [&](const FVector2D& P) -> bool
                {
                        for (const TArray<FVector2D>& Hole : VPHoles)
                                if (InPoly(P, Hole)) return true;
                        return false;
                };
                float AX = MxX - MnX, AY = MxY - MnY;
                if (AX <= 0.f || AY <= 0.f) continue;

                // Scatter tree transforms
                float TS = 800.f / TreeDensityScale;
                int32 NTX = FMath::Max(1, FMath::FloorToInt(AX / TS)), NTY = FMath::Max(1, FMath::FloorToInt(AY / TS));
                for (int32 tx = 0; tx < NTX; tx++) for (int32 ty = 0; ty < NTY; ty++)
                {
                        float PX = MnX + TS * ((float)tx + 0.5f) + FMath::FRandRange(-TS * 0.2f, TS * 0.2f);
                        float PY = MnY + TS * ((float)ty + 0.5f) + FMath::FRandRange(-TS * 0.2f, TS * 0.2f);
                        FVector2D TP(PX, PY);
                        if (!InPoly(TP, VP) || InVegetationHole(TP) || InAnyBldg(TP) || OnRoad(TP)) continue;
                        float S = FMath::FRandRange(0.7f, 1.3f);
                        FTransform X(FVector(PX, PY, SampleVegZ(PX, PY))); X.SetScale3D(FVector(S)); OutTreeTransforms.Add(X);
                }

                // Scatter grass transforms
                float GS = 200.f / GrassDensityScale;
                int32 NGX = FMath::Max(1, FMath::FloorToInt(AX / GS)), NGY = FMath::Max(1, FMath::FloorToInt(AY / GS));
                for (int32 gx = 0; gx < NGX; gx++) for (int32 gy = 0; gy < NGY; gy++)
                {
                        float PX = MnX + GS * ((float)gx + 0.5f) + FMath::FRandRange(-GS * 0.3f, GS * 0.3f);
                        float PY = MnY + GS * ((float)gy + 0.5f) + FMath::FRandRange(-GS * 0.3f, GS * 0.3f);
                        FVector2D TP(PX, PY);
                        if (!InPoly(TP, VP) || InVegetationHole(TP) || InAnyBldg(TP) || OnRoad(TP)) continue;
                        float S = FMath::FRandRange(0.5f, 1.5f), R = FMath::FRandRange(0.f, 360.f);
                        OutGrassTransforms.Add(FTransform(FRotator(0, R, 0), FVector(PX, PY, SampleVegZ(PX, PY)), FVector(S)));
                }
        }
}
