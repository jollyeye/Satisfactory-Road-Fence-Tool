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

void RMRoadLinePaint::InstallHooks()
{
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
