// Copyright Hiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Thin wrapper around Esri's official LERC (Limited Error Raster Compression)
 * C API, vendored under Private/ThirdParty/Lerc. This is the same codec
 * elevation3d.arcgis.com uses to serve World Elevation 3D tiles (and what
 * streets.gl decodes client-side via its "lerc" npm package).
 *
 * Only single-band float decode is exposed, since that's all HiroGeo's
 * elevation pipeline needs.
 */
class HIROGEOLERC_API FHiroGeoLercDecoder
{
public:
	/**
	 * Decodes a single-band LERC blob (e.g. the raw bytes of an HTTP response
	 * body from an Esri ImageServer /tile/ endpoint) into a row-major float
	 * array, top-left to bottom-right.
	 *
	 * @param LercData      Pointer to the raw LERC blob bytes.
	 * @param LercDataSize  Size of LercData in bytes.
	 * @param OutHeights    Receives Width*Height float values on success.
	 * @param OutWidth      Receives the decoded image width in pixels.
	 * @param OutHeight     Receives the decoded image height in pixels.
	 * @return true on success, false if the blob is invalid/corrupt or not
	 *         single-band float data.
	 */
	static bool DecodeSingleBandFloat(
		const uint8* LercData,
		int32 LercDataSize,
		TArray<float>& OutHeights,
		int32& OutWidth,
		int32& OutHeight,
		TArray<uint8>* OutValidMask = nullptr);
};
