#include "UI/RMBarrierWindow.h"
#include "Buildables/RMBuildableBarrier.h"
#include "Net/RMRemoteCallObject.h"
#include "UI/FGWindow.h"
#include "RoadMakerModule.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Border.h"
#include "Components/Spacer.h"
#include "Styling/SlateBrush.h"
#include "Styling/CoreStyle.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UnrealType.h"

namespace RMBarrierUI
{
	const TCHAR* FramePath = TEXT("/Game/FactoryGame/Interface/UI/Widget_Window_DarkMode.Widget_Window_DarkMode_C");
	// Colours sampled from the game's windows: dark panels, FICSIT orange for the chosen one.
	const FLinearColor Panel(0.035f, 0.037f, 0.042f, 1.f);
	const FLinearColor Button(0.09f, 0.095f, 0.105f, 1.f);
	const FLinearColor ButtonHover(0.16f, 0.165f, 0.18f, 1.f);
	const FLinearColor Orange(0.96f, 0.39f, 0.07f, 1.f);
	const FLinearColor Text(0.9f, 0.9f, 0.88f, 1.f);
	const FLinearColor Dim(0.55f, 0.56f, 0.58f, 1.f);

	FSlateBrush Flat(const FLinearColor& Color)
	{
		FSlateBrush B;
		B.DrawAs = ESlateBrushDrawType::Box;
		B.TintColor = FSlateColor(Color);
		B.Margin = FMargin(0.f);
		return B;
	}

	void StyleButton(UButton* B, const FLinearColor& Normal, const FLinearColor& Hover)
	{
		FButtonStyle S = B->GetStyle();
		S.Normal = Flat(Normal);
		S.Hovered = Flat(Hover);
		S.Pressed = Flat(Hover);
		S.Disabled = Flat(Normal);
		S.NormalPadding = FMargin(0.f);
		S.PressedPadding = FMargin(0.f);
		B->SetStyle(S);
	}

	UTextBlock* MakeText(UWidgetTree* Tree, const FText& Value, int32 Size, const FLinearColor& Color, bool bBold = false)
	{
		UTextBlock* T = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		T->SetText(Value);
		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(bBold ? "Bold" : "Regular", Size);
		T->SetFont(Font);
		T->SetColorAndOpacity(FSlateColor(Color));
		return T;
	}

	/** Calls a Blueprint function with no parameters by name, if it exists. */
	void CallByName(UObject* Object, const TCHAR* Name, void* Params = nullptr)
	{
		if (UFunction* F = Object ? Object->FindFunction(FName(Name)) : nullptr)
		{
			Object->ProcessEvent(F, Params);
		}
	}
}

URMBarrierWindow::URMBarrierWindow(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Like the game's building windows: the mouse is free, the player stands still, centred.
	mUseKeyboard = true;
	mUseMouse = true;
	mDesiredHorizontalAlignment = HAlign_Center;
	mDesiredVerticalAlignment = VAlign_Center;
}

bool URMBarrierWindow::HasAutomation()
{
	return FindObject<UClass>(nullptr, TEXT("/Script/AutomatedMachines.AMBlueprintLibrary")) != nullptr;
}

bool URMBarrierWindow::OpenAutomationWindow(AActor* Building)
{
	// Automated Machines' MODDING.md: called by name, so Road Maker does not depend on it.
	UClass* Library = FindObject<UClass>(nullptr, TEXT("/Script/AutomatedMachines.AMBlueprintLibrary"));
	UFunction* Function = Library ? Library->FindFunctionByName(TEXT("OpenAutomationWindow")) : nullptr;
	if (!Function || !Building)
	{
		return false;
	}
	struct { AActor* Building; bool ReturnValue; } Params{ Building, false };
	Library->GetDefaultObject()->ProcessEvent(Function, &Params);
	return Params.ReturnValue;
}

ARMBuildableBarrier* URMBarrierWindow::GetBarrier() const
{
	return Cast<ARMBuildableBarrier>(mInteractObject.Get());
}

