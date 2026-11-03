#include "Customization/RMRoadLinePaint.h"
#include "RoadMakerModule.h"
#include "Customization/RMRoadLineDescriptor.h"
#include "Buildables/RMBuildableRoad.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunPaint.h"
#include "FGCustomizationRecipe.h"
#include "FGCharacterPlayer.h"
#include "Patching/NativeHookManager.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Components/Image.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/UObjectIterator.h"

namespace
{
	TSubclassOf<URMRoadLineDescriptor> GActiveSides;
	TSubclassOf<URMRoadLineDescriptor> GActiveLanes;
}

bool RMRoadLinePaint::IsAnyActive()
{
	return GActiveSides || GActiveLanes;
}

FText RMRoadLinePaint::GetActiveText()
{
	TArray<FString> Parts;
	for (TSubclassOf<URMRoadLineDescriptor> D : { GActiveSides, GActiveLanes })
	{
		if (D)
		{
			Parts.Add(UFGItemDescriptor::GetItemName(D).ToString());
		}
	}
	return FText::FromString(TEXT("Road Lines: ") + FString::Join(Parts, TEXT(" + ")));
}

/** Road under the player's crosshair, if any. */
ARMBuildableRoad* RMRoadLinePaint::TraceRoad(APlayerController* PC)
{
	if (!PC || !PC->GetWorld())
	{
		return nullptr;
	}
	FVector Eye;
	FRotator View;
	PC->GetPlayerViewPoint(Eye, View);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RMRoadLineTrace), true, PC->GetPawn());
	FHitResult Hit;
	if (!PC->GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + View.Vector() * 10000.f, ECC_Visibility, Params))
	{
		return nullptr;
	}
	return Cast<ARMBuildableRoad>(Hit.GetActor());
}

bool RMRoadLinePaint::IsPaintStateActive(APlayerController* PC)
{
	AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
	AFGBuildGun* Gun = Character ? Character->GetBuildGun() : nullptr;
	return Gun && Gun->IsEquipped() && Cast<UFGBuildGunStatePaint>(Gun->GetCurrentState()) != nullptr;
}

TSharedRef<SWidget> URMRoadLinesHint::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UVerticalBox* Screen = WidgetTree->ConstructWidget<UVerticalBox>();
		WidgetTree->RootWidget = Screen;
		UVerticalBoxSlot* Fill = Screen->AddChildToVerticalBox(WidgetTree->ConstructWidget<USpacer>());
		Fill->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		Box = WidgetTree->ConstructWidget<UBorder>();
		Box->SetPadding(FMargin(12.f, 6.f));
		UVerticalBoxSlot* BoxSlot = Screen->AddChildToVerticalBox(Box);
		BoxSlot->SetHorizontalAlignment(HAlign_Center);
		BoxSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 180.f));
		Text = WidgetTree->ConstructWidget<UTextBlock>();
		FSlateFontInfo Font = Text->GetFont();
		Font.Size = 16;
		Text->SetFont(Font);
		Text->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		Box->SetContent(Text);
	}
	return Super::RebuildWidget();
}

void URMRoadLinesHint::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (!Box || !Text)
	{
		return;
	}
	APlayerController* PC = GetOwningPlayer();
	const bool bShow = RMRoadLinePaint::IsAnyActive() && RMRoadLinePaint::IsPaintStateActive(PC);
	Box->SetVisibility(bShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	if (!bShow)
	{
		return;
	}
	if (RMRoadLinePaint::TraceRoad(PC))
	{
		Text->SetText(RMRoadLinePaint::GetActiveText());
		Box->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.025f, 0.75f));
	}
	else
	{
		// Same warning as before: the option only applies to roads.
		Text->SetText(NSLOCTEXT("RoadMaker", "AimAtRoadLines", "Aim at a road"));
		Box->SetBrushColor(FLinearColor(0.6f, 0.08f, 0.05f, 0.85f));
	}
}

#if !WITH_EDITOR

namespace
{
	TWeakObjectPtr<URMRoadLinesHint> GRoadLinesHint;

