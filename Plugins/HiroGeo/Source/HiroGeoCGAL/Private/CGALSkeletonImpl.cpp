// Copyright Hiro. All Rights Reserved.

#include "CGALSkeletonImpl.h"  // First include - satisfies UBT "first header" rule

// ========================================================================
// CRITICAL: Undefine NDEBUG before including CGAL headers
// UE5 defines NDEBUG in Development/Shipping, which triggers
// CGAL's test-suite #error check.
// ========================================================================
#ifdef NDEBUG
#undef NDEBUG
#endif

// ========================================================================
// CGAL Configuration - MUST be before any CGAL #include
// ========================================================================
#ifndef CGAL_DISABLE_ROUNDING_MATH_CHECK
#define CGAL_DISABLE_ROUNDING_MATH_CHECK 1
#endif
#ifndef CGAL_NO_DEPRECATED_CODE
#define CGAL_NO_DEPRECATED_CODE 1
#endif
#ifndef CGAL_NO_ASSERTIONS
#define CGAL_NO_ASSERTIONS 1
#endif

// Force-disable CGAL_TEST_SUITE
#ifdef CGAL_TEST_SUITE
#undef CGAL_TEST_SUITE
#endif
#define CGAL_TEST_SUITE 0

// Prevent CGAL from trying to auto-detect compiler features
// that trigger "not defined as preprocessor macro" warnings
#ifndef _M_IX86_FP
#define _M_IX86_FP 2  // MSVC x64 default: SSE2
#endif

// ========================================================================
// Suppress all MSVC warnings from CGAL headers
// C4668 = 'macro' is not defined as preprocessor macro
// ========================================================================
#pragma warning(push)
#pragma warning(disable: 4668 4996 4512 4267 4244 4702 4005 4456 4458 4457 4189 4701 4505 4250 4275 4661 4703 4459 4626 4625 4624 4464)

// ========================================================================
// STEP 1: Include CGAL's config.h FIRST
// This is auto-generated and may define CGAL_USE_LEDA=1 if it was
// configured that way. We'll override it right after.
// ========================================================================
#include <CGAL/config.h>

// ========================================================================
// STEP 2: Force-UNDEFINE LEDA and other features we don't have
// Using #undef instead of #define 0 because CGAL checks with
// both #if and #ifdef — #define 0 would make #ifdef TRUE!
// ========================================================================
#ifdef CGAL_USE_LEDA
#undef CGAL_USE_LEDA
#endif
#ifdef CGAL_USE_CORE
#undef CGAL_USE_CORE
#endif

// ========================================================================
// STEP 3: Now include the actual CGAL headers
// ========================================================================
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Polygon_2.h>
#include <CGAL/Polygon_with_holes_2.h>
#include <CGAL/create_straight_skeleton_2.h>
#include <CGAL/create_straight_skeleton_from_polygon_with_holes_2.h>
#include <CGAL/Straight_skeleton_2.h>
#include <CGAL/Straight_skeleton_face_base_2.h>
#include <CGAL/Straight_skeleton_vertex_base_2.h>
#include <CGAL/Straight_skeleton_halfedge_base_2.h>
#include <CGAL/Constrained_Delaunay_triangulation_2.h>
#include <CGAL/Constrained_triangulation_plus_2.h>
#include <CGAL/Triangulation_face_base_with_info_2.h>
#include <CGAL/Polygon_2_algorithms.h>

#include <memory>
#include <vector>
#include <map>
#include <queue>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <iostream>

#pragma warning(pop)

// ========================================================================
// CGAL Type Definitions
// ========================================================================

typedef CGAL::Exact_predicates_inexact_constructions_kernel K;
typedef K::Point_2    Point_2;
// NOTE: Do NOT typedef FT at global scope - it conflicts with UE5's FT.
// Use K::FT directly where needed inside function bodies.
// typedef K::FT FT;  // REMOVED: causes 'FT hides global declaration'

typedef CGAL::Polygon_2<K>       Polygon_2;

// Straight Skeleton types
typedef CGAL::Straight_skeleton_2<K>  Skeleton_2;
typedef std::shared_ptr<Skeleton_2>   SkeletonPtr;

