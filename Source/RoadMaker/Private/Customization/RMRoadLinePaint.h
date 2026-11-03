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

	/** True if the labels include the "Road Lines" tab name (customizer open on or near our tab). */
	inline bool IsAnyLabelVisible(const TArray<FString>& Labels)
	{
		for (const FString& L : Labels)
		{
			if (L.StartsWith(TEXT("Road Lines<")) || L.StartsWith(TEXT("Sidelines<")))
			{
				return true;
			}
		}
		return false;
	}
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