void URMBarrierWindow::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (!WidgetTree || WidgetTree->RootWidget)
	{
		return;
	}
	UClass* FrameClass = LoadClass<UUserWidget>(nullptr, RMBarrierUI::FramePath);
	// The page: the three modes, the state, the Automation button.
	UBorder* Page = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	Page->SetBrush(RMBarrierUI::Flat(RMBarrierUI::Panel));
	Page->SetPadding(FMargin(24.f, 20.f));
	USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	Size->SetWidthOverride(460.f);
	Size->AddChild(Page);
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Page->SetContent(Column);

	Column->AddChildToVerticalBox(RMBarrierUI::MakeText(WidgetTree, NSLOCTEXT("RoadMaker", "BarrierWindowMode", "MODE"), 14, RMBarrierUI::Dim, true));
	const FText Names[3] = { ARMBuildableBarrier::ModeName(ERMBarrierMode::Automatic), ARMBuildableBarrier::ModeName(ERMBarrierMode::Open),
		ARMBuildableBarrier::ModeName(ERMBarrierMode::Closed) };
	for (int32 I = 0; I < 3; ++I)
	{
		UButton* B = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		UTextBlock* Label = RMBarrierUI::MakeText(WidgetTree, Names[I], 18, RMBarrierUI::Text, true);
		B->AddChild(Label);
		if (UVerticalBoxSlot* S = Column->AddChildToVerticalBox(B))
		{
			S->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
			S->SetHorizontalAlignment(HAlign_Fill);
		}
		mModeButtons.Add(B);
		mModeLabels.Add(Label);
	}
	mModeButtons[0]->OnClicked.AddDynamic(this, &URMBarrierWindow::OnAutomaticClicked);
	mModeButtons[1]->OnClicked.AddDynamic(this, &URMBarrierWindow::OnOpenClicked);
	mModeButtons[2]->OnClicked.AddDynamic(this, &URMBarrierWindow::OnClosedClicked);

	mStatus = RMBarrierUI::MakeText(WidgetTree, FText::GetEmpty(), 14, RMBarrierUI::Dim);
	if (UVerticalBoxSlot* S = Column->AddChildToVerticalBox(mStatus))
	{
		S->SetPadding(FMargin(0.f, 18.f, 0.f, 0.f));
	}
	UTextBlock* Help = RMBarrierUI::MakeText(WidgetTree, NSLOCTEXT("RoadMaker", "BarrierWindowHelp",
		"Automatic: the arm draws back while a vehicle is near. Always closed: self-driving vehicles coming towards it stop, and drive on when it opens. The arm is a hologram: nothing collides with it."), 12, RMBarrierUI::Dim);
	Help->SetAutoWrapText(false);
	Help->SetWrapTextAt(410.f);
	if (UVerticalBoxSlot* S = Column->AddChildToVerticalBox(Help))
	{
		S->SetPadding(FMargin(0.f, 8.f, 0.f, 0.f));
	}
	if (HasAutomation())
	{
		mAutomationButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		RMBarrierUI::StyleButton(mAutomationButton, RMBarrierUI::Button, RMBarrierUI::ButtonHover);
		mAutomationButton->AddChild(RMBarrierUI::MakeText(WidgetTree, NSLOCTEXT("RoadMaker", "BarrierWindowAutomation", "Automation"), 16, RMBarrierUI::Orange, true));
		mAutomationButton->OnClicked.AddDynamic(this, &URMBarrierWindow::OnAutomationClicked);
		if (UVerticalBoxSlot* S = Column->AddChildToVerticalBox(mAutomationButton))
		{
			S->SetPadding(FMargin(0.f, 18.f, 0.f, 0.f));
		}
	}

	if (FrameClass)
	{
		// The game's frame as our root, the page in its body.
		mFrame = WidgetTree->ConstructWidget<UUserWidget>(FrameClass);
		WidgetTree->RootWidget = mFrame;
		mFrame->SetContentForSlot(TEXT("WindowBody"), Size);
	}
	else
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("Barrier window: the game's window frame was not found; plain page"));
		WidgetTree->RootWidget = Size;
	}
}

