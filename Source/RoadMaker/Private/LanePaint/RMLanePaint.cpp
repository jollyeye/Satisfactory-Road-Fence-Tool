#include "LanePaint/RMLanePaint.h"
#include "Buildables/RMBuildableRoad.h"
#include "Holograms/RMDisqualifiers.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

// ---------------------------------------------------------------------------------------------
// Settings and menu lifetime
// ---------------------------------------------------------------------------------------------

namespace
{
	FRMMarkings GSettings;
	TWeakObjectPtr<URMLanePaintMenu> GMenu;

	const FLinearColor ColAsphalt(0.03f, 0.03f, 0.035f);
	const FLinearColor ColLine(0.85f, 0.85f, 0.8f);
	const FLinearColor ColPanel(0.02f, 0.02f, 0.025f, 0.92f);
	const FLinearColor ColButton(0.12f, 0.12f, 0.13f);
	const FLinearColor ColSelected(0.95f, 0.5f, 0.1f); // FICSIT orange
}

FRMMarkings& RMLanePaint::Settings()
{
	return GSettings;
}

void RMLanePaint::ToggleMenu(APlayerController* PC)
{
	if (GMenu.IsValid())
	{
		CloseMenu();
		return;
	}
	if (!PC || !PC->IsLocalController())
	{
		return;
	}
	URMLanePaintMenu* Menu = CreateWidget<URMLanePaintMenu>(PC, URMLanePaintMenu::StaticClass());
	if (!Menu)
	{
		return;
	}
	Menu->SetIsFocusable(true);
	Menu->AddToViewport(1000);
	GMenu = Menu;

	FInputModeUIOnly Mode;
	Mode.SetWidgetToFocus(Menu->TakeWidget());
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PC->SetInputMode(Mode);
	PC->SetShowMouseCursor(true);
}

void RMLanePaint::CloseMenu()
{
	URMLanePaintMenu* Menu = GMenu.Get();
	GMenu.Reset();
	if (!Menu)
	{
		return;
	}
	APlayerController* PC = Menu->GetOwningPlayer();
	Menu->RemoveFromParent();
	if (PC)
	{
		PC->SetInputMode(FInputModeGameOnly());
		PC->SetShowMouseCursor(false);
	}
}

// ---------------------------------------------------------------------------------------------
// Hologram
// ---------------------------------------------------------------------------------------------

ARMLanePaintHologram::ARMLanePaintHologram()
{
	if (!RootComponent)
	{
		SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	}
}

void ARMLanePaintHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Switching tools or leaving build mode closes the menu.
	RMLanePaint::CloseMenu();
	Super::EndPlay(EndPlayReason);
}

void ARMLanePaintHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	SetActorLocation(hitResult.Location);
	mTargetRoad = Cast<ARMBuildableRoad>(hitResult.GetActor());
}

void ARMLanePaintHologram::CheckValidPlacement()
{
	// No Super: nothing is placed, so clearance and floor checks do not apply.
	if (!mTargetRoad.IsValid())
	{
		AddConstructDisqualifier(URMCD_AimAtRoad::StaticClass());
	}
}

bool ARMLanePaintHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	if (ARMBuildableRoad* Road = mTargetRoad.Get())
	{
		// The road replicates the change to other players. Only the host can change it, so this
		// works in single player and for the host; clients would need a server call (not yet).
		if (Road->HasAuthority())
		{
			Road->SetMarkings(RMLanePaint::Settings());
		}
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Menu widget, built in code (no Blueprint asset needed)
// ---------------------------------------------------------------------------------------------

namespace
{
	UWidget* Fixed(UWidgetTree* Tree, UWidget* Content, float Width, float Height = 0.f)
	{
		USizeBox* Box = Tree->ConstructWidget<USizeBox>();
		if (Width > 0.f)
		{
			Box->SetWidthOverride(Width);
		}
		if (Height > 0.f)
		{
			Box->SetHeightOverride(Height);
		}
		if (Content)
		{
			Box->AddChild(Content);
		}
		return Box;
	}

	UBorder* Solid(UWidgetTree* Tree, const FLinearColor& Color)
	{
		UBorder* B = Tree->ConstructWidget<UBorder>();
		B->SetBrushColor(Color);
		return B;
	}

	void AddToRow(UHorizontalBox* Row, UWidget* W, bool bFill)
	{
		UHorizontalBoxSlot* Slot = Row->AddChildToHorizontalBox(W);
		Slot->SetSize(FSlateChildSize(bFill ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic));
		Slot->SetVerticalAlignment(VAlign_Fill);
	}

	UTextBlock* Label(UWidgetTree* Tree, const FString& Text, int32 Size = 14)
	{
		UTextBlock* T = Tree->ConstructWidget<UTextBlock>();
		T->SetText(FText::FromString(Text));
		FSlateFontInfo Font = T->GetFont();
		Font.Size = Size;
		T->SetFont(Font);
		T->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		T->SetJustification(ETextJustify::Center);
		return T;
	}
}

TSharedRef<SWidget> URMLanePaintMenu::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	return Super::RebuildWidget();
}

