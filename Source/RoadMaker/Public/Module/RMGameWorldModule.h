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

	/** Adds mExtraRecipes to the recipe unlock of mExtraRecipesSchematic (a game milestone). */
	void AddRecipesToGameSchematic();

	/** If that game milestone is already bought in this save, unlock the extra recipes now. */
	void GrantExtraRecipesIfPurchased();

	/**
	 * Development only: draws a few game textures into PNG files in D:/SFMods/TextureDump (only if
	 * D:/SFMods exists), because the editor project has placeholder textures only.
	 */
	void DumpGameTexturesForDevelopment();

	/**
	 * Customizer materials for the fence walls: every fence wall variant is added to each game wall
	 * material (concrete / FICSIT / steel) with the recipe of the variant in that material, so the
	 * customizer's Material tab swaps a fence wall like a game wall.
	 */
	void RegisterFenceWallMaterials();

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TArray<TSubclassOf<class AFGBuildable>> mFenceWallVariants;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TMap<TSubclassOf<class UFGFactoryCustomizationDescriptor_Material>, TSubclassOf<class UFGRecipe>> mFenceWallMaterialRecipes;

	/**
	 * Recipes unlocked together with a game recipe (key): our recipe is added to every schematic that
	 * unlocks the game's one, e.g. the concrete fence wall with the customizer's concrete wall material.
	 */
	void AddRecipesAlongsideGameRecipes();

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TMap<TSubclassOf<class UFGRecipe>, TSubclassOf<class UFGRecipe>> mUnlockAlongside;

	/** A game milestone that also unlocks mExtraRecipes (the fence wall: Tier 1 Base Building). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TSubclassOf<class UFGSchematic> mExtraRecipesSchematic;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TArray<TSubclassOf<class UFGRecipe>> mExtraRecipes;

	/** Swatch new roads use until the player picks another default for "Roads". */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	TSubclassOf<class UFGFactoryCustomizationDescriptor_Swatch> mDefaultRoadSwatch;
};
