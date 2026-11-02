// Fence along a spline. One C++ class; make three Blueprint children, one per mFenceType.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableSpline.h"
#include "RMBuildableFence.generated.h"

UENUM(BlueprintType)
enum class ERMFenceType : uint8
{
	GuardRail     UMETA(DisplayName = "Metal Guard Rail"),
	JerseyBarrier UMETA(DisplayName = "Cement Blocks"),
	GridFence     UMETA(DisplayName = "Metal Grid Fence"),
};

UCLASS()
class ROADMAKER_API ARMBuildableFence : public ARMBuildableSpline
{
	GENERATED_BODY()

public:
	ARMBuildableFence(const FObjectInitializer& ObjectInitializer);

	virtual void RebuildMesh() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** +1: the road is on the fence's right (+Right of its spline), -1: on its left. Set by the hologram. */
	void SetInwardSign(float InSign) { mInwardSign = InSign >= 0.f ? 1.f : -1.f; }

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Fence")
	ERMFenceType mFenceType = ERMFenceType::GuardRail;

	/** How far the fence sits in from the road edge, in cm (half the fence footprint is enough). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Fence")
	float mEdgeInset = 35.f;

	/** Section 0: steel for guard rail and grid posts, concrete for cement blocks. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mMainMaterial = nullptr;

	/** Section 1: masked grid material for the grid fence panel. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mPanelMaterial = nullptr;

protected:
	UFUNCTION()
	void OnRep_InwardSign() { RebuildMesh(); }

	/** Which side of the fence the road is on; the guard rail beam faces that way. */
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_InwardSign)
	float mInwardSign = 1.f;
};
