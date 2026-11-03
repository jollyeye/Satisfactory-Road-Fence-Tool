#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "RMRoadLinePaint.generated.h"

class APlayerController;
class ARMBuildableRoad;
class UBorder;
class UTextBlock;

namespace RMRoadLinePaint
{
	/**
	 * Road Lines in the customizer: remembers the chosen Road Lines options when the customizer
	 * selects them, and while one is selected the customizer's fire button applies it to the aimed
	 * road instead of painting. A small on-screen line shows the choice, or "Aim at a road".
	 */
	void InstallHooks();

	bool IsAnyActive();
	FText GetActiveText();
	ARMBuildableRoad* TraceRoad(APlayerController* PC);
	bool IsPaintStateActive(APlayerController* PC);
}

/** "Road Lines: ..." / "Aim at a road" line while a Road Lines option is selected in the customizer. */
UCLASS()
class URMRoadLinesHint : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

protected:
	UPROPERTY(Transient)
	UBorder* Box = nullptr;

	UPROPERTY(Transient)
	UTextBlock* Text = nullptr;
};