	void EnsureHint(UFGBuildGunStatePaint* State)
	{
		if (GRoadLinesHint.IsValid() || !State)
		{
			return;
		}
		AFGBuildGun* Gun = State->GetBuildGun();
		APawn* Pawn = Gun ? Cast<APawn>(Gun->GetOwner()) : nullptr;
		APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
		if (!PC || !PC->IsLocalController())
		{
			return;
		}
		if (URMRoadLinesHint* Hint = CreateWidget<URMRoadLinesHint>(PC, URMRoadLinesHint::StaticClass()))
		{
			Hint->SetVisibility(ESlateVisibility::HitTestInvisible);
			Hint->AddToViewport(500);
			GRoadLinesHint = Hint;
		}
	}

	/** A customization was picked in the customizer: track Road Lines picks, drop them otherwise. */
	void OnCustomizationPicked(UFGBuildGunStatePaint* State, TSubclassOf<UFGFactoryCustomizationDescriptor> Desc)
	{
		if (Desc && Desc->IsChildOf(URMRoadLineDescriptor::StaticClass()))
		{
			TSubclassOf<URMRoadLineDescriptor> Option(Desc.Get());
			if (Option->GetDefaultObject<URMRoadLineDescriptor>()->mIsSideLineOption)
			{
				GActiveSides = Option;
			}
			else
			{
				GActiveLanes = Option;
			}
			EnsureHint(State);
			UE_LOG(LogRoadMaker, Display, TEXT("Road Lines: selected %s"), *Desc->GetName());
		}
		else if (Desc)
		{
			GActiveSides = nullptr;
			GActiveLanes = nullptr;
			// Road Lines options are material-type descriptors. If the paint gun still holds one as
			// its active material (e.g. a colour was picked after it), drop it so it never gets
			// applied to a building as a material.
			if (State)
			{
				TSubclassOf<UFGFactoryCustomizationDescriptor_Material> Material = State->GetActiveMaterialDesc();
				if (Material && Material->IsChildOf(URMRoadLineDescriptor::StaticClass()))
				{
					State->SetActiveMaterialDesc(nullptr);
				}
			}
		}
	}
}


namespace
{
	/**
	 * The customizer's own menu tiles never show a mod option's icon (they keep whatever the reused
	 * tile showed before). While the paint gun is out, find tiles labelled with a Road Lines option
	 * name and put that option's icon into the tile's icon image (plain texture or material).
	 */
	TMap<FString, TWeakObjectPtr<UTexture2D>> GIconsByName;
	bool GLoggedTileLayout = false;

