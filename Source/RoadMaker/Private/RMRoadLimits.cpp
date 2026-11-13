#include "RMRoadLimits.h"
#include "RoadMakerModule.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Misc/ConfigCacheIni.h"
#include "FGBuildGunModeDescriptor.h"
#include "Equipment/FGBuildGun.h"
#include "FGCharacterPlayer.h"
#include "HAL/PlatformTime.h"
#include "FGPlayerController.h"
#include "Input/FGBoundMappingContextHandle.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "FGInputLibrary.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "UObject/UObjectIterator.h"

namespace
{
	const TCHAR* LimitsSection = TEXT("RoadMaker");
	const TCHAR* LimitsKey = TEXT("bRoadLimitsOff");

	bool GLimitsLoaded = false;
	bool GLimitsOff = false;

	/** Alt press being watched: when it went down, and whether another key (Tab...) came with it. */
	double GAltDownTime = -1.0;
	bool GAltWithOtherKey = false;

	/** Road Maker's input context while a road is held (see BindRoadInput). */
	FBoundMappingContextHandle GRoadInputHandle;

	UInputAction* LimitsAction()
	{
		static UInputAction* Action = [] { UInputAction* A = LoadObject<UInputAction>(nullptr, TEXT("/RoadMaker/Input/IA_RM_ToggleLimits.IA_RM_ToggleLimits")); if (A) { A->AddToRoot(); } return A; }();
		return Action;
	}

	UInputMappingContext* RoadInputContext()
	{
		static UInputMappingContext* Context = [] { UInputMappingContext* C = LoadObject<UInputMappingContext>(nullptr, TEXT("/RoadMaker/Input/MC_RM_RoadTools.MC_RM_RoadTools")); if (C) { C->AddToRoot(); } return C; }();
		return Context;
	}

