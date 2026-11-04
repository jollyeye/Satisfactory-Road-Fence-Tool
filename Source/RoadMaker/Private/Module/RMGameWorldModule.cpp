#include "Module/RMGameWorldModule.h"
#include "RoadMakerModule.h"
#include "FGSchematic.h"
#include "FGSchematicManager.h"
#include "FGRecipeManager.h"
#include "Unlocks/FGUnlockRecipe.h"
#include "Engine/World.h"
#include "FGGameState.h"
#include "FGSwatchGroup.h"
#include "Buildables/RMSwatchGroupRoads.h"
#include "UObject/UnrealType.h"

void URMGameWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	Super::DispatchLifecycleEvent(Phase);
	if (Phase == ELifecyclePhase::INITIALIZATION)
	{
		AddRecipesToGameSchematic();
	}
	if (Phase == ELifecyclePhase::POST_INITIALIZATION)
	{
		AddRecipesToGameSchematic(); // in case the earlier phase ran before the class was loaded
		GrantRecipesOfPurchasedSchematics();
		GrantExtraRecipesIfPurchased();
		EnsureRoadSwatchGroup();
	}
}

void URMGameWorldModule::AddRecipesToGameSchematic()
{
	if (!mExtraRecipesSchematic || mExtraRecipes.Num() == 0)
	{
		return;
	}
	// mRecipes of UFGUnlockRecipe is protected: write it through reflection. The schematic's default
	// object holds the unlock objects the game reads, so the milestone shows and unlocks the recipes.
	FArrayProperty* RecipesProp = FindFProperty<FArrayProperty>(UFGUnlockRecipe::StaticClass(), TEXT("mRecipes"));
	if (!RecipesProp)
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("UFGUnlockRecipe::mRecipes not found; extra recipes not added"));
		return;
	}
	for (UFGUnlock* Unlock : UFGSchematic::GetUnlocks(mExtraRecipesSchematic))
	{
		if (UFGUnlockRecipe* RecipeUnlock = Cast<UFGUnlockRecipe>(Unlock))
		{
			TArray<TSubclassOf<UFGRecipe>>* List = RecipesProp->ContainerPtrToValuePtr<TArray<TSubclassOf<UFGRecipe>>>(RecipeUnlock);
			int32 Added = 0;
			for (const TSubclassOf<UFGRecipe>& Recipe : mExtraRecipes)
			{
				if (Recipe && !List->Contains(Recipe))
				{
					List->Add(Recipe);
					++Added;
				}
			}
			if (Added > 0)
			{
				UE_LOG(LogRoadMaker, Display, TEXT("Added %d recipe(s) to %s"), Added, *mExtraRecipesSchematic->GetName());
			}
			return;
		}
	}
	UE_LOG(LogRoadMaker, Warning, TEXT("%s has no recipe unlock; extra recipes not added"), *mExtraRecipesSchematic->GetName());
}

void URMGameWorldModule::GrantExtraRecipesIfPurchased()
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client || !mExtraRecipesSchematic)
	{
		return;
	}
	AFGSchematicManager* Schematics = AFGSchematicManager::Get(World);
	AFGRecipeManager* Recipes = AFGRecipeManager::Get(World);
	if (Schematics && Recipes && Schematics->IsSchematicPurchased(mExtraRecipesSchematic))
	{
		TArray<TSubclassOf<UFGRecipe>> ToAdd = mExtraRecipes;
		Recipes->AddAvailableRecipes(ToAdd);
		UE_LOG(LogRoadMaker, Display, TEXT("Ensured %d extra recipe(s) of bought %s"), ToAdd.Num(), *mExtraRecipesSchematic->GetName());
	}
}

void URMGameWorldModule::GrantRecipesOfPurchasedSchematics()
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	AFGSchematicManager* Schematics = AFGSchematicManager::Get(World);
	AFGRecipeManager* Recipes = AFGRecipeManager::Get(World);
	if (!Schematics || !Recipes)
	{
		return;
	}

	for (const TSubclassOf<UFGSchematic>& Schematic : mSchematics)
	{
		if (!Schematic || !Schematics->IsSchematicPurchased(Schematic))
		{
			continue;
		}
		for (UFGUnlock* Unlock : UFGSchematic::GetUnlocks(Schematic))
		{
			if (const UFGUnlockRecipe* RecipeUnlock = Cast<UFGUnlockRecipe>(Unlock))
			{
				TArray<TSubclassOf<UFGRecipe>> ToAdd = RecipeUnlock->GetRecipesToUnlock();
				Recipes->AddAvailableRecipes(ToAdd);
				UE_LOG(LogRoadMaker, Display, TEXT("Ensured %d recipe(s) of bought schematic %s"), ToAdd.Num(), *Schematic->GetName());
			}
		}
	}
}

void URMGameWorldModule::EnsureRoadSwatchGroup()
{
	UWorld* World = GetWorld();
	AFGGameState* GameState = World ? World->GetGameState<AFGGameState>() : nullptr;
	if (!GameState || World->GetNetMode() == NM_Client)
	{
		return;
	}
	// mSwatchGroupDatum is private; it is a saved, replicated UPROPERTY, so go through reflection.
	FArrayProperty* Prop = FindFProperty<FArrayProperty>(AFGGameState::StaticClass(), TEXT("mSwatchGroupDatum"));
	if (!Prop)
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("mSwatchGroupDatum not found; Roads swatch group not added"));
		return;
	}
	TArray<FSwatchGroupData>* Datum = Prop->ContainerPtrToValuePtr<TArray<FSwatchGroupData>>(GameState);
	for (const FSwatchGroupData& Entry : *Datum)
	{
		if (Entry.SwatchGroup == URMSwatchGroup_Roads::StaticClass())
		{
			return; // already there (from the save, or the player's choice)
		}
	}
	FSwatchGroupData Roads;
	Roads.SwatchGroup = URMSwatchGroup_Roads::StaticClass();
	Roads.Swatch = mDefaultRoadSwatch;
	Datum->Add(Roads);
	GameState->ForceNetUpdate();
	UE_LOG(LogRoadMaker, Display, TEXT("Added Roads swatch group (default %s)"), mDefaultRoadSwatch ? *mDefaultRoadSwatch->GetName() : TEXT("none"));
}
