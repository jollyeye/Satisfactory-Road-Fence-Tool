#include "Module/RMGameWorldModule.h"
#include "RoadMakerModule.h"
#include "FGSchematic.h"
#include "FGSchematicManager.h"
#include "FGRecipeManager.h"
#include "Unlocks/FGUnlockRecipe.h"
#include "Engine/World.h"

void URMGameWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	Super::DispatchLifecycleEvent(Phase);
	if (Phase == ELifecyclePhase::POST_INITIALIZATION)
	{
		GrantRecipesOfPurchasedSchematics();
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
				UE_LOG(LogRoadMaker, Log, TEXT("Ensured %d recipe(s) of bought schematic %s"), ToAdd.Num(), *Schematic->GetName());
			}
		}
	}
}
