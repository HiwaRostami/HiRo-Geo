// Copyright Hiro. All Rights Reserved.

#include "HiroGeoElevationFetcher.h"
#include "HiroGeoLercDecoder.h"

#include "HttpModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogHiroGeoElevation, Log, All);

// ============================================================================
// Web Mercator slippy-tile helpers (EPSG:3857 / OSM-style XYZ tiling — the
// same scheme elevation3d.arcgis.com uses, and what streets.gl relies on).
// All of these work in "global pixel space": pixel (0,0) is the NW corner of
// the whole map at the given zoom, tiles are 256x256, so there are
// 256 * 2^Zoom pixels across the full width of the world.
// ============================================================================
namespace HiroGeoWebMercator
{
	static double LonToGlobalPixelX(double LonDeg, int32 Zoom)
	{
		const double WorldPx = 256.0 * static_cast<double>(1LL << Zoom);
		return (LonDeg + 180.0) / 360.0 * WorldPx;
	}

	static double LatToGlobalPixelY(double LatDeg, int32 Zoom)
	{
		const double WorldPx = 256.0 * static_cast<double>(1LL << Zoom);
		const double ClampedLat = FMath::Clamp(LatDeg, -85.05112878, 85.05112878);
		const double LatRad = FMath::DegreesToRadians(ClampedLat);
		const double Y = FMath::Loge(FMath::Tan(LatRad) + 1.0 / FMath::Cos(LatRad));
		return (1.0 - Y / UE_PI) / 2.0 * WorldPx;
	}

	static int64 LonToTileX(double LonDeg, int32 Zoom)
	{
		return static_cast<int64>(FMath::FloorToDouble(LonToGlobalPixelX(LonDeg, Zoom) / 256.0));
	}

	static int64 LatToTileY(double LatDeg, int32 Zoom)
	{
		return static_cast<int64>(FMath::FloorToDouble(LatToGlobalPixelY(LatDeg, Zoom) / 256.0));
	}
}

// ============================================================================
// FHiroGeoHeightmap
// ============================================================================
float FHiroGeoHeightmap::SampleAtLatLon(double Lat, double Lon) const
{
	if (!IsValid())
	{
		return 0.0f;
	}

	const double GlobalPxX = HiroGeoWebMercator::LonToGlobalPixelX(Lon, Zoom);
	const double GlobalPxY = HiroGeoWebMercator::LatToGlobalPixelY(Lat, Zoom);

	const double LocalX = FMath::Clamp(GlobalPxX - OriginPixelX, 0.0, static_cast<double>(Width - 1));
	const double LocalY = FMath::Clamp(GlobalPxY - OriginPixelY, 0.0, static_cast<double>(Height - 1));

	const int32 X0 = FMath::Clamp(static_cast<int32>(LocalX), 0, Width - 1);
	const int32 Y0 = FMath::Clamp(static_cast<int32>(LocalY), 0, Height - 1);
	const int32 X1 = FMath::Min(X0 + 1, Width - 1);
	const int32 Y1 = FMath::Min(Y0 + 1, Height - 1);
	const float FracX = static_cast<float>(LocalX - X0);
	const float FracY = static_cast<float>(LocalY - Y0);

	const float H00 = Heights[Y0 * Width + X0];
	const float H10 = Heights[Y0 * Width + X1];
	const float H01 = Heights[Y1 * Width + X0];
	const float H11 = Heights[Y1 * Width + X1];

	const float Hx0 = FMath::Lerp(H00, H10, FracX);
	const float Hx1 = FMath::Lerp(H01, H11, FracX);
	return FMath::Lerp(Hx0, Hx1, FracY);
}

// ============================================================================
// FHiroGeoElevationFetcher
// ============================================================================
TSharedPtr<FHiroGeoElevationFetcher> FHiroGeoElevationFetcher::FetchForBounds(
	double MinLat, double MinLon, double MaxLat, double MaxLon,
	int32 ZoomLevel, FOnComplete OnComplete, bool bUseDiskCache)
{
	TSharedPtr<FHiroGeoElevationFetcher> Fetcher = MakeShared<FHiroGeoElevationFetcher>();
	Fetcher->CompletionDelegate = OnComplete;
	Fetcher->bDiskCache = bUseDiskCache;
	Fetcher->Start(MinLat, MinLon, MaxLat, MaxLon, ZoomLevel);
	return Fetcher;
}

FString FHiroGeoElevationFetcher::GetCacheDirectory()
{
	return FPaths::ProjectSavedDir() / TEXT("HiroGeoElevationCache");
}