	void BuildIconTable()
	{
		if (GIconsByName.Num() > 0)
		{
			return;
		}
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(URMRoadLineDescriptor::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract)
				&& *It != URMRoadLineDescriptor::StaticClass())
			{
				URMRoadLineDescriptor* D = It->GetDefaultObject<URMRoadLineDescriptor>();
				if (UTexture2D* Icon = D ? D->GetIcon() : nullptr)
				{
					GIconsByName.Add(UFGItemDescriptor::GetItemName(*It).ToString(), Icon);
				}
			}
		}
	}

	/** All widgets of a tile, including those inside nested user widgets (the icon is). */
	void CollectWidgets(UUserWidget* Root, TArray<UWidget*>& Out, int32 Depth = 0)
	{
		if (!Root || !Root->WidgetTree || Depth > 4)
		{
			return;
		}
		TArray<UWidget*> Widgets;
		Root->WidgetTree->GetAllWidgets(Widgets);
		for (UWidget* W : Widgets)
		{
			Out.Add(W);
			if (UUserWidget* Nested = Cast<UUserWidget>(W))
			{
				CollectWidgets(Nested, Out, Depth + 1);
			}
		}
	}

	/**
	 * Tiles we changed, so the game's own look can be put back when the menu reuses a tile for a
	 * colour, material or pattern (tiles are pooled across tabs).
	 */
	struct FPatchedTile
	{
		TWeakObjectPtr<UTextBlock> Label;
		TWeakObjectPtr<UImage> Icon;
		FSlateBrush OriginalBrush;
		TArray<TPair<TWeakObjectPtr<UTextBlock>, ESlateVisibility>> HiddenTexts;
	};
	TArray<FPatchedTile> GPatched;

	bool IsRoadLinesLabel(const UTextBlock* Label)
	{
		return Label && GIconsByName.Contains(Label->GetText().ToString().TrimStartAndEnd());
	}

	/** Puts the game's look back on tiles that no longer show a Road Lines option. */
	void RestoreReusedTiles()
	{
		for (int32 I = GPatched.Num() - 1; I >= 0; --I)
		{
			FPatchedTile& T = GPatched[I];
			if (T.Label.IsValid() && IsRoadLinesLabel(T.Label.Get()))
			{
				continue;
			}
			if (UImage* Image = T.Icon.Get())
			{
				Image->SetBrush(T.OriginalBrush);
			}
			for (const auto& Hidden : T.HiddenTexts)
			{
				if (UTextBlock* Text = Hidden.Key.Get())
				{
					Text->SetVisibility(Hidden.Value);
				}
			}
			GPatched.RemoveAtSwap(I);
		}
	}

	FPatchedTile* FindPatched(const UTextBlock* Label)
	{
		return GPatched.FindByPredicate([Label](const FPatchedTile& T) { return T.Label.Get() == Label; });
	}

	void PutIconInTile(UTextBlock* Label, UUserWidget* Tile, UTexture2D* Icon)
	{
		TArray<UWidget*> Widgets;
		CollectWidgets(Tile, Widgets);

		FPatchedTile* Patched = FindPatched(Label);
		for (UWidget* W : Widgets)
		{
			UImage* Image = Cast<UImage>(W);
			UUserWidget* Owner = W->GetTypedOuter<UUserWidget>();
			// The option picture is the image "mIcon" of the customizer icon widget. It is normally a
			// material that paints swatch colours; we draw the Road Lines picture in its place.
			if (!Image || Image->GetName() != TEXT("mIcon") || !Owner || !Owner->GetClass()->GetName().Contains(TEXT("CustomiserIcon")))
			{
				continue;
			}
			if (!Patched)
			{
				FPatchedTile New;
				New.Label = Label;
				New.Icon = Image;
				New.OriginalBrush = Image->GetBrush();
				// The "Edit" label left over from the colour tab: Road Lines options are not editable.
				for (UWidget* Other : Widgets)
				{
					UTextBlock* Text = Cast<UTextBlock>(Other);
					if (Text && Text->GetText().ToString().Equals(TEXT("Edit"), ESearchCase::IgnoreCase))
					{
						New.HiddenTexts.Emplace(Text, Text->GetVisibility());
						Text->SetVisibility(ESlateVisibility::Collapsed);
					}
				}
				Patched = &GPatched.Add_GetRef(New);
				if (!GLoggedTileLayout)
				{
					GLoggedTileLayout = true;
					UE_LOG(LogRoadMaker, Display, TEXT("Road Lines icons: drawing into %s.mIcon (was %s)"),
						*Owner->GetClass()->GetName(),
						New.OriginalBrush.GetResourceObject() ? *New.OriginalBrush.GetResourceObject()->GetName() : TEXT("none"));
				}
			}
			if (Image->GetBrush().GetResourceObject() != Icon)
			{
				FSlateBrush Brush = Patched->OriginalBrush;
				Brush.SetResourceObject(Icon);
				Brush.DrawAs = ESlateBrushDrawType::Image;
				Brush.TintColor = FSlateColor(FLinearColor::White);
				Image->SetBrush(Brush);
				Image->SetColorAndOpacity(FLinearColor::White);
			}
			return;
		}
	}

	TArray<FString> GetKeysOf(const TMap<FString, TWeakObjectPtr<UTexture2D>>& Map)
	{
		TArray<FString> Keys;
		Map.GetKeys(Keys);
		return Keys;
	}

	bool FixCustomizerIcons(float)
	{
		UWorld* World = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::Game && Context.World())
			{
				World = Context.World();
				break;
			}
		}
		// The customizer menu runs while the build gun is out (in its menu state, not the paint state).
		APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		AFGCharacterPlayer* Character = PC ? Cast<AFGCharacterPlayer>(PC->GetPawn()) : nullptr;
		AFGBuildGun* Gun = Character ? Character->GetBuildGun() : nullptr;
		if (!Gun || !Gun->IsEquipped())
		{
			return true;
		}
		BuildIconTable();
		RestoreReusedTiles();
		static bool bLoggedLabels = false;
		TArray<FString> SeenLabels;
		int32 Matches = 0;
		for (TObjectIterator<UTextBlock> It; It; ++It)
		{
			UTextBlock* Label = *It;
			if (!Label || Label->IsTemplate() || !Label->IsVisible())
			{
				continue;
			}
			const FString Text = Label->GetText().ToString();
			if (!bLoggedLabels && SeenLabels.Num() < 60 && !Text.IsEmpty())
			{
				SeenLabels.Add(FString::Printf(TEXT("%s<%s>"), *Text, *Label->GetClass()->GetName()));
			}
			const TWeakObjectPtr<UTexture2D>* Icon = GIconsByName.Find(Text.TrimStartAndEnd());
			if (Icon && Icon->IsValid())
			{
				++Matches;
				PutIconInTile(Label, Label->GetTypedOuter<UUserWidget>(), Icon->Get());
			}
		}
		// Once per session, when the Road Lines tab is open but no tile was matched, log what the
		// menu labels look like (helps if the game draws its labels differently).
		if (!bLoggedLabels && Matches == 0 && RMRoadLinePaint::IsAnyLabelVisible(SeenLabels))
		{
			bLoggedLabels = true;
			UE_LOG(LogRoadMaker, Display, TEXT("Road Lines icons: no tile matched. Labels: %s | Known: %s"),
				*FString::Join(SeenLabels, TEXT(", ")), *FString::Join(GetKeysOf(GIconsByName), TEXT(", ")));
		}
		return true;
	}
}

