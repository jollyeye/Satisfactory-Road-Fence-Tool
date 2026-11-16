// Blueprint Road (Road Mode, 1.4.0): the hidden centre line of a Road Mode placement, as wide as the blueprint
// ("Blueprint Road (4 wide)"...). Laid down with every Road Mode placement, at the blueprint's floor, part of its
// blueprint proxy. Invisible and without collision, except while the local player holds the dismantle tool or draws
// in Road Mode: then a wireframe, drawn over everything. Dismantling it dismantles the whole placement.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableSpline.h"
#include "RMBuildableBlueprintRoad.generated.h"

UCLASS()
class ROADMAKER_API ARMBuildableBlueprintRoad : public ARMBuildableSpline
{
	GENERATED_BODY()

public:
	ARMBuildableBlueprintRoad(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void RebuildMesh() override;

	// Dismantle: shown as "Blueprint Road (N wide)", takes the whole placement along.
	virtual FText GetDismantleDisplayName_Implementation(AFGCharacterPlayer* byCharacter) const override;
	virtual void GetChildDismantleActors_Implementation(TArray<AActor*>& out_ChildDismantleActors) const override;
	virtual void GetDismantleRefund_Implementation(TArray<FInventoryStack>& out_refund, bool noBuildCostEnabled) const override {}

	/** Width in cm (the blueprint's width across its arrow). Set before it finishes spawning. */
	void SetWidth(float WidthCm) { mWidthCm = WidthCm; }
	float GetWidth() const { return mWidthCm; }
	/** Width in foundations (4, 5, 6...). */
	int32 GetWidthUnits() const { return FMath::Max(1, FMath::RoundToInt(mWidthCm / RM::FoundationSize)); }

	/** The wireframe: both edges, the centre line and a line across every 8 m, lying on the frames (any space). */
	static void BuildWire(const TArray<FRMFrame>& Frames, float Width, FRMMesh& Out);
	/** The wireframe material (unlit, drawn over everything, coloured by vertex colour: M_RM_Overlay) and its colours. */
	static UMaterialInterface* WireMaterial();
	static TArray<FLinearColor> WireColors(const FRMMesh& Wire);

	/** Shows / hides every Blueprint Road for the local player (the module's ticker: dismantle tool or Road Mode in hand). */
	static void InstallVisibility();

protected:
	virtual bool ShowsBuildAnimation() const override { return false; }
	virtual bool UsesReveal() const override { return false; }
	virtual int32 GetChunkCount() const override { return 1; }

	void SetShown(bool bShown);

	UPROPERTY(SaveGame, Replicated)
	float mWidthCm = 3200.f;

	bool mShown = true;
};
