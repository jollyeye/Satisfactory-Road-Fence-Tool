#include "UI/RMScreenHint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

namespace RMScreenHintState
{
	TWeakObjectPtr<URMScreenHint> GHint;
}

void RMScreenHint::Show(UWorld* World, const FText& InText, bool bWarning)
{
	if (IsRunningDedicatedServer() || !World)
	{
		return;
	}
	URMScreenHint* Hint = RMScreenHintState::GHint.Get();
	if (!Hint || Hint->GetWorld() != World)
	{
		Hide();
		APlayerController* PC = World->GetFirstPlayerController();
		if (!PC || !PC->IsLocalController())
		{
			return;
		}
		Hint = CreateWidget<URMScreenHint>(PC, URMScreenHint::StaticClass());
		if (!Hint)
		{
			return;
		}
		Hint->SetVisibility(ESlateVisibility::HitTestInvisible);
		Hint->AddToViewport(500);
		RMScreenHintState::GHint = Hint;
	}
	Hint->SetHint(InText, bWarning);
}

void RMScreenHint::Hide()
{
	if (URMScreenHint* Hint = RMScreenHintState::GHint.Get())
	{
		Hint->RemoveFromParent();
	}
	RMScreenHintState::GHint.Reset();
}

TSharedRef<SWidget> URMScreenHint::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		// Like the Road Lines hint: a dark box low in the middle of the screen.
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

void URMScreenHint::SetHint(const FText& InText, bool bWarning)
{
	if (Text && Box)
	{
		Text->SetText(InText);
		Box->SetBrushColor(bWarning ? FLinearColor(0.6f, 0.08f, 0.05f, 0.85f) : FLinearColor(0.02f, 0.02f, 0.025f, 0.75f));
	}
}
