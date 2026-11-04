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
#include "Kismet/KismetRenderingLibrary.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Texture.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

void URMGameWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	Super::DispatchLifecycleEvent(Phase);
	if (Phase == ELifecyclePhase::INITIALIZATION)
	{
		AddRecipesToGameSchematic();
		RegisterFenceWallMaterials();
	}
	if (Phase == ELifecyclePhase::POST_INITIALIZATION)
	{
		AddRecipesToGameSchematic(); // in case the earlier phase ran before the class was loaded
		GrantRecipesOfPurchasedSchematics();
		GrantExtraRecipesIfPurchased();
		EnsureRoadSwatchGroup();
		DumpGameTexturesForDevelopment();
	}
}

void URMGameWorldModule::DumpGameTexturesForDevelopment()
{
	static bool bDone = false;
	UWorld* World = GetWorld();
	const FString Dir = TEXT("D:/SFMods/TextureDump");
	if (bDone || !World || World->GetNetMode() == NM_DedicatedServer || !IFileManager::Get().DirectoryExists(TEXT("D:/SFMods")))
	{
		return;
	}
	bDone = true;
	IFileManager::Get().MakeDirectory(*Dir, true);
	UMaterialInterface* ColorMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_TextureDump.M_RM_TextureDump"));
	UMaterialInterface* NormalMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_TextureDumpN.M_RM_TextureDumpN"));
	struct FDump { const TCHAR* Path; bool bNormal; };
	const FDump Dumps[] = {
		{ TEXT("/Game/FactoryGame/Buildable/Building/Wall/ConcreteWallSet/Textures/TX_WallSetConcrete_01_BC.TX_WallSetConcrete_01_BC"), false },
		{ TEXT("/Game/FactoryGame/Buildable/Building/Wall/ConcreteWallSet/Textures/TX_WallSetConcrete_01_N.TX_WallSetConcrete_01_N"), true },
		{ TEXT("/Game/FactoryGame/Buildable/Building/Wall/ConcreteWallSet/Textures/TX_WallSetConcrete_8x1_BC.TX_WallSetConcrete_8x1_BC"), false },
		{ TEXT("/Game/FactoryGame/Buildable/Building/Wall/FicsitWallSet/Textures/TX_WallSetOrange_BC.TX_WallSetOrange_BC"), false },
		{ TEXT("/Game/FactoryGame/Buildable/Building/Wall/FicsitWallSet/Textures/TX_WallSetOrange_N.TX_WallSetOrange_N"), true },
		{ TEXT("/Game/FactoryGame/Buildable/Building/Wall/SteelWallSet/Textures/TX_WallSetSteel_01_BC.TX_WallSetSteel_01_BC"), false },
		{ TEXT("/Game/FactoryGame/Buildable/Building/Wall/SteelWallSet/Textures/TX_WallSetSteel_01_N.TX_WallSetSteel_01_N"), true },
	};
	for (const FDump& D : Dumps)
	{
		UTexture* Tex = LoadObject<UTexture>(nullptr, D.Path);
		UMaterialInterface* Mat = D.bNormal ? NormalMat : ColorMat;
		if (!Tex || !Mat)
		{
			UE_LOG(LogRoadMaker, Warning, TEXT("Texture dump: cannot load %s (material %s)"), D.Path, Mat ? TEXT("ok") : TEXT("missing"));
			continue;
		}
		const int32 W = FMath::Clamp(FMath::RoundToInt(Tex->GetSurfaceWidth()), 64, 4096);
		const int32 H = FMath::Clamp(FMath::RoundToInt(Tex->GetSurfaceHeight()), 64, 4096);
		UTextureRenderTarget2D* RT = UKismetRenderingLibrary::CreateRenderTarget2D(World, W, H, RTF_RGBA8_SRGB);
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Mat, this);
		MID->SetTextureParameterValue(TEXT("Tex"), Tex);
		UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, RT, MID);
		FString Package, Object;
		FString(D.Path).Split(TEXT("."), &Package, &Object);
		const FString Name = Object + TEXT(".png");
		UKismetRenderingLibrary::ExportRenderTarget(World, RT, Dir, Name);
		UE_LOG(LogRoadMaker, Display, TEXT("Texture dump: %s (%dx%d) -> %s/%s"), D.Path, W, H, *Dir, *Name);
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
			// The game's wall materials only apply to the buildables they list (the game's walls).
			if (Variant && Desc->mValidBuildables.Num() > 0)
			{
				Desc->mValidBuildables.AddUnique(Variant);
			}
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
