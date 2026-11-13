// Road limits toggle (Alt while holding a road): in any road build mode, off = no turn or slope
// limits, up to 1 km long, the build gun reaching 1 km (what the Long Curve build mode used to be).
// The choice is per player, remembered between sessions (GameUserSettings.ini). While a road
// hologram is held a hint shows the key and the state; toggling shows a short notice.
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "RMRoadLimits.generated.h"

class APlayerController;
class UTextBlock;

namespace RMRoadLimits
{
	/** True when the limits are off (this player's choice, remembered). */
	ROADMAKER_API bool AreOff();

	/**
	 * Every frame while a road hologram is held by this (local) player: Alt pressed and released on
	 * its own (not Alt+Tab) toggles the limits. True if it toggled.
	 */
	ROADMAKER_API bool HandleInput(APlayerController* PC);

	/** The "[Alt] Road limits" hint, while a road hologram is held. */
	ROADMAKER_API void ShowHint(APlayerController* PC);
	ROADMAKER_API void HideHint();
}

/** Bottom-centre hint: "[Alt]  Road limits: on / off", and a short notice after toggling. */
UCLASS()
class ROADMAKER_API URMRoadLimitsHint : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** Updates the text to the current state; bNotice shows the bigger notice line for a moment. */
	void Refresh(bool bNotice);

protected:
	UPROPERTY(Transient)
	UTextBlock* mText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* mNotice = nullptr;

	float mNoticeLeft = 0.f;
};