void URMBarrierWindow::NativeConstruct()
{
	Super::NativeConstruct();
	if (mFrame && !mFrameSetUp)
	{
		mFrameSetUp = true;
		// The title tab: one entry in the frame's tab list (a struct with a text and a texture), then its buttons.
		if (FArrayProperty* Tabs = FindFProperty<FArrayProperty>(mFrame->GetClass(), TEXT("mTabs")))
		{
			FScriptArrayHelper Array(Tabs, Tabs->ContainerPtrToValuePtr<void>(mFrame));
			Array.EmptyValues();
			const int32 Index = Array.AddValue();
			if (FStructProperty* Inner = CastField<FStructProperty>(Tabs->Inner))
			{
				for (TFieldIterator<FProperty> It(Inner->Struct); It; ++It)
				{
					if (FTextProperty* TextProp = CastField<FTextProperty>(*It))
					{
						TextProp->SetPropertyValue_InContainer(Array.GetRawPtr(Index), NSLOCTEXT("RoadMakerAssets", "BoomBarrier.Name", "Boom Barrier"));
						break;
					}
				}
			}
			RMBarrierUI::CallByName(mFrame, TEXT("InitTabs"));
		}
		// No inventory panel.
		struct { bool Show; bool Animate; } Inv{ false, false };
		RMBarrierUI::CallByName(mFrame, TEXT("SetInventoryVisibility"), &Inv);
		// The frame's X: the first button inside its close button widget closes us.
		if (UFGWindow* Window = Cast<UFGWindow>(mFrame))
		{
			if (UUserWidget* Close = Window->GetCloseButton())
			{
				UButton* Found = nullptr;
				if (Close->WidgetTree)
				{
					Close->WidgetTree->ForEachWidget([&Found](UWidget* W) { if (!Found) { Found = Cast<UButton>(W); } });
				}
				if (Found)
				{
					Found->OnClicked.AddUniqueDynamic(this, &URMBarrierWindow::OnCloseClicked);
				}
			}
		}
	}
	for (UButton* B : mModeButtons)
	{
		RMBarrierUI::StyleButton(B, RMBarrierUI::Button, RMBarrierUI::ButtonHover);
	}
	Refresh();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(mRefreshTimer, FTimerDelegate::CreateWeakLambda(this, [this]() { Refresh(); }), 0.2f, true);
	}
}

void URMBarrierWindow::NativeDestruct()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(mRefreshTimer);
	}
	Super::NativeDestruct();
}

void URMBarrierWindow::Refresh()
{
	ARMBuildableBarrier* Barrier = GetBarrier();
	if (!Barrier)
	{
		return;
	}
	const int32 Current = int32(Barrier->GetMode());
	for (int32 I = 0; I < mModeButtons.Num(); ++I)
	{
		const bool bOn = I == Current;
		RMBarrierUI::StyleButton(mModeButtons[I], bOn ? RMBarrierUI::Orange : RMBarrierUI::Button, bOn ? RMBarrierUI::Orange : RMBarrierUI::ButtonHover);
		mModeLabels[I]->SetColorAndOpacity(FSlateColor(bOn ? FLinearColor(0.02f, 0.02f, 0.02f, 1.f) : RMBarrierUI::Text));
	}
	if (mStatus)
	{
		mStatus->SetText(FText::Format(NSLOCTEXT("RoadMaker", "BarrierWindowStatus", "Arm: {0}    Vehicle near: {1}"),
			Barrier->ShouldBeOpen() ? NSLOCTEXT("RoadMaker", "BarrierArmUp", "drawn back") : NSLOCTEXT("RoadMaker", "BarrierArmDown", "across"),
			Barrier->AM_Condition_Vehicle_Waiting() ? NSLOCTEXT("RoadMaker", "BarrierYes", "yes") : NSLOCTEXT("RoadMaker", "BarrierNo", "no")));
	}
}

void URMBarrierWindow::SetMode(uint8 Mode)
{
	ARMBuildableBarrier* Barrier = GetBarrier();
	if (!Barrier)
	{
		return;
	}
	if (Barrier->HasAuthority())
	{
		Barrier->SetMode(ERMBarrierMode(Mode));
	}
	else if (URMRemoteCallObject* RCO = URMRemoteCallObject::Get(GetOwningPlayer()))
	{
		RCO->ServerSetBarrierMode(Barrier, Mode);
	}
	Refresh();
}

void URMBarrierWindow::OnAutomaticClicked() { SetMode(uint8(ERMBarrierMode::Automatic)); }
void URMBarrierWindow::OnOpenClicked() { SetMode(uint8(ERMBarrierMode::Open)); }
void URMBarrierWindow::OnClosedClicked() { SetMode(uint8(ERMBarrierMode::Closed)); }

void URMBarrierWindow::OnAutomationClicked()
{
	if (ARMBuildableBarrier* Barrier = GetBarrier())
	{
		if (OpenAutomationWindow(Barrier))
		{
			OnEscapePressed(); // the Automation window takes over
		}
	}
}

void URMBarrierWindow::OnCloseClicked()
{
	OnEscapePressed();
}
