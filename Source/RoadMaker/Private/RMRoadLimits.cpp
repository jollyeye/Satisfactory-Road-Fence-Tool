#include "RMRoadLimits.h"
#include "RoadMakerModule.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Misc/ConfigCacheIni.h"
#include "FGBuildGunModeDescriptor.h"
#include "Equipment/FGBuildGun.h"
#include "FGCharacterPlayer.h"
#include "HAL/PlatformTime.h"

namespace
{
	const TCHAR* LimitsSection = TEXT("RoadMaker");
	const TCHAR* LimitsKey = TEXT("bRoadLimitsOff");

	bool GLimitsLoaded = false;
	bool GLimitsOff = false;

	/** Alt press being watched: when it went down, and whether another key (Tab...) came with it. */
	double GAltDownTime = -1.0;
	bool GAltWithOtherKey = false;

	/** The build modes' own names (before the state was added). */
	TMap<FName, FText> GModeNames;

	void LoadLimits()
	{
		if (!GLimitsLoaded)
		{
			GLimitsLoaded = true;
			if (GConfig)
			{
				GConfig->GetBool(LimitsSection, LimitsKey, GLimitsOff, GGameUserSettingsIni);
			}
		}
	}

	void SaveLimits()
	{
		if (GConfig)
		{
			GConfig->SetBool(LimitsSection, LimitsKey, GLimitsOff, GGameUserSettingsIni);
			GConfig->Flush(false, GGameUserSettingsIni);
		}
	}
}

bool RMRoadLimits::AreOff()
{
	LoadLimits();
	return GLimitsOff;
}

bool RMRoadLimits::HandleInput(APlayerController* PC)
{
	if (!PC || !PC->IsLocalController())
	{
		return false;
	}
	LoadLimits();
	const bool bAltDown = PC->IsInputKeyDown(EKeys::LeftAlt) || PC->IsInputKeyDown(EKeys::RightAlt);
	const double Now = FPlatformTime::Seconds();
	if (bAltDown)
	{
		if (GAltDownTime < 0.0)
		{
			GAltDownTime = Now;
			GAltWithOtherKey = false;
		}
		// Alt+Tab (leaving the game), Alt+Enter and the like: not a toggle.
		if (PC->IsInputKeyDown(EKeys::Tab) || PC->IsInputKeyDown(EKeys::Enter) || PC->IsInputKeyDown(EKeys::F4))
		{
			GAltWithOtherKey = true;
		}
		return false;
	}
	if (GAltDownTime < 0.0)
	{
		return false;
	}
	// Released: a short press on its own toggles.
	const bool bToggle = !GAltWithOtherKey && Now - GAltDownTime < 0.6;
	GAltDownTime = -1.0;
	if (!bToggle)
	{
		return false;
	}
	GLimitsOff = !GLimitsOff;
	SaveLimits();
	UE_LOG(LogRoadMaker, Display, TEXT("Road limits %s"), GLimitsOff ? TEXT("off") : TEXT("on"));
	return true;
}

void RMRoadLimits::LabelModes(const TArray<TSubclassOf<UFGBuildGunModeDescriptor>>& Modes, APlayerController* PC)
{
	static FTextProperty* NameProperty = FindFProperty<FTextProperty>(UFGBuildGunModeDescriptor::StaticClass(), TEXT("mDisplayName"));
	if (!NameProperty)
	{
		return;
	}
	const FText State = AreOff() ? NSLOCTEXT("RoadMaker", "LimitsOff", "Limits: OFF") : NSLOCTEXT("RoadMaker", "LimitsOn", "Limits: ON");
	for (const TSubclassOf<UFGBuildGunModeDescriptor>& Mode : Modes)
	{
		// Only Road Maker's own modes (the game's are shared by every building).
		UFGBuildGunModeDescriptor* Cdo = Mode && Mode->GetPathName().StartsWith(TEXT("/RoadMaker/")) ? Mode->GetDefaultObject<UFGBuildGunModeDescriptor>() : nullptr;
		if (!Cdo)
		{
			continue;
		}
		const FName Key = Mode->GetFName();
		const FText* Known = GModeNames.Find(Key);
		const FText Base = Known ? *Known : GModeNames.Add(Key, Cdo->GetDisplayNameInternal());
		NameProperty->SetPropertyValue_InContainer(Cdo, FText::Format(NSLOCTEXT("RoadMaker", "ModeWithLimits", "{0} ({1})"), Base, State));
	}
	// The build gun's mode label updates when the mode changes: announce the current one again.
	AFGCharacterPlayer* Player = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
	if (AFGBuildGun* Gun = Player ? Player->GetBuildGun() : nullptr)
	{
		Gun->mOnBuildGunModeChanged.Broadcast(Gun->GetCurrentBuildGunMode());
	}
}