typedef CGAL::Polygon_with_holes_2<K> Polygon_with_holes_2;

// CDT face info stores the polygon "nesting level" (distance in constrained-edge
// crossings from the infinite face). Odd nesting = inside the polygon; even
// nesting (0 outside, 2 = inside a hole, ...) = outside. This is the standard
// CGAL "mark domains" approach and handles polygons WITH HOLES correctly.
struct FaceInfo
{
    int Nesting;
    FaceInfo() : Nesting(-1) {}
    bool IsInside() const { return Nesting >= 0 && (Nesting % 2) == 1; }
};

typedef CGAL::Triangulation_vertex_base_2<K>                           VB;
typedef CGAL::Constrained_triangulation_face_base_2<K>                 CFB_Base;
typedef CGAL::Triangulation_face_base_with_info_2<FaceInfo, K>         FBWI;
typedef CGAL::Constrained_triangulation_face_base_2<K, FBWI>           CFB;
typedef CGAL::Triangulation_data_structure_2<VB, CFB>                  TDS;
typedef CGAL::Constrained_Delaunay_triangulation_2<K, TDS>             CDT;
typedef CDT::Face_handle                                               FaceHandle;

// ========================================================================
// Helper: build a CGAL ring (simple polygon) from a flat coordinate array.
// Removes a duplicate closing vertex; returns false if not a valid simple ring.
// Orientation is left to the caller (outer = CCW, hole = CW).
// ========================================================================
static bool BuildRing(const std::vector<double>& Points, Polygon_2& OutPoly)
{
    OutPoly.clear();
    for (size_t i = 0; i + 1 < Points.size(); i += 2)
        OutPoly.push_back(Point_2(Points[i], Points[i + 1]));

    if (OutPoly.size() >= 2 &&
        CGAL::squared_distance(OutPoly.vertex(0), OutPoly.vertex(OutPoly.size() - 1)) < 0.01)
    {
        Polygon_2 Clean;
        for (auto it = OutPoly.vertices_begin(); it != OutPoly.vertices_end() - 1; ++it)
            Clean.push_back(*it);
        OutPoly = Clean;
    }

    return OutPoly.size() >= 3 && OutPoly.is_simple();
}

