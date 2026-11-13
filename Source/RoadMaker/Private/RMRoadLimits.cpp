#include "RMRoadLimits.h"
#include "RoadMakerModule.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Misc/ConfigCacheIni.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Spacer.h"
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

	TWeakObjectPtr<URMRoadLimitsHint> GLimitsHint;

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
	if (URMRoadLimitsHint* Hint = GLimitsHint.Get())
	{
		Hint->Refresh(true);
	}
	return true;
}

void RMRoadLimits::ShowHint(APlayerController* PC)
{
	if (!PC || !PC->IsLocalController())
	{
		return;
	}
	URMRoadLimitsHint* Hint = GLimitsHint.Get();
	if (!Hint)
	{
		Hint = CreateWidget<URMRoadLimitsHint>(PC, URMRoadLimitsHint::StaticClass());
		if (!Hint)
		{
			return;
		}
		Hint->SetVisibility(ESlateVisibility::HitTestInvisible);
		Hint->AddToViewport(500);
		GLimitsHint = Hint;
	}
	Hint->Refresh(false);
}

void RMRoadLimits::HideHint()
{
	if (URMRoadLimitsHint* Hint = GLimitsHint.Get())
	{
		Hint->RemoveFromParent();
	}
	GLimitsHint.Reset();
	GAltDownTime = -1.0;
}

TSharedRef<SWidget> URMRoadLimitsHint::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		// Bottom centre, under the crosshair area, like the game's own build hints.
		UVerticalBox* Screen = WidgetTree->ConstructWidget<UVerticalBox>();
		WidgetTree->RootWidget = Screen;
		UVerticalBoxSlot* Fill = Screen->AddChildToVerticalBox(WidgetTree->ConstructWidget<USpacer>());
		Fill->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		mNotice = WidgetTree->ConstructWidget<UTextBlock>();
		FSlateFontInfo NoticeFont = mNotice->GetFont();
		NoticeFont.Size = 22;
		mNotice->SetFont(NoticeFont);
		mNotice->SetShadowOffset(FVector2D(1.f, 1.f));
		mNotice->SetShadowColorAndOpacity(FLinearColor(0.f, 0.f, 0.f, 0.8f));
		UVerticalBoxSlot* NoticeSlot = Screen->AddChildToVerticalBox(mNotice);
		NoticeSlot->SetHorizontalAlignment(HAlign_Center);
		NoticeSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 10.f));

		UBorder* Box = WidgetTree->ConstructWidget<UBorder>();
		Box->SetBrushColor(FLinearColor(0.02f, 0.02f, 0.025f, 0.75f));
		Box->SetPadding(FMargin(12.f, 6.f));
		UVerticalBoxSlot* BoxSlot = Screen->AddChildToVerticalBox(Box);
		BoxSlot->SetHorizontalAlignment(HAlign_Center);
		BoxSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 140.f));

		mText = WidgetTree->ConstructWidget<UTextBlock>();
		FSlateFontInfo Font = mText->GetFont();
		Font.Size = 16;
		mText->SetFont(Font);
		Box->SetContent(mText);
		Refresh(false);
	}
	return Super::RebuildWidget();
}

void URMRoadLimitsHint::Refresh(bool bNotice)
{
	const bool bOff = RMRoadLimits::AreOff();
	const FLinearColor Orange(1.f, 0.55f, 0.12f);
	if (mText)
	{
		mText->SetText(bOff
			? NSLOCTEXT("RoadMaker", "LimitsHintOff", "[Alt]  Road limits: OFF  (up to 1 km, any turn or slope)")
			: NSLOCTEXT("RoadMaker", "LimitsHintOn", "[Alt]  Road limits: on"));
		mText->SetColorAndOpacity(FSlateColor(bOff ? Orange : FLinearColor::White));
	}
	if (bNotice)
	{
		mNoticeLeft = 2.f;
	}
	if (mNotice)
	{
		mNotice->SetText(bOff ? NSLOCTEXT("RoadMaker", "LimitsNoticeOff", "Road limits OFF") : NSLOCTEXT("RoadMaker", "LimitsNoticeOn", "Road limits ON"));
		mNotice->SetColorAndOpacity(FSlateColor(bOff ? Orange : FLinearColor::White));
		mNotice->SetVisibility(mNoticeLeft > 0.f ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Hidden);
	}
}

void URMRoadLimitsHint::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (mNoticeLeft > 0.f)
	{
		mNoticeLeft -= InDeltaTime;
		if (mNoticeLeft <= 0.f && mNotice)
		{
			mNotice->SetVisibility(ESlateVisibility::Hidden);
		}
	}
}
