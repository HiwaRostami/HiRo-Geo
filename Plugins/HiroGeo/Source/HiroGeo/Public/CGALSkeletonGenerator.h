// Copyright Hiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// ============================================================================
// FCGALSkeletonVertex - A vertex in the CGAL straight skeleton
// Position is in 2D local coordinates, Time is the skeleton distance
// from the polygon boundary (0 for boundary vertices, >0 for interior).
// ============================================================================
struct FCGALSkeletonVertex
{
	FVector2D Position;     // 2D position in local coordinates
	float Time;             // Skeleton time (distance from boundary)
	bool bIsBoundary;       // True if this is a boundary vertex (time=0)

	FCGALSkeletonVertex()
		: Position(FVector2D::ZeroVector), Time(0.f), bIsBoundary(true) {}

	FCGALSkeletonVertex(const FVector2D& InPos, float InTime, bool bInBoundary)
		: Position(InPos), Time(InTime), bIsBoundary(bInBoundary) {}
};

// ============================================================================
// FCGALSkeletonEdge - An edge in the CGAL straight skeleton
// Vertices are indices into FCGALSkeletonResult.Vertices array.
// Bisector edges are interior skeleton edges (not on the polygon boundary).
// ============================================================================
struct FCGALSkeletonEdge
{
	int32 VertexA;          // Index into FCGALSkeletonResult.Vertices
	int32 VertexB;          // Index into FCGALSkeletonResult.Vertices
	bool bIsBisector;       // True if this is a bisector edge (not a boundary edge)

	FCGALSkeletonEdge()
		: VertexA(-1), VertexB(-1), bIsBisector(false) {}

	FCGALSkeletonEdge(int32 InA, int32 InB, bool bInBisector)
		: VertexA(InA), VertexB(InB), bIsBisector(bInBisector) {}
};

// ============================================================================
// FCGALSkeletonResult - Result of CGAL straight skeleton generation
// Contains all skeleton vertices and edges, plus MaxTime for height
// normalization. Boundary vertices have Time=0, interior vertices have
// Time>0 proportional to their distance from the polygon boundary.
// ============================================================================
// ============================================================================
// FCGALSkeletonFace - One roof panel of the straight skeleton, corresponding
// to a single footprint edge. VertexIndices index into
// FCGALSkeletonResult.Vertices and list the face boundary in order.
// ============================================================================
struct FCGALSkeletonFace
{
	TArray<int32> VertexIndices;
};

struct FCGALSkeletonResult
{
	TArray<FCGALSkeletonVertex> Vertices;
	TArray<FCGALSkeletonEdge> Edges;
	TArray<FCGALSkeletonFace> Faces;
	float MaxTime;          // Maximum skeleton time (for height normalization)

	FCGALSkeletonResult() : MaxTime(0.f) {}

	bool IsValid() const { return Vertices.Num() > 0 && MaxTime > 0.01f; }
};

// ============================================================================
// FCGALSkeletonGenerator - Generates straight skeletons using CGAL
// Also provides CGAL-based Constrained Delaunay Triangulation for
// robust polygon triangulation with Steiner point insertion.
// ============================================================================
class HIROGEO_API FCGALSkeletonGenerator
{
public:
	/** Generate a straight skeleton for the given polygon.
	 *  Returns the skeleton result with vertices, edges, and timing info. */
	static FCGALSkeletonResult GenerateSkeleton(const TArray<FVector2D>& Polygon);

	/** Straight skeleton for a polygon WITH HOLES (inner courtyards). */
	static FCGALSkeletonResult GenerateSkeleton(
		const TArray<FVector2D>& Polygon,
		const TArray<TArray<FVector2D>>& Holes);

	/** Triangulate a 2D polygon using CGAL Constrained Delaunay Triangulation.
	 *  OutTriangles contains vertex indices (into combined Poly+SteinerPoints array).
	 *  OutSteinerPoints receives any Steiner points inserted by the CDT. */
	static void TriangulateWithCGAL(
		const TArray<FVector2D>& Polygon,
		TArray<int32>& OutTriangles,
		TArray<FVector2D>& OutSteinerPoints);

	/** Triangulate a 2D polygon WITH HOLES (courtyards excluded). Indices are
	 *  into the combined (Polygon ++ OutSteinerPoints) array, where hole
	 *  vertices appear among the Steiner points. */
	static void TriangulateWithCGAL(
		const TArray<FVector2D>& Polygon,
		const TArray<TArray<FVector2D>>& Holes,
		TArray<int32>& OutTriangles,
		TArray<FVector2D>& OutSteinerPoints);
};