// ========================================================================
// GenerateSkeleton (with optional holes) - full skeleton with vertex times.
// CGAL's straight skeleton natively supports a Polygon_with_holes_2, which
// gives correct hipped/gabled roofs around inner courtyards.
// ========================================================================
CGALSkeletonResult CGAL_GenerateSkeletonWithHoles(
    const std::vector<double>& OuterPoints,
    const std::vector<std::vector<double>>& Holes)
{
    CGALSkeletonResult Result;
    Result.MaxTime = 0.0;

    Polygon_2 Outer;
    if (!BuildRing(OuterPoints, Outer))
        return Result;
    if (Outer.orientation() != CGAL::COUNTERCLOCKWISE)
        Outer.reverse_orientation();

    SkeletonPtr Skeleton;
    try
    {
        if (Holes.empty())
        {
            Skeleton = CGAL::create_interior_straight_skeleton_2(
                Outer.vertices_begin(), Outer.vertices_end());
        }
        else
        {
            Polygon_with_holes_2 PWH(Outer);
            for (const std::vector<double>& H : Holes)
            {
                Polygon_2 Hole;
                if (!BuildRing(H, Hole))
                    continue;
                // Holes must be clockwise inside a Polygon_with_holes_2.
                if (Hole.orientation() != CGAL::CLOCKWISE)
                    Hole.reverse_orientation();
                PWH.add_hole(Hole);
            }
            Skeleton = CGAL::create_interior_straight_skeleton_2(PWH);
        }
    }
    catch (...)
    {
        return Result;
    }

    if (!Skeleton)
        return Result;

    // Build vertex map: CGAL vertex pointer -> our index
    std::map<const Skeleton_2::Vertex*, int> VertexToIndex;
    int NextIdx = 0;

    for (auto VIt = Skeleton->vertices_begin(); VIt != Skeleton->vertices_end(); ++VIt)
    {
        const Point_2& P = VIt->point();
        double Time = CGAL::to_double(VIt->time());

        CGALSkeletonVertex V;
        V.X = CGAL::to_double(P.x());
        V.Y = CGAL::to_double(P.y());
        V.Time = Time;

        int Idx = NextIdx++;
        Result.Vertices.push_back(V);
        VertexToIndex[&*VIt] = Idx;

        if (Time > Result.MaxTime)
            Result.MaxTime = Time;
    }

    // Extract edges (only once per halfedge pair)
    for (auto HIt = Skeleton->halfedges_begin(); HIt != Skeleton->halfedges_end(); ++HIt)
    {
        if (HIt->id() > HIt->opposite()->id())
            continue;

        auto ItA = VertexToIndex.find(&*(HIt->vertex()));
        auto ItB = VertexToIndex.find(&*(HIt->opposite()->vertex()));

        if (ItA == VertexToIndex.end() || ItB == VertexToIndex.end())
            continue;

        CGALSkeletonEdge E;
        E.VertexA = ItA->second;
        E.VertexB = ItB->second;
        E.IsBisector = HIt->is_bisector() ? 1 : 0;
        Result.Edges.push_back(E);
    }

    // ---- Extract FACES ----
    // Each straight-skeleton face corresponds to one input contour edge and is
    // the sloped roof panel above that edge. Walk the halfedge cycle of every
    // face, collecting its boundary vertices in order. Building the roof from
    // these faces (one polygon per footprint edge, all meeting at the ridge)
    // gives a watertight, gap-free roof for ANY polygon, instead of the fragile
    // Delaunay-over-points approach that drops triangles / spawns slivers.
    for (auto FIt = Skeleton->faces_begin(); FIt != Skeleton->faces_end(); ++FIt)
    {
        CGALSkeletonFace Face;
        auto H0 = FIt->halfedge();
        if (H0 == Skeleton_2::Halfedge_handle())
            continue;

        auto H = H0;
        int Guard = 0;
        do
        {
            auto It = VertexToIndex.find(&*(H->vertex()));
            if (It != VertexToIndex.end())
                Face.VertexIndices.push_back(It->second);
            H = H->next();
            if (++Guard > 100000) break; // safety against malformed cycles
        } while (H != H0 && H != Skeleton_2::Halfedge_handle());

        if (Face.VertexIndices.size() >= 3)
            Result.Faces.push_back(Face);
    }

    return Result;
}

// No-holes convenience wrapper.
CGALSkeletonResult CGAL_GenerateSkeleton(const std::vector<double>& PolygonPoints)
{
    return CGAL_GenerateSkeletonWithHoles(PolygonPoints, std::vector<std::vector<double>>());
}

// ========================================================================
// GenerateSkeletonEdges - Legacy: edge position pairs
// ========================================================================
std::vector<CGALSkeletonEdgePair> CGAL_GenerateSkeletonEdges(const std::vector<double>& PolygonPoints)
{
    std::vector<CGALSkeletonEdgePair> Result;

    CGALSkeletonResult Skel = CGAL_GenerateSkeleton(PolygonPoints);

    for (const CGALSkeletonEdge& E : Skel.Edges)
    {
        if (!E.IsBisector) continue;
        if (E.VertexA < 0 || E.VertexA >= (int)Skel.Vertices.size()) continue;
        if (E.VertexB < 0 || E.VertexB >= (int)Skel.Vertices.size()) continue;

        const CGALSkeletonVertex& VA = Skel.Vertices[E.VertexA];
        const CGALSkeletonVertex& VB = Skel.Vertices[E.VertexB];

        CGALSkeletonEdgePair Pair;
        Pair.AX = VA.X; Pair.AY = VA.Y;
        Pair.BX = VB.X; Pair.BY = VB.Y;
        Result.push_back(Pair);
    }

    return Result;
}

