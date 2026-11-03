// A "Road Lines" option in the customizer. Each descriptor changes either the side lines or the
// lane layout of the road it is applied to; the other setting is kept.
// Derives from the pattern type because the customizer menu draws patterns with their icon (other
// types are drawn as colour swatches). It is never applied as a pattern: the paint gun's fire is
// intercepted while a Road Lines option is selected (see RMRoadLinePaint).
#pragma once

#include "CoreMinimal.h"
#include "FGFactoryColoringTypes.h"
#include "RMGeometry.h"
#include "RMRoadLineDescriptor.generated.h"

UCLASS(Blueprintable, BlueprintType)
class ROADMAKER_API URMRoadLineDescriptor : public UFGFactoryCustomizationDescriptor_Pattern
{
	GENERATED_BODY()

public:
	/** True: this option sets side lines. False: it sets the lane layout. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	bool mIsSideLineOption = false;

	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	bool mSideLines = true;

	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	int32 mLanes = 2;

	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	bool mContinuous = false;

	/** Writes this option into existing markings, leaving the other setting as it was. */
	static void ApplyTo(TSubclassOf<URMRoadLineDescriptor> Option, FRMMarkings& InOut)
	{
		const URMRoadLineDescriptor* D = Option ? Option->GetDefaultObject<URMRoadLineDescriptor>() : nullptr;
		if (!D)
		{
			return;
		}
		if (D->mIsSideLineOption)
		{
			InOut.bEdgeLines = D->mSideLines;
		}
		else
		{
			InOut.Lanes = static_cast<uint8>(FMath::Clamp(D->mLanes, 1, 3));
			InOut.bContinuous = D->mContinuous;
		}
	}
};
