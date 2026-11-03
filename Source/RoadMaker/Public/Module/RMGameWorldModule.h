// Root game world module of Road Maker. Besides registering the schematics (done by SML), it makes
// sure recipes added to an already bought Road Maker milestone (e.g. after a mod update) unlock.
#pragma once

#include "CoreMinimal.h"
#include "Module/GameWorldModule.h"
#include "RMGameWorldModule.generated.h"

UCLASS(Blueprintable)
class ROADMAKER_API URMGameWorldModule : public UGameWorldModule
{
	GENERATED_BODY()

public:
	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

protected:
	/** For every bought schematic of this mod, add any of its recipes the save does not have yet. */
	void GrantRecipesOfPurchasedSchematics();

	/** Adds "Roads" to the game's swatch groups (customizer "set as default for" list) if missing. */
	void EnsureRoadSwatchGroup();

	/** Swatch new roads use until the player picks another default for "Roads". */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TSubclassOf<class UFGFactoryCustomizationDescriptor_Swatch> mDefaultRoadSwatch;
};