void RMRoadLinePaint::InstallHooks()
{
	// Road Lines tiles in the customizer menu: put our icons in, ten times a second while it is open.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&FixCustomizerIcons), 0.1f);

	SUBSCRIBE_METHOD_AFTER(UFGBuildGunStatePaint::SetActiveCustomization,
		[](UFGBuildGunStatePaint* State, TSubclassOf<UFGFactoryCustomizationDescriptor> Desc)
		{
			OnCustomizationPicked(State, Desc);
		});

	SUBSCRIBE_METHOD_AFTER(UFGBuildGunStatePaint::SetActiveRecipe,
		[](UFGBuildGunStatePaint* State, TSubclassOf<UFGCustomizationRecipe> Recipe)
		{
			if (Recipe)
			{
				OnCustomizationPicked(State, UFGCustomizationRecipe::GetCustomizationDescriptor(Recipe));
			}
		});

	SUBSCRIBE_METHOD_AFTER(UFGBuildGunStatePaint::ClearAllCustomizations, [](UFGBuildGunStatePaint* State)
	{
		GActiveSides = nullptr;
		GActiveLanes = nullptr;
	});

	SUBSCRIBE_METHOD_AFTER(UFGBuildGunStatePaint::RemoveCustomization,
		[](UFGBuildGunStatePaint* State, TSubclassOf<UFGFactoryCustomizationDescriptor> Desc)
		{
			if (Desc == GActiveSides.Get())
			{
				GActiveSides = nullptr;
			}
			if (Desc == GActiveLanes.Get())
			{
				GActiveLanes = nullptr;
			}
		});

	// Fire with a Road Lines option selected: apply it to the aimed road instead of painting.
	SUBSCRIBE_METHOD(AFGBuildGun::OnPrimaryFirePressed, [](auto& Scope, AFGBuildGun* Gun)
	{
		if (!IsAnyActive() || !Gun || !Cast<UFGBuildGunStatePaint>(Gun->GetCurrentState()))
		{
			return;
		}
		Scope.Cancel();
		APawn* Pawn = Cast<APawn>(Gun->GetOwner());
		ARMBuildableRoad* Road = TraceRoad(Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr);
		if (!Road || !Road->HasAuthority())
		{
			return; // the hint already says "Aim at a road"; clients in multiplayer: not yet
		}
		FRMMarkings Markings = Road->GetMarkings();
		URMRoadLineDescriptor::ApplyTo(GActiveSides, Markings);
		URMRoadLineDescriptor::ApplyTo(GActiveLanes, Markings);
		Road->SetMarkings(Markings);
	});
}

#else

void RMRoadLinePaint::InstallHooks() {}

#endif
