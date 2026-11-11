// Copyright Hiro. All Rights Reserved.

#include "HiroGeoLercDecoder.h"

#include "Lerc_c_api.h"
#include "Lerc_types.h"

DEFINE_LOG_CATEGORY_STATIC(LogHiroGeoLERC, Log, All);

bool FHiroGeoLercDecoder::DecodeSingleBandFloat(
	const uint8* LercData,
	int32 LercDataSize,
	TArray<float>& OutHeights,
	int32& OutWidth,
	int32& OutHeight,
	TArray<uint8>* OutValidMask)
{
	OutHeights.Reset();
	if (OutValidMask) OutValidMask->Reset();
	OutWidth = 0;
	OutHeight = 0;

	if (LercData == nullptr || LercDataSize <= 0)
	{
		UE_LOG(LogHiroGeoLERC, Warning, TEXT("HiroGeoLERC: empty blob passed to DecodeSingleBandFloat"));
		return false;
	}

	// --- Step 1: read the blob header so we know how big the output needs to be. ---
	const int32 InfoArrSize = (int32)LercNS::InfoArrOrder::_last;
	const int32 DataRangeArrSize = (int32)LercNS::DataRangeArrOrder::_last;

	TArray<uint32> InfoArr;
	InfoArr.SetNumZeroed(InfoArrSize);
	TArray<double> DataRangeArr;
	DataRangeArr.SetNumZeroed(DataRangeArrSize);

	const lerc_status InfoStatus = lerc_getBlobInfo(
		LercData, (uint32)LercDataSize,
		InfoArr.GetData(), DataRangeArr.GetData(),
		InfoArrSize, DataRangeArrSize);

	if (InfoStatus != 0)
	{
		UE_LOG(LogHiroGeoLERC, Warning, TEXT("HiroGeoLERC: lerc_getBlobInfo failed (code %u)"), InfoStatus);
		return false;
	}

	const int32 NCols = (int32)InfoArr[(int32)LercNS::InfoArrOrder::nCols];
	const int32 NRows = (int32)InfoArr[(int32)LercNS::InfoArrOrder::nRows];
	const int32 NBands = (int32)InfoArr[(int32)LercNS::InfoArrOrder::nBands];
	const int32 NDepth = (int32)InfoArr[(int32)LercNS::InfoArrOrder::nDim];
	const int32 NMasks = (int32)InfoArr[(int32)LercNS::InfoArrOrder::nMasks];

	if (NCols <= 0 || NRows <= 0 || NCols > 4096 || NRows > 4096 || NMasks < 0 || NMasks > 1)
	{
		UE_LOG(LogHiroGeoLERC, Warning, TEXT("HiroGeoLERC: blob reports invalid dimensions %dx%d"), NCols, NRows);
		return false;
	}

	if (NBands != 1 || NDepth != 1)
	{
		// Elevation tiles are always single-band, single-value-per-pixel. If this
		// ever fires, the endpoint/format has changed and the caller needs updating.
		UE_LOG(LogHiroGeoLERC, Warning, TEXT("HiroGeoLERC: expected single-band data, got nBands=%d nDepth=%d"), NBands, NDepth);
		return false;
	}

	// --- Step 2: decode. ---
	OutHeights.SetNumUninitialized(NCols * NRows);

	TArray<uint8> ValidBytes;
	if (NMasks > 0)
	{
		ValidBytes.SetNumZeroed(NCols * NRows * NMasks);
	}

	const lerc_status DecodeStatus = lerc_decode(
		LercData, (uint32)LercDataSize,
		NMasks, NMasks > 0 ? ValidBytes.GetData() : nullptr,
		/*nDepth=*/1, NCols, NRows, /*nBands=*/1,
		(uint32)LercNS::DataType::dt_float,
		(void*)OutHeights.GetData());

	if (DecodeStatus != 0)
	{
		UE_LOG(LogHiroGeoLERC, Warning, TEXT("HiroGeoLERC: lerc_decode failed (code %u)"), DecodeStatus);
		OutHeights.Reset();
		return false;
	}

	// Preserve validity separately. Callers can reject no-data in the region
	// they actually consume without discarding an otherwise usable tile merely
	// because an unused border pixel is masked out.
	if (OutValidMask)
	{
		OutValidMask->SetNumUninitialized(NCols * NRows);
		if (NMasks > 0)
			FMemory::Memcpy(OutValidMask->GetData(), ValidBytes.GetData(), NCols * NRows);
		else
			FMemory::Memset(OutValidMask->GetData(), 1, NCols * NRows);
	}

	OutWidth = NCols;
	OutHeight = NRows;
	UE_LOG(LogHiroGeoLERC, Verbose,
		TEXT("HiroGeoLERC: decoded %dx%d (bands=%d masks=%d range=[%.2f,%.2f])"),
		NCols, NRows, NBands, NMasks,
		DataRangeArr.Num() > 0 ? DataRangeArr[0] : 0.0,
		DataRangeArr.Num() > 1 ? DataRangeArr[1] : 0.0);
	return true;
}
