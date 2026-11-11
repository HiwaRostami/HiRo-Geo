// Copyright Hiro. All Rights Reserved.

#include "CGALSkeletonGenerator.h"
#include "CoreMinimal.h"

// ============================================================================
// This translation unit is now a THIN ADAPTER.
//
// All CGAL work happens inside the isolated HiroGeoCGAL module, behind a
// pure-C++ ABI (CGALSkeletonImpl.h - no UE types, no CGAL types). Here we
// only translate between that ABI and the UE-facing FCGAL* structs.
//
// IMPORTANT: Do NOT include any <CGAL/...> header in this file. CGAL must
// stay isolated in HiroGeoCGAL, which is compiled with RTTI + exceptions +
// no-unity. Pulling CGAL back into the HiroGeo module would re-introduce
// the build-flag mismatch the bridge was built to avoid.
// ============================================================================
#include "CGALSkeletonImpl.h"   // Pure-C++ bridge (std::vector only)

// ============================================================================
// GenerateSkeleton - delegate to CGAL_GenerateSkeleton in the bridge module
// ============================================================================
// Local helper: flatten a UE ring to [X0,Y0, X1,Y1, ...] for the bridge ABI.
static void FlattenRing(const TArray<FVector2D>& Ring, std::vector<double>& Out)
{
	Out.clear();
	Out.reserve(static_cast<size_t>(Ring.Num()) * 2);
	for (const FVector2D& P : Ring)
	{
		Out.push_back(static_cast<double>(P.X));
		Out.push_back(static_cast<double>(P.Y));
	}
}

static std::vector<std::vector<double>> FlattenHoles(const TArray<TArray<FVector2D>>& Holes)
{
	std::vector<std::vector<double>> Out;
	Out.reserve(Holes.Num());
	for (const TArray<FVector2D>& H : Holes)
	{
		if (H.Num() < 3) continue;
		std::vector<double> FH;
		FlattenRing(H, FH);
		Out.push_back(std::move(FH));
	}
	return Out;
}

FCGALSkeletonResult FCGALSkeletonGenerator::GenerateSkeleton(
	const TArray<FVector2D>& InPolygon)
{
	return GenerateSkeleton(InPolygon, TArray<TArray<FVector2D>>());
}

FCGALSkeletonResult FCGALSkeletonGenerator::GenerateSkeleton(
	const TArray<FVector2D>& InPolygon,
	const TArray<TArray<FVector2D>>& Holes)
{
	FCGALSkeletonResult Result;

	if (InPolygon.Num() < 3)
	{
		return Result;
	}

	std::vector<double> Flat;
	FlattenRing(InPolygon, Flat);
	const std::vector<std::vector<double>> FlatHoles = FlattenHoles(Holes);

	// The bridge swallows CGAL exceptions internally and returns an empty
	// result for degenerate / non-simple polygons.
	const CGALSkeletonResult Skel = CGAL_GenerateSkeletonWithHoles(Flat, FlatHoles);

	if (Skel.Vertices.empty())
	{
		UE_LOG(LogTemp, Verbose, TEXT("CGAL Skeleton: empty result (degenerate or non-simple polygon)"));
		return Result;
	}

	Result.MaxTime = static_cast<float>(Skel.MaxTime);

	Result.Vertices.Reserve(static_cast<int32>(Skel.Vertices.size()));
	for (const CGALSkeletonVertex& V : Skel.Vertices)
	{
		const float Time = static_cast<float>(V.Time);
		Result.Vertices.Add(FCGALSkeletonVertex(
			FVector2D(static_cast<float>(V.X), static_cast<float>(V.Y)),
			Time,
			/*bIsBoundary=*/ Time < 0.001f));
	}

	Result.Edges.Reserve(static_cast<int32>(Skel.Edges.size()));
	for (const CGALSkeletonEdge& E : Skel.Edges)
	{
		Result.Edges.Add(FCGALSkeletonEdge(E.VertexA, E.VertexB, E.IsBisector != 0));
	}

	Result.Faces.Reserve(static_cast<int32>(Skel.Faces.size()));
	for (const CGALSkeletonFace& F : Skel.Faces)
	{
		FCGALSkeletonFace Face;
		Face.VertexIndices.Reserve(static_cast<int32>(F.VertexIndices.size()));
		for (int Idx : F.VertexIndices)
			Face.VertexIndices.Add(Idx);
		Result.Faces.Add(MoveTemp(Face));
	}

	UE_LOG(LogTemp, Verbose,
		TEXT("Generated CGAL Skeleton: %d vertices, %d edges, maxTime=%.1f"),
		Result.Vertices.Num(), Result.Edges.Num(), Result.MaxTime);

	return Result;
}