UWidget* URMLanePaintMenu::MakePreview(int32 Lanes, bool bEdgeLines)
{
	// Top-down sketch of a road: fence margins, side lines, lanes split by dashed lines.
	UWidgetTree* Tree = WidgetTree;
	UBorder* Road = Solid(Tree, ColAsphalt);
	Road->SetPadding(FMargin(0.f));
	UHorizontalBox* Row = Tree->ConstructWidget<UHorizontalBox>();
	Road->SetContent(Row);

	auto Line = [&]() { return Fixed(Tree, Solid(Tree, ColLine), 3.f); };
	auto Gap = [&](float W) { return Fixed(Tree, Tree->ConstructWidget<USpacer>(), W); };
	auto Dashed = [&]()
	{
		UVerticalBox* Col = Tree->ConstructWidget<UVerticalBox>();
		for (int32 I = 0; I < 5; ++I)
		{
			UVerticalBoxSlot* Dash = Col->AddChildToVerticalBox(Solid(Tree, ColLine));
			Dash->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			UVerticalBoxSlot* Space = Col->AddChildToVerticalBox(Tree->ConstructWidget<USpacer>());
			FSlateChildSize S(ESlateSizeRule::Fill);
			S.Value = 1.5f;
			Space->SetSize(S);
		}
		return Fixed(Tree, Col, 3.f);
	};

	AddToRow(Row, Gap(8.f), false);
	AddToRow(Row, bEdgeLines ? Line() : Gap(3.f), false);
	for (int32 L = 0; L < Lanes; ++L)
	{
		if (L > 0)
		{
			AddToRow(Row, Dashed(), false);
		}
		AddToRow(Row, Tree->ConstructWidget<USpacer>(), true);
	}
	AddToRow(Row, bEdgeLines ? Line() : Gap(3.f), false);
	AddToRow(Row, Gap(8.f), false);

	return Fixed(Tree, Road, 96.f, 120.f);
}

UWidget* URMLanePaintMenu::MakeLaneButton(int32 Lanes)
{
	UWidgetTree* Tree = WidgetTree;
	UButton* Button = Tree->ConstructWidget<UButton>();
	Button->SetBackgroundColor(ColButton);

	UVerticalBox* Col = Tree->ConstructWidget<UVerticalBox>();
	Button->AddChild(Col);

	// The picture is rebuilt when side lines are toggled, so keep its parent.
	USizeBox* PreviewSlot = Tree->ConstructWidget<USizeBox>();
	PreviewSlot->AddChild(MakePreview(Lanes, GSettings.bEdgeLines));
	UVerticalBoxSlot* PS = Col->AddChildToVerticalBox(PreviewSlot);
	PS->SetHorizontalAlignment(HAlign_Center);
	PS->SetPadding(FMargin(6.f));
	mPreviewSlots.Add(PreviewSlot);

	UVerticalBoxSlot* TS = Col->AddChildToVerticalBox(Label(Tree, Lanes == 1 ? TEXT("1 lane") : FString::Printf(TEXT("%d lanes"), Lanes)));
	TS->SetHorizontalAlignment(HAlign_Center);
	TS->SetPadding(FMargin(4.f, 0.f, 4.f, 6.f));

	mLaneButtons.Add(Button);
	return Button;
}