FString FHiroGeoElevationFetcher::MakeCacheFilePath(int32 InZoom, int64 TileX, int64 TileY)
{
	// v2: Esri Terrain3D tiles are 257x257 (1px leading border). Older caches
	// written under the broken ≥258 acceptance gate may be fine payloads, but
	// keep a versioned filename so we never re-use a half-decoded/corrupt blob.
	return GetCacheDirectory() / FString::Printf(TEXT("lerc_v2_%d_%lld_%lld.bin"), InZoom, TileY, TileX);
}

bool FHiroGeoElevationFetcher::TryLoadCachedTile(int32 InZoom, int64 TileX, int64 TileY, TArray<uint8>& OutBytes)
{
	const FString Path = MakeCacheFilePath(InZoom, TileX, TileY);
	if (!FPaths::FileExists(Path)) return false;
	if (!FFileHelper::LoadFileToArray(OutBytes, *Path) || OutBytes.Num() < 32)
	{
		OutBytes.Reset();
		return false;
	}
	// Reject obvious non-LERC / HTML error pages that may have been cached.
	const bool bLerc1 = OutBytes.Num() >= 10
		&& OutBytes[0] == 'C' && OutBytes[1] == 'n' && OutBytes[2] == 't' && OutBytes[3] == 'Z';
	const bool bLerc2 = OutBytes.Num() >= 5
		&& OutBytes[0] == 'L' && OutBytes[1] == 'e' && OutBytes[2] == 'r' && OutBytes[3] == 'c';
	if (!bLerc1 && !bLerc2)
	{
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation: corrupt cache for z=%d y=%lld x=%lld (%d bytes, bad magic) — deleting"),
			InZoom, TileY, TileX, OutBytes.Num());
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		PF.DeleteFile(*Path);
		OutBytes.Reset();
		return false;
	}
	return true;
}

bool FHiroGeoElevationFetcher::CommitCacheTempFile(const FString& TempPath, const FString& FinalPath)
{
#if PLATFORM_WINDOWS
	// MoveFileEx with REPLACE_EXISTING performs one namespace operation: if the
	// replace fails, Windows leaves the existing destination in place. WRITE_THROUGH
	// asks Windows not to return until the move has reached the filesystem.
	return ::MoveFileExW(
		*TempPath,
		*FinalPath,
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	// UE's replace-capable move is the closest portable API. Unlike the old code,
	// HiroGeo never explicitly removes the destination before attempting it.
	return IFileManager::Get().Move(
		*FinalPath, *TempPath, /*Replace=*/true, /*EvenIfReadOnly=*/false,
		/*Attributes=*/false, /*bDoNotRetryOrError=*/true);
#endif
}

bool FHiroGeoElevationFetcher::SaveCachedTile(int32 InZoom, int64 TileX, int64 TileY, const TArray<uint8>& Bytes)
{
	if (Bytes.Num() < 32) return false;
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.CreateDirectoryTree(*GetCacheDirectory()))
	{
		return false;
	}
	const FString FinalPath = MakeCacheFilePath(InZoom, TileX, TileY);
	const FString TempPath = FinalPath + TEXT(".") +
		FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");

	if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
	{
		PF.DeleteFile(*TempPath);
		return false;
	}

	const bool bCommitted = CommitCacheTempFile(TempPath, FinalPath);
	if (!bCommitted)
	{
#if PLATFORM_WINDOWS
		const uint32 CommitError = ::GetLastError();
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation: failed to commit cache temp '%s' (Win32 error %u); preserving any existing '%s'"),
			*TempPath, CommitError, *FinalPath);
#else
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation: failed to commit cache temp '%s'; preserving any existing '%s'"),
			*TempPath, *FinalPath);
#endif
		PF.DeleteFile(*TempPath);
	}
	return bCommitted;
}