// ============================================================================
// TriangulateWithCGAL - delegate to CGAL_Triangulate in the bridge module.
//
// The bridge performs Constrained Delaunay Triangulation AND a flood-fill
// inside/outside classification, so only interior faces are returned. This
// is correct for concave footprints (L/U-shaped buildings, courtyards),
// where the previous in-module implementation incorrectly emitted exterior
// triangles for every finite face.
//
// Index convention (matches the bridge):
//   indices [0, Polygon.Num())                 -> original polygon vertices
//   indices [Polygon.Num(), +SteinerPoints)    -> Steiner points
// Callers combine (Polygon ++ OutSteinerPoints) to resolve indices.
// ============================================================================
void FCGALSkeletonGenerator::TriangulateWithCGAL(
	const TArray<FVector2D>& Polygon,
	TArray<int32>& OutTriangles,
	TArray<FVector2D>& OutSteinerPoints)
{
	TriangulateWithCGAL(Polygon, TArray<TArray<FVector2D>>(), OutTriangles, OutSteinerPoints);
}

void FCGALSkeletonGenerator::TriangulateWithCGAL(
	const TArray<FVector2D>& Polygon,
	const TArray<TArray<FVector2D>>& Holes,
	TArray<int32>& OutTriangles,
	TArray<FVector2D>& OutSteinerPoints)
{
	OutTriangles.Empty();
	OutSteinerPoints.Empty();

	if (Polygon.Num() < 3)
	{
		return;
	}

	std::vector<double> Flat;
	FlattenRing(Polygon, Flat);
	const std::vector<std::vector<double>> FlatHoles = FlattenHoles(Holes);

	const CGALTriangulationResult Tri = CGAL_TriangulateWithHoles(Flat, FlatHoles);

	// Steiner points are returned flat as [X0,Y0, X1,Y1, ...].
	const int32 NumSteiner = static_cast<int32>(Tri.SteinerPoints.size() / 2);
	OutSteinerPoints.Reserve(NumSteiner);
	for (int32 i = 0; i < NumSteiner; i++)
	{
		OutSteinerPoints.Add(FVector2D(
			static_cast<float>(Tri.SteinerPoints[i * 2]),
			static_cast<float>(Tri.SteinerPoints[i * 2 + 1])));
	}

	const int32 TotalVertices = Polygon.Num() + OutSteinerPoints.Num();

	OutTriangles.Reserve(static_cast<int32>(Tri.Triangles.size()));
	// Copy in triples, validating indices so a malformed result can never
	// produce out-of-range indices downstream (guards the historic OOM crash).
	for (size_t i = 0; i + 2 < Tri.Triangles.size(); i += 3)
	{
		const int32 V0 = Tri.Triangles[i];
		const int32 V1 = Tri.Triangles[i + 1];
		const int32 V2 = Tri.Triangles[i + 2];

		if (V0 >= 0 && V0 < TotalVertices &&
			V1 >= 0 && V1 < TotalVertices &&
			V2 >= 0 && V2 < TotalVertices &&
			V0 != V1 && V0 != V2 && V1 != V2)
		{
			OutTriangles.Add(V0);
			OutTriangles.Add(V1);
			OutTriangles.Add(V2);
		}
	}

	UE_LOG(LogTemp, Verbose,
		TEXT("CGAL CDT: %d input verts, %d Steiner points, %d triangles"),
		Polygon.Num(), OutSteinerPoints.Num(), OutTriangles.Num() / 3);
}
