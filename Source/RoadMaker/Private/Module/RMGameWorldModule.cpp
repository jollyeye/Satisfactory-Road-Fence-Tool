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
#include "FGFactoryColoringTypes.h"
#include "Buildables/FGBuildable.h"

void URMGameWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	Super::DispatchLifecycleEvent(Phase);
	if (Phase == ELifecyclePhase::INITIALIZATION)
	{
		AddRecipesToGameSchematic();
		// The game's wall materials list the fence walls (so the build menu can treat them like the
		// game's walls), but the customizer switches them in place (RMFenceWallPaint).
		RegisterFenceWallMaterials();
	}
	if (Phase == ELifecyclePhase::POST_INITIALIZATION)
	{
		AddRecipesToGameSchematic(); // in case the earlier phase ran before the class was loaded
		AddRecipesAlongsideGameRecipes();
		GrantRecipesOfPurchasedSchematics();
		GrantExtraRecipesIfPurchased();
		EnsureRoadSwatchGroup();
	}
}

TMap<TSubclassOf<UFGSchematic>, TArray<TSubclassOf<UFGRecipe>>> URMGameWorldModule::GetGameSchematicRecipes() const
{
	TMap<TSubclassOf<UFGSchematic>, TArray<TSubclassOf<UFGRecipe>>> Out;
	if (mExtraRecipesSchematic)
	{
		Out.FindOrAdd(mExtraRecipesSchematic).Append(mExtraRecipes);
	}
	for (const TPair<TSubclassOf<UFGRecipe>, TSubclassOf<UFGSchematic>>& Pair : mRecipeUnlockedBy)
	{
		if (Pair.Key && Pair.Value)
		{
			Out.FindOrAdd(Pair.Value).AddUnique(Pair.Key);
		}
	}
	return Out;
}

void URMGameWorldModule::AddRecipesToGameSchematic()
{
	// mRecipes of UFGUnlockRecipe is protected: write it through reflection. The schematic's default
	// object holds the unlock objects the game reads, so the milestone shows and unlocks the recipes.
	FArrayProperty* RecipesProp = FindFProperty<FArrayProperty>(UFGUnlockRecipe::StaticClass(), TEXT("mRecipes"));
	if (!RecipesProp)
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("UFGUnlockRecipe::mRecipes not found; extra recipes not added"));
		return;
	}
	for (const TPair<TSubclassOf<UFGSchematic>, TArray<TSubclassOf<UFGRecipe>>>& Entry : GetGameSchematicRecipes())
	{
		bool bDone = false;
		for (UFGUnlock* Unlock : UFGSchematic::GetUnlocks(Entry.Key))
		{
			if (UFGUnlockRecipe* RecipeUnlock = Cast<UFGUnlockRecipe>(Unlock))
			{
				TArray<TSubclassOf<UFGRecipe>>* List = RecipesProp->ContainerPtrToValuePtr<TArray<TSubclassOf<UFGRecipe>>>(RecipeUnlock);
				int32 Added = 0;
				for (const TSubclassOf<UFGRecipe>& Recipe : Entry.Value)
				{
					if (Recipe && !List->Contains(Recipe))
					{
						List->Add(Recipe);
						++Added;
					}
				}
				if (Added > 0)
				{
					UE_LOG(LogRoadMaker, Display, TEXT("Added %d recipe(s) to %s"), Added, *Entry.Key->GetName());
				}
				bDone = true;
				break;
			}
		}
		if (!bDone)
		{
			UE_LOG(LogRoadMaker, Warning, TEXT("%s has no recipe unlock; extra recipes not added"), *Entry.Key->GetName());
		}
	}
}

void URMGameWorldModule::RegisterFenceWallMaterials()
{
	for (const TPair<TSubclassOf<UFGFactoryCustomizationDescriptor_Material>, TSubclassOf<UFGRecipe>>& Entry : mFenceWallMaterialRecipes)
	{
		UFGFactoryCustomizationDescriptor_Material* Desc = Entry.Key ? Entry.Key.GetDefaultObject() : nullptr;
		if (!Desc || !Entry.Value)
		{
			continue;
		}
		int32 Added = 0;
		for (const TSubclassOf<AFGBuildable>& Variant : mFenceWallVariants)
		{
			if (Variant && !Desc->GetBuildableMap().Contains(Variant) && Desc->AddType(Variant, Entry.Value))
			{
				++Added;
			}
		}
		if (Added > 0)
		{
			UE_LOG(LogRoadMaker, Display, TEXT("Fence walls: %s -> %s for %d variant(s)"), *Entry.Key->GetName(), *Entry.Value->GetName(), Added);
		}
	}
}

void URMGameWorldModule::AddRecipesAlongsideGameRecipes()
{
	UWorld* World = GetWorld();
	AFGSchematicManager* Schematics = World ? AFGSchematicManager::Get(World) : nullptr;
	FArrayProperty* RecipesProp = FindFProperty<FArrayProperty>(UFGUnlockRecipe::StaticClass(), TEXT("mRecipes"));
	if (!Schematics || !RecipesProp || mUnlockAlongside.Num() == 0)
	{
		return;
	}
	AFGRecipeManager* Recipes = AFGRecipeManager::Get(World);
	TArray<TSubclassOf<UFGSchematic>> All;
	Schematics->GetAllSchematics(All);
	for (const TSubclassOf<UFGSchematic>& Schematic : All)
	{
		if (!Schematic)
		{
			continue;
		}
		for (UFGUnlock* Unlock : UFGSchematic::GetUnlocks(Schematic))
		{
			UFGUnlockRecipe* RecipeUnlock = Cast<UFGUnlockRecipe>(Unlock);
			if (!RecipeUnlock)
			{
				continue;
			}
			TArray<TSubclassOf<UFGRecipe>>* List = RecipesProp->ContainerPtrToValuePtr<TArray<TSubclassOf<UFGRecipe>>>(RecipeUnlock);
			TArray<TSubclassOf<UFGRecipe>> ToAdd;
			for (const TPair<TSubclassOf<UFGRecipe>, TSubclassOf<UFGRecipe>>& Pair : mUnlockAlongside)
			{
				if (Pair.Key && Pair.Value && List->Contains(Pair.Key) && !List->Contains(Pair.Value))
				{
					ToAdd.Add(Pair.Value);
				}
			}
			if (ToAdd.Num() == 0)
			{
				continue;
			}
			List->Append(ToAdd);
			UE_LOG(LogRoadMaker, Display, TEXT("%d recipe(s) now also unlock with %s"), ToAdd.Num(), *Schematic->GetName());
			if (Recipes && World->GetNetMode() != NM_Client && Schematics->IsSchematicPurchased(Schematic))
			{
				Recipes->AddAvailableRecipes(ToAdd);
			}
		}
	}
}

void URMGameWorldModule::GrantExtraRecipesIfPurchased()
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
	for (const TPair<TSubclassOf<UFGSchematic>, TArray<TSubclassOf<UFGRecipe>>>& Entry : GetGameSchematicRecipes())
	{
		if (Schematics->IsSchematicPurchased(Entry.Key))
		{
			TArray<TSubclassOf<UFGRecipe>> ToAdd = Entry.Value;
			Recipes->AddAvailableRecipes(ToAdd);
			UE_LOG(LogRoadMaker, Display, TEXT("Ensured %d extra recipe(s) of bought %s"), ToAdd.Num(), *Entry.Key->GetName());
		}
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