	/** The keys the limits action is bound to right now (the player's choice), else Alt. */
	TArray<FKey> LimitsKeys(APlayerController* PC)
	{
		TArray<FKey> Keys;
		ULocalPlayer* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
		UEnhancedInputLocalPlayerSubsystem* Input = LocalPlayer ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
		UEnhancedPlayerInput* PlayerInput = Input ? Input->GetPlayerInput() : nullptr;
		UInputAction* Action = LimitsAction();
		if (PlayerInput && Action)
		{
			for (const FEnhancedActionKeyMapping& Mapping : PlayerInput->GetEnhancedActionMappings())
			{
				if (Mapping.Action == Action && Mapping.Key.IsValid())
				{
					Keys.AddUnique(Mapping.Key);
				}
			}
		}
		if (Keys.Num() == 0)
		{
			Keys.Add(EKeys::LeftAlt);
			Keys.Add(EKeys::RightAlt);
		}
		return Keys;
	}

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
	bool bAltDown = false;
	for (const FKey& Key : LimitsKeys(PC))
	{
		bAltDown |= PC->IsInputKeyDown(Key);
	}
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

UInputAction* RMRoadLimits::GetLimitsAction()
{
	return LimitsAction();
}

void RMRoadLimits::BindRoadInput(APlayerController* PC, bool bBind)
{
	AFGPlayerController* FGPC = Cast<AFGPlayerController>(PC);
	UInputMappingContext* Context = RoadInputContext();
	if (!FGPC || !FGPC->IsLocalController() || !Context)
	{
		return;
	}
	// Counted: switching between road recipes makes the new hologram before the old one goes.
	static int32 Holders = 0;
	Holders = FMath::Max(0, Holders + (bBind ? 1 : -1));
	FGPC->SetMappingContextBoundWithHandle(GRoadInputHandle, Context, Holders > 0, 10);
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

namespace
{
	/** The bar's panel (the parent of the game's hint widgets) and our hint in it. */
	TWeakObjectPtr<UPanelWidget> GHintPanel;
	TWeakObjectPtr<UUserWidget> GLimitsHintWidget;
	double GNextHintSearch = 0.0;

	/** A game hint widget: a user widget with a text named mHintDescription. */
	UTextBlock* HintDescription(UUserWidget* Widget)
	{
		return Widget && Widget->WidgetTree ? Widget->WidgetTree->FindWidget<UTextBlock>(FName(TEXT("mHintDescription"))) : nullptr;
	}

	/** The other text of a hint widget (the key). */
	UTextBlock* HintKeyText(UUserWidget* Widget)
	{
		UTextBlock* Found = nullptr;
		if (Widget && Widget->WidgetTree)
		{
			Widget->WidgetTree->ForEachWidget([&Found](UWidget* Child)
			{
				UTextBlock* Text = Cast<UTextBlock>(Child);
				if (!Found && Text && Text->GetFName() != FName(TEXT("mHintDescription")))
				{
					Found = Text;
				}
			});
		}
		return Found;
	}

	/**
	 * Diagnostic (once per session, the first time the bar is looked for): the player's widgets on
	 * screen whose class mentions Build or Hint, with the user widgets and texts inside them.
	 */
	void LogHintWidgets(APlayerController* PC)
	{
		static bool bLogged = false;
		if (bLogged)
		{
			return;
		}
		bLogged = true;
		int32 Lines = 0;
		for (TObjectIterator<UUserWidget> It; It && Lines < 400; ++It)
		{
			UUserWidget* Screen = *It;
			if (!Screen || Screen->HasAnyFlags(RF_ClassDefaultObject) || Screen->GetOwningPlayer() != PC || !Screen->WidgetTree)
			{
				continue;
			}
			const FString ClassName = Screen->GetClass()->GetName();
			if (!ClassName.Contains(TEXT("Build")) && !ClassName.Contains(TEXT("Hint")) && !ClassName.Contains(TEXT("Keybind")))
			{
				continue;
			}
			UE_LOG(LogRoadMaker, Display, TEXT("HintBar: widget %s (%s) parent=%s visible=%d"), *Screen->GetName(), *ClassName,
				Screen->GetParent() ? *Screen->GetParent()->GetName() : TEXT("-"), Screen->IsVisible() ? 1 : 0);
			++Lines;
			Screen->WidgetTree->ForEachWidget([&Lines](UWidget* Child)
			{
				if (Lines >= 400 || !Child)
				{
					return;
				}
				if (UTextBlock* Text = Cast<UTextBlock>(Child))
				{
					UE_LOG(LogRoadMaker, Display, TEXT("HintBar:    text %s = \"%s\" (in %s)"), *Text->GetName(), *Text->GetText().ToString(),
						Text->GetParent() ? *Text->GetParent()->GetName() : TEXT("-"));
					++Lines;
				}
				else if (Cast<UUserWidget>(Child) || Cast<UPanelWidget>(Child))
				{
					UE_LOG(LogRoadMaker, Display, TEXT("HintBar:    %s %s (%s) in %s"), Cast<UUserWidget>(Child) ? TEXT("widget") : TEXT("panel"),
						*Child->GetName(), *Child->GetClass()->GetName(), Child->GetParent() ? *Child->GetParent()->GetName() : TEXT("-"));
					++Lines;
				}
			});
		}
		UE_LOG(LogRoadMaker, Display, TEXT("HintBar: %d line(s)"), Lines);
	}

	/** The build mode widget's hint panel and one of the game's hints in it, for this player. */
	UPanelWidget* FindHintPanel(APlayerController* PC, UUserWidget*& OutSample)
	{
		LogHintWidgets(PC);
		OutSample = nullptr;
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* Screen = *It;
			if (!Screen || Screen->HasAnyFlags(RF_ClassDefaultObject) || Screen->GetOwningPlayer() != PC
				|| !Screen->GetClass()->GetName().StartsWith(TEXT("Widget_BuildMode")) || !Screen->WidgetTree)
			{
				continue;
			}
			UPanelWidget* Panel = nullptr;
			Screen->WidgetTree->ForEachWidget([&Panel, &OutSample](UWidget* Child)
			{
				UUserWidget* Hint = Cast<UUserWidget>(Child);
				if (!Panel && Hint && HintDescription(Hint) && Hint->GetParent())
				{
					Panel = Hint->GetParent();
					OutSample = Hint;
				}
			});
			if (Panel)
			{
				return Panel;
			}
		}
		return nullptr;
	}
}

void RMRoadLimits::UpdateHintBar(APlayerController* PC, bool bShow)
{
	if (!bShow || !PC || !PC->IsLocalController())
	{
		if (UUserWidget* Hint = GLimitsHintWidget.Get())
		{
			Hint->RemoveFromParent();
		}
		GLimitsHintWidget.Reset();
		return;
	}
	UUserWidget* Hint = GLimitsHintWidget.Get();
	UPanelWidget* Panel = GHintPanel.Get();
	// Still in the bar (the game clears and refills the bar when it changes its hints).
	if (!Hint || !Panel || Hint->GetParent() != Panel)
	{
		const double Now = FPlatformTime::Seconds();
		if (Now < GNextHintSearch && !Panel)
		{
			return;
		}
		GNextHintSearch = Now + 1.0;
		UUserWidget* Sample = nullptr;
		Panel = FindHintPanel(PC, Sample);
		GHintPanel = Panel;
		static int32 SearchLogs = 0;
		if (SearchLogs++ < 5)
		{
			UE_LOG(LogRoadMaker, Display, TEXT("HintBar: panel %s, sample %s"), Panel ? *Panel->GetName() : TEXT("none"),
				Sample ? *Sample->GetClass()->GetName() : TEXT("none"));
		}
		if (!Panel || !Sample)
		{
			return;
		}
		if (!Hint || Hint->GetClass() != Sample->GetClass())
		{
			Hint = CreateWidget<UUserWidget>(PC, Sample->GetClass());
			GLimitsHintWidget = Hint;
		}
		if (!Hint)
		{
			return;
		}
		Hint->RemoveFromParent();
		Panel->AddChild(Hint);
	}
	// The texts every frame (the widget may set its own when built), and the current key.
	TArray<FKey> Keys = LimitsKeys(PC);
	if (UTextBlock* Description = HintDescription(Hint))
	{
		Description->SetText(NSLOCTEXT("RoadMaker", "LimitsHint", "Toggle Limits"));
	}
	if (UTextBlock* KeyText = HintKeyText(Hint))
	{
		const FText Name = Keys.Num() > 0 ? UFGInputLibrary::GetAbbreviatedKeyName(Keys[0]) : FText::GetEmpty();
		KeyText->SetText(Name.IsEmpty() && Keys.Num() > 0 ? Keys[0].GetDisplayName(false) : Name);
	}
	Hint->SetVisibility(ESlateVisibility::HitTestInvisible);
}
