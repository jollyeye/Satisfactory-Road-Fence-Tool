#include "Signs/RMIconLibraryTools.h"
#include "FGIconLibrary.h"
#include "FGIconDatabaseSubsystem.h"
#include "RoadMakerModule.h"
#include "Engine/World.h"
#include "Engine/AssetManager.h"
#include "Misc/CoreDelegates.h"
#include "UObject/UnrealType.h"

void URMIconLibraryTools::SetIcons(UFGIconLibrary* Library, const TArray<UObject*>& Icons, const TArray<FString>& Names, const TArray<bool>& Animated,
	bool bBackgrounds)
{
	if (!Library)
	{
		return;
	}
	Library->mIconData.Reset();
	Library->mMonochromeIconData.Reset();
	Library->mMaterialIconData.Reset();
	for (int32 I = 0; I < Icons.Num(); ++I)
	{
		FIconData D;
		D.ID = I;
		D.Texture = Icons[I];
		D.IconName = FText::FromString(Names.IsValidIndex(I) ? Names[I] : FString::Printf(TEXT("Road Maker %d"), I));
		D.DisplayNameOverride = true;
		D.IconType = bBackgrounds ? EIconType::ESIT_Material : EIconType::ESIT_Monochrome;
		D.Hidden = false;
		D.SearchOnly = false;
		D.Animated = Animated.IsValidIndex(I) && Animated[I];
		Library->mIconData.Add(D);
		(bBackgrounds ? Library->mMaterialIconData : Library->mMonochromeIconData).Add(D);
	}
	Library->MarkPackageDirty();
	UE_LOG(LogRoadMaker, Display, TEXT("Icon library %s: %d icons"), *Library->GetPathName(), Icons.Num());
}

void URMIconLibraryTools::RegisterLibrary()
{
	auto Register = []()
	{
		if (!UAssetManager::IsInitialized())
		{
			return;
		}
		UAssetManager& AM = UAssetManager::Get();
		const int32 Found = AM.ScanPathForPrimaryAssets(FPrimaryAssetType(TEXT("FGIconLibrary")), TEXT("/RoadMaker/Icons"), UFGIconLibrary::StaticClass(), false);
		UE_LOG(LogRoadMaker, Display, TEXT("Sign icons: asset manager scan of /RoadMaker/Icons found %d icon librar%s"), Found, Found == 1 ? TEXT("y") : TEXT("ies"));
	};
	if (UAssetManager::IsInitialized())
	{
		Register();
	}
	else
	{
		FCoreDelegates::OnPostEngineInit.AddLambda(Register);
	}
}

namespace
{
	bool HasOurIcons(const AFGIconDatabaseSubsystem* Db, TArray<FString>* OutNames = nullptr)
	{
		bool bAny = false;
		for (const FIconData& D : Db->GetAllIconData())
		{
			if (D.Texture.ToSoftObjectPath().ToString().StartsWith(TEXT("/RoadMaker/")))
			{
				bAny = true;
				if (OutNames)
				{
					OutNames->Add(FString::Printf(TEXT("%s (id %d, type %d%s)"), *D.IconName.ToString(), D.ID, static_cast<int32>(D.IconType),
						D.Animated ? TEXT(", animated") : TEXT("")));
				}
			}
		}
		return bAny;
	}

	void LogCategories(const AFGIconDatabaseSubsystem* Db)
	{
		TMap<int32, TArray<FString>> ByType;
		for (const FIconData& D : Db->GetAllIconData())
		{
			TArray<FString>& Names = ByType.FindOrAdd(static_cast<int32>(D.IconType));
			Names.Add(D.IconName.ToString() + (D.Hidden ? TEXT(" [hidden]") : TEXT("")));
		}
		for (const auto& Pair : ByType)
		{
			TArray<FString> Some;
			for (int32 I = 0; I < Pair.Value.Num() && Some.Num() < 25; ++I)
			{
				Some.Add(Pair.Value[I]);
			}
			UE_LOG(LogRoadMaker, Display, TEXT("Sign icons: category %d has %d icons: %s"), Pair.Key, Pair.Value.Num(), *FString::Join(Some, TEXT(", ")));
		}
	}
}

void URMIconLibraryTools::EnsureOurIcons(UWorld* World)
{
	static TWeakObjectPtr<AFGIconDatabaseSubsystem> GDone;
	static TWeakObjectPtr<AFGIconDatabaseSubsystem> GInjected;
	static bool bLoggedCategories = false;
	AFGIconDatabaseSubsystem* Db = World ? AFGIconDatabaseSubsystem::Get(World) : nullptr;
	if (!Db || GDone.Get() == Db || Db->GetAllIconData().Num() == 0)
	{
		return;
	}
	if (!bLoggedCategories)
	{
		bLoggedCategories = true;
		LogCategories(Db);
	}
	TArray<FString> Ours;
	if (HasOurIcons(Db, &Ours))
	{
		GDone = Db;
		UE_LOG(LogRoadMaker, Display, TEXT("Sign icons: %s ours: %s"), GInjected.Get() == Db ? TEXT("added") : TEXT("the game found"),
			*FString::Join(Ours, TEXT(", ")));
		return;
	}
	if (GInjected.Get() == Db)
	{
		return; // added, not rebuilt yet (or it failed: logged below once)
	}
	GInjected = Db;
	// The game did not find our library: add it to the database's list and let the database rebuild (its OnRep does).
	UFGIconLibrary* Library = LoadObject<UFGIconLibrary>(nullptr, LibraryPath);
	FArrayProperty* ListProp = FindFProperty<FArrayProperty>(AFGIconDatabaseSubsystem::StaticClass(), TEXT("mGlobalIconLibraries"));
	UFunction* Rebuild = Db->FindFunction(TEXT("OnRep_GlobalIconLibraries"));
	if (!Library || !ListProp || !Rebuild)
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("Sign icons: cannot add ours (library %s, list %s, rebuild %s)"), Library ? TEXT("ok") : TEXT("missing"),
			ListProp ? TEXT("ok") : TEXT("missing"), Rebuild ? TEXT("ok") : TEXT("missing"));
		return;
	}
	TArray<TSoftObjectPtr<UFGIconLibrary>>* List = ListProp->ContainerPtrToValuePtr<TArray<TSoftObjectPtr<UFGIconLibrary>>>(Db);
	const TSoftObjectPtr<UFGIconLibrary> Ours2(Library);
	if (!List->Contains(Ours2))
	{
		List->Add(Ours2);
	}
	Db->ProcessEvent(Rebuild, nullptr);
	UE_LOG(LogRoadMaker, Display, TEXT("Sign icons: the game did not find our library; added it (%d libraries), database now %d icons, ours %s"),
		List->Num(), Db->GetAllIconData().Num(), HasOurIcons(Db) ? TEXT("in") : TEXT("still missing"));
}
