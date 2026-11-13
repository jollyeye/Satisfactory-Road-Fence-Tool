#include "Signs/RMIconLibraryTools.h"
#include "FGIconLibrary.h"
#include "FGIconDatabaseSubsystem.h"
#include "RoadMakerModule.h"
#include "Engine/World.h"

void URMIconLibraryTools::SetIcons(UFGIconLibrary* Library, const TArray<UObject*>& Icons, const TArray<FString>& Names, const TArray<bool>& Animated)
{
	if (!Library)
	{
		return;
	}
	Library->mIconData.Reset();
	Library->mMonochromeIconData.Reset();
	for (int32 I = 0; I < Icons.Num(); ++I)
	{
		FIconData D;
		D.ID = I;
		D.Texture = Icons[I];
		D.IconName = FText::FromString(Names.IsValidIndex(I) ? Names[I] : FString::Printf(TEXT("Road Maker %d"), I));
		D.DisplayNameOverride = true;
		D.IconType = EIconType::ESIT_Monochrome;
		D.Hidden = false;
		D.SearchOnly = false;
		D.Animated = Animated.IsValidIndex(I) && Animated[I];
		Library->mIconData.Add(D);
		Library->mMonochromeIconData.Add(D);
	}
	Library->MarkPackageDirty();
	UE_LOG(LogRoadMaker, Display, TEXT("Icon library %s: %d icons"), *Library->GetPathName(), Icons.Num());
}

void URMIconLibraryTools::LogOurIcons(UWorld* World)
{
	static bool bDone = false;
	AFGIconDatabaseSubsystem* Db = (!bDone && World) ? AFGIconDatabaseSubsystem::Get(World) : nullptr;
	if (!Db || Db->GetAllIconData().Num() == 0)
	{
		return;
	}
	bDone = true;
	TArray<FString> Ours;
	for (const FIconData& D : Db->GetAllIconData())
	{
		const UObject* T = D.Texture.Get();
		const FString Path = D.Texture.ToSoftObjectPath().ToString();
		if (Path.StartsWith(TEXT("/RoadMaker/")))
		{
			Ours.Add(FString::Printf(TEXT("%s (id %d%s%s)"), *D.IconName.ToString(), D.ID, D.Animated ? TEXT(", animated") : TEXT(""), T ? TEXT("") : TEXT(", not loaded")));
		}
	}
	if (Ours.Num() > 0)
	{
		UE_LOG(LogRoadMaker, Display, TEXT("Sign icons: the game took %d of ours: %s"), Ours.Num(), *FString::Join(Ours, TEXT(", ")));
	}
	else
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("Sign icons: none of ours in the game's icon database (%d icons in it); the library /RoadMaker/Icons/IL_RoadMaker was not found by the game"),
			Db->GetAllIconData().Num());
	}
}