void FHiroGeoElevationFetcher::Start(double MinLat, double MinLon, double MaxLat, double MaxLon, int32 ZoomLevel)
{
	if (!FMath::IsFinite(MinLat) || !FMath::IsFinite(MinLon) ||
		!FMath::IsFinite(MaxLat) || !FMath::IsFinite(MaxLon) ||
		MinLat < -90.0 || MaxLat > 90.0 || MinLon < -180.0 || MaxLon > 180.0 ||
		ZoomLevel < 0 || ZoomLevel > 30)
	{
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation fetch aborted: invalid bounds or zoom (lat %.6f..%.6f, lon %.6f..%.6f, zoom %d)"),
			MinLat, MaxLat, MinLon, MaxLon, ZoomLevel);
		Finish(false);
		return;
	}

	Zoom = ZoomLevel;

	TileXMin = HiroGeoWebMercator::LonToTileX(MinLon, Zoom);
	TileXMax = HiroGeoWebMercator::LonToTileX(MaxLon, Zoom);
	// Latitude and tile-Y both run "north = small Y", so MaxLat maps to the
	// smaller tile Y and MinLat to the larger one.
	TileYMin = HiroGeoWebMercator::LatToTileY(MaxLat, Zoom);
	TileYMax = HiroGeoWebMercator::LatToTileY(MinLat, Zoom);

	if (TileXMin > TileXMax) { Swap(TileXMin, TileXMax); }
	if (TileYMin > TileYMax) { Swap(TileYMin, TileYMax); }

	NumTilesX = static_cast<int32>(TileXMax - TileXMin + 1);
	NumTilesY = static_cast<int32>(TileYMax - TileYMin + 1);

	if (static_cast<int64>(NumTilesX) * static_cast<int64>(NumTilesY) > MaxTilesPerFetch)
	{
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation fetch aborted: %d x %d = %d tiles requested at zoom %d exceeds the safety cap of %d tiles. ")
			TEXT("Lower ElevationZoomLevel for large OSM extracts."),
			NumTilesX, NumTilesY, NumTilesX * NumTilesY, Zoom, MaxTilesPerFetch);
		Finish(false);
		return;
	}

	Heightmap = MakeShared<FHiroGeoHeightmap>();
	Heightmap->Width = NumTilesX * TileUsablePx;
	Heightmap->Height = NumTilesY * TileUsablePx;
	Heightmap->Zoom = Zoom;
	Heightmap->OriginPixelX = static_cast<double>(TileXMin) * 256.0;
	Heightmap->OriginPixelY = static_cast<double>(TileYMin) * 256.0;
	Heightmap->Heights.SetNumZeroed(Heightmap->Width * Heightmap->Height);

	UE_LOG(LogHiroGeoElevation, Log,
		TEXT("HiroGeo elevation: fetching %d x %d tile(s) at zoom %d for bounds [%.6f,%.6f]-[%.6f,%.6f] (diskCache=%d)"),
		NumTilesX, NumTilesY, Zoom, MinLat, MinLon, MaxLat, MaxLon, bDiskCache ? 1 : 0);

	PendingTileCount = NumTilesX * NumTilesY;
	for (int64 TY = TileYMin; TY <= TileYMax; TY++)
	{
		for (int64 TX = TileXMin; TX <= TileXMax; TX++)
		{
			RequestTile(TX, TY);
		}
	}
}

bool FHiroGeoElevationFetcher::ApplyDecodedTile(
	int64 TileX, int64 TileY, const TArray<float>& TileHeights,
	const TArray<uint8>& ValidMask, int32 TileW, int32 TileH)
{
	// Esri Terrain3D / streets-gl: tiles are typically 257x257 with a 1px
	// leading border (source uses pixels [1..256]). Some endpoints deliver
	// 258x258 with a 1px border on every side — both are accepted as long as
	// we have at least TileUsablePx+1 samples so the +1 offset fits.
	if (TileW < TileUsablePx + TileBorderPx || TileH < TileUsablePx + TileBorderPx)
	{
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation: tile (x=%lld,y=%lld) decoded to %dx%d — need at least %dx%d (256+1 border)"),
			TileX, TileY, TileW, TileH, TileUsablePx + TileBorderPx, TileUsablePx + TileBorderPx);
		return false;
	}
	if (TileHeights.Num() < TileW * TileH)
	{
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation: tile (x=%lld,y=%lld) height buffer too small (%d vs %dx%d)"),
			TileX, TileY, TileHeights.Num(), TileW, TileH);
		return false;
	}
	if (ValidMask.Num() != 0 && ValidMask.Num() < TileW * TileH)
	{
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation: tile (x=%lld,y=%lld) validity mask too small (%d vs %dx%d)"),
			TileX, TileY, ValidMask.Num(), TileW, TileH);
		return false;
	}

	const int32 DestOffsetX = static_cast<int32>(TileX - TileXMin) * TileUsablePx;
	const int32 DestOffsetY = static_cast<int32>(TileY - TileYMin) * TileUsablePx;

	// streets-gl EsriElevationFetcher.ts: sourceX/Y = local + 1
	for (int32 Y = 0; Y < TileUsablePx; Y++)
	{
		for (int32 X = 0; X < TileUsablePx; X++)
		{
			const int32 SrcIdx = (Y + TileBorderPx) * TileW + (X + TileBorderPx);
			if ((ValidMask.Num() > 0 && ValidMask[SrcIdx] == 0) || !FMath::IsFinite(TileHeights[SrcIdx]))
			{
				UE_LOG(LogHiroGeoElevation, Warning,
					TEXT("HiroGeo elevation: tile (x=%lld,y=%lld) has no-data/non-finite sample in usable area at %d,%d"),
					TileX, TileY, X, Y);
				return false;
			}
			const int32 DstIdx = (DestOffsetY + Y) * Heightmap->Width + (DestOffsetX + X);
			Heightmap->Heights[DstIdx] = TileHeights[SrcIdx];
		}
	}
	return true;
}

