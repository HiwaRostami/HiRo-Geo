// Copyright Hiro. All Rights Reserved.

// ========================================================================
// CGALSkeletonImpl.h - Pure C++ Bridge Header
// NO UE5 headers, NO CGAL headers - only standard C++ types
// This file is the ONLY interface between HiroGeo (UE5) and HiroGeoCGAL
// ========================================================================
#pragma once

#include <vector>

// ========================================================================
// Export macro for DLL boundary
// When compiled inside HiroGeoCGAL module: CGALBRIDGE_IMPL is defined
//   -> __declspec(dllexport)
// When included from HiroGeo module: CGALBRIDGE_IMPL is NOT defined
//   -> __declspec(dllimport)
// ========================================================================
#ifndef CGALBRIDGE_API
    #ifdef CGALBRIDGE_IMPL
        #define CGALBRIDGE_API __declspec(dllexport)
    #else
        #define CGALBRIDGE_API __declspec(dllimport)
    #endif
#endif

// ========================================================================
// C++ Structs - Pure data, no UE types, no CGAL types
// ========================================================================

/** A vertex of the straight skeleton with 2D position and time value.
 *  Time = distance from polygon boundary (skeleton wavefront time).
 *  Higher time = further from boundary = higher roof point. */
struct CGALSkeletonVertex
{
    double X;
    double Y;
    double Time;
};

/** An edge of the straight skeleton connecting two vertices.
 *  Stores indices into the vertex array. */
struct CGALSkeletonEdge
{
    int VertexA;
    int VertexB;
    int IsBisector;  // 1 = interior bisector edge, 0 = boundary edge
};

/** A face of the straight skeleton. Each face corresponds to exactly ONE
 *  input contour (footprint) edge, and is the sloped roof panel rising from
 *  that edge up to the skeleton ridge. VertexIndices lists the face boundary
 *  vertices (indices into CGALSkeletonResult.Vertices) in order. */
struct CGALSkeletonFace
{
    std::vector<int> VertexIndices;
};

/** Complete straight skeleton result with vertices, edges and faces. */
struct CGALSkeletonResult
{
    std::vector<CGALSkeletonVertex> Vertices;
    std::vector<CGALSkeletonEdge>   Edges;
    std::vector<CGALSkeletonFace>   Faces;
    double MaxTime;

    CGALSkeletonResult() : MaxTime(0.0) {}
};

/** A pair of 2D points representing a skeleton edge (legacy format). */
struct CGALSkeletonEdgePair
{
    double AX, AY;  // Point A
    double BX, BY;  // Point B
};

/** Result of constrained Delaunay triangulation. */
struct CGALTriangulationResult
{
    int OriginalVertexCount;             // Number of original polygon vertices
    std::vector<int>    Triangles;       // Triangle indices (3 per triangle)
    std::vector<double> SteinerPoints;   // Steiner points as [X0,Y0, X1,Y1, ...]

    CGALTriangulationResult() : OriginalVertexCount(0) {}
};

// ========================================================================
// Bridge Functions - Implemented in HiroGeoCGAL module
// These use C++ linkage (NOT extern "C") because of std::vector parameters
// ========================================================================

/** Generate the straight skeleton of a polygon with full vertex time data.
 *  @param PolygonPoints  Flat array of coordinates [X0,Y0, X1,Y1, ...]
 *  @return               Complete skeleton with vertices (position + time) and edges */
CGALBRIDGE_API CGALSkeletonResult CGAL_GenerateSkeleton(
    const std::vector<double>& PolygonPoints);

/** Straight skeleton of a polygon WITH HOLES (inner courtyards).
 *  @param OuterPoints  Outer ring [X0,Y0, ...]
 *  @param Holes        Each inner ring as a flat [X0,Y0, ...] array */
CGALBRIDGE_API CGALSkeletonResult CGAL_GenerateSkeletonWithHoles(
    const std::vector<double>& OuterPoints,
    const std::vector<std::vector<double>>& Holes);

/** Legacy method: Generate skeleton edges as position pairs (no time data). */
CGALBRIDGE_API std::vector<CGALSkeletonEdgePair> CGAL_GenerateSkeletonEdges(
    const std::vector<double>& PolygonPoints);

/** Triangulate a 2D polygon using CGAL's constrained Delaunay triangulation.
 *  @param PolygonPoints  Flat array of coordinates [X0,Y0, X1,Y1, ...]
 *  @return               Triangulation result with triangles + optional Steiner points */
CGALBRIDGE_API CGALTriangulationResult CGAL_Triangulate(
    const std::vector<double>& PolygonPoints);

/** Triangulate a 2D polygon WITH HOLES (courtyards excluded via nesting depth).
 *  @param OuterPoints  Outer ring [X0,Y0, ...]
 *  @param Holes        Each inner ring as a flat [X0,Y0, ...] array */
CGALBRIDGE_API CGALTriangulationResult CGAL_TriangulateWithHoles(
    const std::vector<double>& OuterPoints,
    const std::vector<std::vector<double>>& Holes);
