// Copyright Hiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

// ============================================================================
// FHiroGeoHeightmap
//
// A dense, Web-Mercator-aligned grid of real-world elevation samples (in
// meters), assembled from one or more decoded Esri World Elevation 3D LERC
// tiles — the same endpoint/dataset streets.gl's EsriElevationFetcher.ts
// pulls from. Supports bilinear lookup by raw WGS84 lat/lon.
// ============================================================================
struct HIROGEO_API FHiroGeoHeightmap
{
	int32 Width = 0;
	int32 Height = 0;
	int32 Zoom = 0;

	// World pixel coordinates (Web Mercator, 256px-per-tile convention) of
	// this heightmap's top-left corner — i.e. pixel [0,0] — at Zoom level.
	double OriginPixelX = 0.0;
	double OriginPixelY = 0.0;

	// Row-major elevation samples in meters, size Width*Height.
	TArray<float> Heights;

	bool IsValid() const { return Width > 0 && Height > 0 && Heights.Num() == Width * Height; }

	/** Bilinear-sample elevation (meters) at a given WGS84 lat/lon. Coordinates
	 *  outside the heightmap are clamped to the nearest edge pixel. */
	float SampleAtLatLon(double Lat, double Lon) const;
};

// ============================================================================
// FHiroGeoElevationFetcher
//
// Downloads the Esri World Elevation 3D LERC tiles covering a given lat/lon
// bounding box, decodes them via the HiroGeoLERC bridge module, and stitches
// them into a single FHiroGeoHeightmap.
//
// UE equivalent of streets-gl's tile cache: decoded LERC tiles are stored under
// Saved/HiroGeoElevationCache/{zoom}_{y}_{x}.bin so re-imports of the same
// cell reuse disk instead of re-hitting elevation3d.arcgis.com.
// ============================================================================
class HIROGEO_API FHiroGeoElevationFetcher : public TSharedFromThis<FHiroGeoElevationFetcher>
{
public:
	DECLARE_DELEGATE_TwoParams(FOnComplete, bool /*bSuccess*/, TSharedPtr<FHiroGeoHeightmap> /*Heightmap*/);

	FHiroGeoElevationFetcher() = default;

	/**
	 * Kicks off the async fetch for the given bounding box and returns the
	 * fetcher object. Keep the returned shared pointer alive (e.g. as a
	 * member on the calling actor) until OnComplete fires, or the in-flight
	 * HTTP requests' completion lambdas will simply hold the last reference
	 * themselves (safe either way, since they capture a TSharedPtr to this).
	 *
	 * @param ZoomLevel  Web-Mercator zoom level for the source tiles. Higher
	 *                   = finer detail but more tiles/requests. ~14-16 is a
	 *                   good range for city-block-scale terrain (roughly
	 *                   2.4m-9.5m per pixel at the equator).
	 * @param bUseDiskCache  When true (default), load/save per-tile LERC
	 *                       payloads under the project Saved folder.
	 */
	static TSharedPtr<FHiroGeoElevationFetcher> FetchForBounds(
		double MinLat, double MinLon, double MaxLat, double MaxLon,
		int32 ZoomLevel, FOnComplete OnComplete, bool bUseDiskCache = true);

	/** Absolute path to the on-disk elevation tile cache directory. */
	static FString GetCacheDirectory();

	/** Cancels in-flight requests without invoking the completion delegate. */
	void Cancel();
	bool IsFinished() const { return bFinished; }

private:
	void Start(double MinLat, double MinLon, double MaxLat, double MaxLon, int32 ZoomLevel);
	void RequestTile(int64 TileX, int64 TileY, int32 Attempt = 0);
	void HandleTileResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully, int64 TileX, int64 TileY, int32 Attempt);
	/** Copies the usable 256x256 interior (streets-gl +1 border offset). Returns false if dims are too small. */
	bool ApplyDecodedTile(int64 TileX, int64 TileY, const TArray<float>& TileHeights, const TArray<uint8>& ValidMask, int32 TileW, int32 TileH);
	void Finish(bool bSuccess);

	static FString MakeCacheFilePath(int32 InZoom, int64 TileX, int64 TileY);
	static bool TryLoadCachedTile(int32 InZoom, int64 TileX, int64 TileY, TArray<uint8>& OutBytes);
	static bool SaveCachedTile(int32 InZoom, int64 TileX, int64 TileY, const TArray<uint8>& Bytes);
	static bool CommitCacheTempFile(const FString& TempPath, const FString& FinalPath);

#if WITH_DEV_AUTOMATION_TESTS
	friend class FHiroGeoElevationNoDataTest;
	friend class FHiroGeoElevationFailureCancelTest;
	friend class FHiroGeoElevationCacheSafetyTest;
#endif

	TSharedPtr<FHiroGeoHeightmap> Heightmap;
	FOnComplete CompletionDelegate;

	int32 Zoom = 0;
	int64 TileXMin = 0, TileXMax = 0, TileYMin = 0, TileYMax = 0;
	int32 NumTilesX = 0, NumTilesY = 0;

	int32 PendingTileCount = 0;
	int32 SuccessfulTiles = 0;
	bool bAnyTileFailed = false;
	bool bFinished = false;
	bool bDiskCache = true;
	TArray<FHttpRequestPtr> ActiveRequests;

	// Esri Terrain3D tiles are typically 257x257 with a 1px leading border
	// (streets.gl copies pixels at sourceX/Y = local+1 into a 256x256 usable
	// region). 258x258 (border on all sides) is also accepted — we still use
	// the +1 offset and ignore any trailing border column/row.
	static constexpr int32 TileBorderPx = 1;
	static constexpr int32 TileUsablePx = 256;

	// Safety cap: refuses to fire off an unreasonable number of HTTP requests
	// for a huge OSM extract at a high zoom level. Lower ElevationZoomLevel
	// for large areas instead of raising this.
	static constexpr int32 MaxTilesPerFetch = 400;
	static constexpr int32 MaxRetriesPerTile = 2;
};