void FHiroGeoElevationFetcher::RequestTile(int64 TileX, int64 TileY, int32 Attempt)
{
	// Disk cache hit — decode locally, skip HTTP (streets-gl style reuse).
	if (bDiskCache && Attempt == 0)
	{
		TArray<uint8> Cached;
		if (TryLoadCachedTile(Zoom, TileX, TileY, Cached))
		{
			TArray<float> TileHeights;
			TArray<uint8> ValidMask;
			int32 TileW = 0, TileH = 0;
			if (FHiroGeoLercDecoder::DecodeSingleBandFloat(Cached.GetData(), Cached.Num(), TileHeights, TileW, TileH, &ValidMask)
				&& ApplyDecodedTile(TileX, TileY, TileHeights, ValidMask, TileW, TileH))
			{
				++SuccessfulTiles;
				if (--PendingTileCount <= 0)
				{
					Finish(SuccessfulTiles > 0 && !bAnyTileFailed);
				}
				return;
			}

			// Corrupt / incompatible cache entry — delete and fall through to HTTP.
			UE_LOG(LogHiroGeoElevation, Warning,
				TEXT("HiroGeo elevation: cache decode failed for z=%d y=%lld x=%lld (decoded %dx%d) — refetching"),
				Zoom, TileY, TileX, TileW, TileH);
			IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
			PF.DeleteFile(*MakeCacheFilePath(Zoom, TileX, TileY));
		}
	}

	// Same endpoint streets.gl's EsriElevationFetcher.ts uses: /tile/{zoom}/{row}/{col}.
	const FString Url = FString::Printf(
		TEXT("https://elevation3d.arcgis.com/arcgis/rest/services/WorldElevation3D/Terrain3D/ImageServer/tile/%d/%lld/%lld"),
		Zoom, TileY, TileX);

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/octet-stream,*/*"));
	Request->SetHeader(TEXT("User-Agent"), TEXT("HiroGeo/1.0 (UnrealEngine; Esri Terrain3D LERC)"));
	Request->SetTimeout(20.0f);

	// Keep this fetcher alive until the response comes back, even if the
	// caller already dropped its own reference.
	TSharedPtr<FHiroGeoElevationFetcher> SharedThis = AsShared();
	Request->OnProcessRequestComplete().BindLambda(
		[SharedThis, TileX, TileY, Attempt](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedSuccessfully)
		{
			SharedThis->HandleTileResponse(Req, Resp, bConnectedSuccessfully, TileX, TileY, Attempt);
		});
	ActiveRequests.Add(Request);
	if (!Request->ProcessRequest())
	{
		Request->OnProcessRequestComplete().Unbind();
		HandleTileResponse(Request, nullptr, false, TileX, TileY, Attempt);
	}
}

void FHiroGeoElevationFetcher::HandleTileResponse(
	FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnectedSuccessfully,
	int64 TileX, int64 TileY, int32 Attempt)
{
	ActiveRequests.Remove(Request);
	if (bFinished)
	{
		return; // Already aborted (e.g. hit the tile-count safety cap).
	}

	const int32 HttpCode = (Response.IsValid()) ? Response->GetResponseCode() : 0;
	const int32 BodyBytes = (Response.IsValid()) ? Response->GetContent().Num() : 0;
	bool bOk = bConnectedSuccessfully && Response.IsValid() && HttpCode == 200 && BodyBytes > 0;

	FString FailReason;
	if (!bConnectedSuccessfully)
	{
		FailReason = TEXT("connection failed");
	}
	else if (!Response.IsValid())
	{
		FailReason = TEXT("null HTTP response");
	}
	else if (HttpCode != 200)
	{
		FailReason = FString::Printf(TEXT("HTTP %d (%d bytes)"), HttpCode, BodyBytes);
	}
	else if (BodyBytes == 0)
	{
		FailReason = TEXT("empty body");
	}

	if (bOk)
	{
		const TArray<uint8>& Body = Response->GetContent();

		TArray<float> TileHeights;
		TArray<uint8> ValidMask;
		int32 TileW = 0, TileH = 0;
		if (!FHiroGeoLercDecoder::DecodeSingleBandFloat(Body.GetData(), Body.Num(), TileHeights, TileW, TileH, &ValidMask))
		{
			bOk = false;
			FailReason = FString::Printf(TEXT("LERC decode failed (%d bytes, magic=%c%c%c%c)"),
				Body.Num(),
				Body.Num() > 0 ? (TCHAR)Body[0] : '?',
				Body.Num() > 1 ? (TCHAR)Body[1] : '?',
				Body.Num() > 2 ? (TCHAR)Body[2] : '?',
				Body.Num() > 3 ? (TCHAR)Body[3] : '?');
		}
		else if (!ApplyDecodedTile(TileX, TileY, TileHeights, ValidMask, TileW, TileH))
		{
			bOk = false;
			FailReason = FString::Printf(TEXT("unexpected tile size %dx%d (need >= %dx%d)"),
				TileW, TileH, TileUsablePx + TileBorderPx, TileUsablePx + TileBorderPx);
		}
		else
		{
			++SuccessfulTiles;
			if (bDiskCache)
			{
				SaveCachedTile(Zoom, TileX, TileY, Body);
			}
		}
	}

	if (!bOk)
	{
		if (Attempt < MaxRetriesPerTile)
		{
			UE_LOG(LogHiroGeoElevation, Warning,
				TEXT("HiroGeo elevation: tile (x=%lld,y=%lld,zoom=%d) attempt %d failed (%s); retrying"),
				TileX, TileY, Zoom, Attempt + 1, *FailReason);
			RequestTile(TileX, TileY, Attempt + 1);
			return;
		}
		bAnyTileFailed = true;
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation: failed tile (x=%lld, y=%lld, zoom=%d) — %s"),
			TileX, TileY, Zoom, *FailReason);
	}

	if (--PendingTileCount <= 0)
	{
		// Never treat missing tiles as sea-level measurements.
		Finish(SuccessfulTiles > 0 && !bAnyTileFailed);
	}
}

void FHiroGeoElevationFetcher::Cancel()
{
	if (bFinished) return;
	bFinished = true;
	CompletionDelegate.Unbind();
	for (const FHttpRequestPtr& Request : ActiveRequests)
	{
		if (Request.IsValid())
		{
			Request->OnProcessRequestComplete().Unbind();
			Request->CancelRequest();
		}
	}
	ActiveRequests.Reset();
}

void FHiroGeoElevationFetcher::Finish(bool bSuccess)
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;

	if (bSuccess && Heightmap.IsValid() && Heightmap->Heights.Num() > 0)
	{
		float MinH = Heightmap->Heights[0];
		float MaxH = Heightmap->Heights[0];
		double Sum = 0.0;
		for (float H : Heightmap->Heights)
		{
			MinH = FMath::Min(MinH, H);
			MaxH = FMath::Max(MaxH, H);
			Sum += H;
		}
		UE_LOG(LogHiroGeoElevation, Log,
			TEXT("HiroGeo elevation: ok — %d/%d tiles, heightmap %dx%d, elev [%.1f .. %.1f] m (avg %.1f)"),
			SuccessfulTiles, NumTilesX * NumTilesY,
			Heightmap->Width, Heightmap->Height,
			MinH, MaxH, Sum / Heightmap->Heights.Num());
	}

	if (bSuccess && bAnyTileFailed)
	{
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation: completed with some tiles missing (%d ok) — terrain will be flat (0m) in those areas."),
			SuccessfulTiles);
	}
	else if (!bSuccess)
	{
		UE_LOG(LogHiroGeoElevation, Warning,
			TEXT("HiroGeo elevation: incomplete coverage (%d/%d tiles valid); rejecting DEM so missing data cannot become 0m terrain."),
			SuccessfulTiles, NumTilesX * NumTilesY);
	}

	ActiveRequests.Reset();
	CompletionDelegate.ExecuteIfBound(bSuccess && Heightmap.IsValid(), bSuccess ? Heightmap : nullptr);
}
