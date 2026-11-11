// A "Road Lines" option in the customizer. Each descriptor changes either the side lines or the
// lane layout of the road it is applied to; the other setting is kept.
// Derives from the material type because the customizer menu draws materials with their own icon
// (swatches are drawn from colour slots, patterns from the game's pattern atlas by ID). It is never
// applied as a material: the paint gun's fire is intercepted while a Road Lines option is selected
// (see RMRoadLinePaint), and its buildable map is empty, so it swaps nothing.
#pragma once

#include "CoreMinimal.h"
#include "FGFactoryColoringTypes.h"
#include "RMGeometry.h"
#include "Buildables/RMBuildableMarking.h"
#include "RMRoadLineDescriptor.generated.h"

UCLASS(Blueprintable, BlueprintType)
class ROADMAKER_API URMRoadLineDescriptor : public UFGFactoryCustomizationDescriptor_Material
{
	GENERATED_BODY()

public:
	/**
	 * Not a physical item: an invalid resource form keeps the options out of item lists (e.g. the
	 * creative mode "give items" menu), like the game's own customizer entries.
	 */
	URMRoadLineDescriptor() { mForm = EResourceForm::RF_INVALID; }

	/** True: this option sets side lines. False: it sets the lane layout. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	bool mIsSideLineOption = false;

	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	bool mSideLines = true;

	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	int32 mLanes = 2;

	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	bool mContinuous = false;

	/** True: this option paints a lane marking (an arrow or a digit) on the aimed road instead of changing its lines. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	bool mIsMarking = false;

	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	ERMMarkingKind mMarkingKind = ERMMarkingKind::Arrow;

	/** 1 = small, 2 = medium (double width and length). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	float mMarkingSize = 1.f;

	/** The buildable and the recipe it is "built with" (so it can be dismantled like any piece). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	TSubclassOf<ARMBuildableMarking> mMarkingClass;

	UPROPERTY(EditDefaultsOnly, Category = "Road Lines")
	TSubclassOf<UFGRecipe> mMarkingRecipe;

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
			InOut.Lanes = static_cast<uint8>(FMath::Clamp(D->mLanes, 1, 4));
			InOut.bContinuous = D->mContinuous;
		}
	}
};
