#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "RMRoadLinePaint.generated.h"

class APlayerController;
class URMRoadLineDescriptor;
class ARMBuildableRoad;
class ARMBuildableIntersection;
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

	/** Road or intersection under the crosshair (whichever Road Lines can go on), else null. */
	AActor* TraceTarget(APlayerController* PC);

	/** True if the active options do something on this target (intersections take side lines only). */
	bool CanApplyTo(const AActor* Target);

	/** Server side: writes the options into a road (side lines and/or lanes) or an intersection (side lines). */
	void ApplyToTarget(AActor* Target, TSubclassOf<class URMRoadLineDescriptor> Sides, TSubclassOf<class URMRoadLineDescriptor> Lanes);
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
	virtual void NativeDestruct() override;

	/** Shows the selected Road Lines on the hovered road/intersection (local only), like the customizer's material preview. */
	void UpdatePreview(AActor* Target);

protected:
	UPROPERTY(Transient)
	UBorder* Box = nullptr;

	UPROPERTY(Transient)
	UTextBlock* Text = nullptr;

	TWeakObjectPtr<AActor> mPreviewTarget;
};