// ========================================================================
// Triangulate (with optional holes) - Constrained Delaunay Triangulation.
// Inserts the outer ring + every hole ring as constraints, then classifies
// faces by NESTING DEPTH (odd = inside, even/hole = outside). This excludes
// inner courtyards from the triangulated surface.
//
// Index convention (unchanged): indices [0, outerVertCount) map to the OUTER
// ring vertices; everything else (Steiner points AND hole vertices) is returned
// in SteinerPoints, so callers can resolve them as (outer ++ SteinerPoints).
// ========================================================================
static void InsertRingConstraints(CDT& cdt, const std::vector<double>& Pts)
{
    std::vector<Point_2> Clean;
    for (size_t i = 0; i + 1 < Pts.size(); i += 2)
    {
        Point_2 P(Pts[i], Pts[i + 1]);
        bool bDup = false;
        for (const Point_2& E : Clean)
            if (CGAL::squared_distance(P, E) < 0.01) { bDup = true; break; }
        if (!bDup) Clean.push_back(P);
    }
    if (Clean.size() < 3) return;
    for (size_t i = 0; i < Clean.size(); i++)
        cdt.insert_constraint(Clean[i], Clean[(i + 1) % Clean.size()]);
}

CGALTriangulationResult CGAL_TriangulateWithHoles(
    const std::vector<double>& OuterPoints,
    const std::vector<std::vector<double>>& Holes)
{
    CGALTriangulationResult Result;
    Result.OriginalVertexCount = 0;

    if (OuterPoints.size() < 6) // need at least 3 points
        return Result;

    int NumInputVerts = static_cast<int>(OuterPoints.size() / 2);
    Result.OriginalVertexCount = NumInputVerts;

    CDT cdt;
    try
    {
        InsertRingConstraints(cdt, OuterPoints);
        for (const std::vector<double>& H : Holes)
            InsertRingConstraints(cdt, H);
    }
    catch (...)
    {
        return Result;
    }

    if (cdt.number_of_vertices() < 3)
        return Result;

    // ---- Mark domains by nesting depth (BFS from the infinite face) ----
    for (FaceHandle fh : cdt.all_face_handles())
        fh->info().Nesting = -1;

    std::queue<FaceHandle> Q;
    cdt.infinite_face()->info().Nesting = 0;
    Q.push(cdt.infinite_face());
    while (!Q.empty())
    {
        FaceHandle fh = Q.front(); Q.pop();
        for (int i = 0; i < 3; i++)
        {
            FaceHandle ni = fh->neighbor(i);
            if (ni->info().Nesting == -1)
            {
                CDT::Edge edge(fh, i);
                ni->info().Nesting = fh->info().Nesting + (cdt.is_constrained(edge) ? 1 : 0);
                Q.push(ni);
            }
        }
    }

    // ---- Index map: outer ring originals first ----
    std::map<Point_2, int> PointToIndex;
    int NextIdx = 0;
    for (int i = 0; i < NumInputVerts; i++)
    {
        Point_2 P(OuterPoints[i * 2], OuterPoints[i * 2 + 1]);
        if (PointToIndex.find(P) == PointToIndex.end())
            PointToIndex[P] = NextIdx++;
    }

    // ---- Collect inside (odd-nesting) faces ----
    for (FaceHandle fh : cdt.finite_face_handles())
    {
        if (!fh->info().IsInside())
            continue;

        int TriIndices[3];
        for (int i = 0; i < 3; i++)
        {
            Point_2 P = fh->vertex(i)->point();
            auto It = PointToIndex.find(P);
            if (It != PointToIndex.end())
            {
                TriIndices[i] = It->second;
            }
            else
            {
                TriIndices[i] = NextIdx;
                PointToIndex[P] = NextIdx;
                Result.SteinerPoints.push_back(CGAL::to_double(P.x()));
                Result.SteinerPoints.push_back(CGAL::to_double(P.y()));
                NextIdx++;
            }
        }

        Result.Triangles.push_back(TriIndices[0]);
        Result.Triangles.push_back(TriIndices[1]);
        Result.Triangles.push_back(TriIndices[2]);
    }

    return Result;
}

// No-holes convenience wrapper.
CGALTriangulationResult CGAL_Triangulate(const std::vector<double>& PolygonPoints)
{
    return CGAL_TriangulateWithHoles(PolygonPoints, std::vector<std::vector<double>>());
}
