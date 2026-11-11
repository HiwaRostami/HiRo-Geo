// Copyright Hiro. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Serializes elevation completion after the synchronous Generate() pass.
 *
 * A full disk-cache hit can complete inline while Generate() is still running,
 * whereas HTTP completion arrives later. Both paths submit through this gate:
 * inline work is retained until Open(), and later work executes immediately.
 * The actor's generation-id and weak-pointer checks remain in the submitted
 * callback, so this class changes ordering without weakening lifetime safety.
 */
class FHiroGeoGenerationGate
{
public:
	void Submit(TFunction<void()>&& Work)
	{
		check(IsInGameThread());
		if (bOpen)
		{
			Work();
			return;
		}

		checkf(!DeferredWork, TEXT("HiroGeo generation gate received more than one completion"));
		DeferredWork = MoveTemp(Work);
	}

	void Open()
	{
		check(IsInGameThread());
		bOpen = true;
		if (DeferredWork)
		{
			TFunction<void()> Work = MoveTemp(DeferredWork);
			Work();
		}
	}

	bool IsOpen() const { return bOpen; }

private:
	bool bOpen = false;
	TFunction<void()> DeferredWork;
};
