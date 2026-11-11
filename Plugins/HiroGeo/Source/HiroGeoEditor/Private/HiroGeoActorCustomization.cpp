// Copyright Hiro. All Rights Reserved.

#include "HiroGeoActorCustomization.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"

TSharedRef<IDetailCustomization> FHiroGeoActorCustomization::MakeInstance()
{
	return MakeShareable(new FHiroGeoActorCustomization);
}

void FHiroGeoActorCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Future customization: reorder categories, hide properties, etc.
}