void URMLanePaintMenu::BuildTree()
{
	UWidgetTree* Tree = WidgetTree;
	mLaneButtons.Reset();
	mPreviewSlots.Reset();

	// Full-screen invisible catcher (so right-click anywhere closes), with a centred panel.
	UBorder* Catcher = Solid(Tree, FLinearColor(0.f, 0.f, 0.f, 0.25f));
	Tree->RootWidget = Catcher;
	UVerticalBox* Screen = Tree->ConstructWidget<UVerticalBox>();
	Catcher->SetContent(Screen);
	UVerticalBoxSlot* TopFill = Screen->AddChildToVerticalBox(Tree->ConstructWidget<USpacer>());
	TopFill->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	UBorder* Panel = Solid(Tree, ColPanel);
	Panel->SetPadding(FMargin(16.f));
	UVerticalBoxSlot* PanelSlot = Screen->AddChildToVerticalBox(Panel);
	PanelSlot->SetHorizontalAlignment(HAlign_Center);

	UVerticalBoxSlot* BottomFill = Screen->AddChildToVerticalBox(Tree->ConstructWidget<USpacer>());
	BottomFill->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	UVerticalBox* Content = Tree->ConstructWidget<UVerticalBox>();
	Panel->SetContent(Content);

	UVerticalBoxSlot* Title = Content->AddChildToVerticalBox(Label(Tree, TEXT("Lane Paint"), 20));
	Title->SetPadding(FMargin(0.f, 0.f, 0.f, 4.f));
	UVerticalBoxSlot* Hint = Content->AddChildToVerticalBox(Label(Tree, TEXT("Pick a layout, then left-click roads to paint them"), 11));
	Hint->SetPadding(FMargin(0.f, 0.f, 0.f, 12.f));

	UHorizontalBox* Choices = Tree->ConstructWidget<UHorizontalBox>();
	Content->AddChildToVerticalBox(Choices);
	for (int32 Lanes = 1; Lanes <= 3; ++Lanes)
	{
		UHorizontalBoxSlot* S = Choices->AddChildToHorizontalBox(MakeLaneButton(Lanes));
		S->SetPadding(FMargin(6.f));
	}
	mLaneButtons[0]->OnClicked.AddDynamic(this, &URMLanePaintMenu::OnLanes1);
	mLaneButtons[1]->OnClicked.AddDynamic(this, &URMLanePaintMenu::OnLanes2);
	mLaneButtons[2]->OnClicked.AddDynamic(this, &URMLanePaintMenu::OnLanes3);

	UHorizontalBox* Bottom = Tree->ConstructWidget<UHorizontalBox>();
	UVerticalBoxSlot* BottomSlot = Content->AddChildToVerticalBox(Bottom);
	BottomSlot->SetPadding(FMargin(6.f, 12.f, 6.f, 0.f));

	mEdgeButton = Tree->ConstructWidget<UButton>();
	mEdgeText = Label(Tree, TEXT(""));
	mEdgeButton->AddChild(mEdgeText);
	mEdgeButton->OnClicked.AddDynamic(this, &URMLanePaintMenu::OnToggleEdges);
	UHorizontalBoxSlot* ES = Bottom->AddChildToHorizontalBox(mEdgeButton);
	ES->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	ES->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f));

	UButton* Close = Tree->ConstructWidget<UButton>();
	Close->SetBackgroundColor(ColButton);
	Close->AddChild(Label(Tree, TEXT("Done")));
	Close->OnClicked.AddDynamic(this, &URMLanePaintMenu::OnClose);
	Bottom->AddChildToHorizontalBox(Fixed(Tree, Close, 110.f));

	Refresh();
}

void URMLanePaintMenu::Refresh()
{
	for (int32 I = 0; I < mLaneButtons.Num(); ++I)
	{
		mLaneButtons[I]->SetBackgroundColor(GSettings.Lanes == I + 1 ? ColSelected : ColButton);
		if (mPreviewSlots.IsValidIndex(I))
		{
			mPreviewSlots[I]->ClearChildren();
			mPreviewSlots[I]->AddChild(MakePreview(I + 1, GSettings.bEdgeLines));
		}
	}
	if (mEdgeButton && mEdgeText)
	{
		mEdgeButton->SetBackgroundColor(GSettings.bEdgeLines ? ColSelected : ColButton);
		mEdgeText->SetText(FText::FromString(GSettings.bEdgeLines ? TEXT("Side lines: ON") : TEXT("Side lines: OFF")));
	}
}

void URMLanePaintMenu::SelectLanes(int32 Lanes)
{
	GSettings.Lanes = static_cast<uint8>(FMath::Clamp(Lanes, 1, 3));
	Refresh();
}

void URMLanePaintMenu::OnLanes1() { SelectLanes(1); }
void URMLanePaintMenu::OnLanes2() { SelectLanes(2); }
void URMLanePaintMenu::OnLanes3() { SelectLanes(3); }

void URMLanePaintMenu::OnToggleEdges()
{
	GSettings.bEdgeLines = !GSettings.bEdgeLines;
	Refresh();
}

void URMLanePaintMenu::OnClose()
{
	RMLanePaint::CloseMenu();
}

FReply URMLanePaintMenu::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Escape || Key == EKeys::Enter || Key == EKeys::Q)
	{
		RMLanePaint::CloseMenu();
		return FReply::Handled();
	}
	if (Key == EKeys::One || Key == EKeys::NumPadOne) { SelectLanes(1); return FReply::Handled(); }
	if (Key == EKeys::Two || Key == EKeys::NumPadTwo) { SelectLanes(2); return FReply::Handled(); }
	if (Key == EKeys::Three || Key == EKeys::NumPadThree) { SelectLanes(3); return FReply::Handled(); }
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply URMLanePaintMenu::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	// Right-click again closes the menu, like a toggle.
	if (InMouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		RMLanePaint::CloseMenu();
		return FReply::Handled();
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}
