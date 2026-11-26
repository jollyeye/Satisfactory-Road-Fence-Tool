// Boom barrier (1.6.0): a post on a road's fence line and a striped arm across the road, hinged at the post's top.
// Three modes, cycled with E (a window comes later): Automatic (the arm lifts while a vehicle is near, never blocks),
// Always Open, Always Closed (the arm is down and stops vehicles: it blocks them, not players). Placed like the Sign
// Gantry: click the side for the post, then the other side (the arm reaches it).
// Automated Machines hooks (AM_ functions, found by name: no dependency): vehicle waiting / open conditions, open and
// automatic actions.
// Local space: X along the road, Y across it towards the arm's end, Z up from the road surface at the post.
// Temporary meshes (boxes): the post (metal), the arm (red and white stripes).
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableBarrier.generated.h"

UENUM()
enum class ERMBarrierMode : uint8
{
	Automatic,
	Open,
	Closed
};

UCLASS()
class ROADMAKER_API ARMBuildableBarrier : public ARMBuildableProcedural
{
	GENERATED_BODY()

public:
	ARMBuildableBarrier(const FObjectInitializer& ObjectInitializer);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void RebuildMesh() override;

	// E: next mode.
	virtual bool IsUseable_Implementation() const override { return true; }
	virtual void OnUse_Implementation(class AFGCharacterPlayer* byCharacter, const FUseState& state) override;
	virtual FText GetLookAtDecription_Implementation(class AFGCharacterPlayer* byCharacter, const FUseState& state) const override;

	void SetArmLength(float Length);
	float GetArmLength() const { return mArmLength; }
	ERMBarrierMode GetMode() const { return mMode; }
	/** Server: the mode (saved, sent to everyone). */
	void SetMode(ERMBarrierMode Mode);
	/** Whether the arm should be up now. */
	bool ShouldBeOpen() const { return mMode == ERMBarrierMode::Open || (mMode == ERMBarrierMode::Automatic && mVehicleNear); }
	static FText ModeName(ERMBarrierMode Mode);

	/** The post and the arm (closed), local space; the hologram shows the same. */
	static void BuildPostMesh(FRMMesh& Out);
	static void BuildArmMesh(float Length, FRMMesh& OutWhite, FRMMesh& OutRed);

	static constexpr float PostHalf = 20.f;      // post 40 x 40 cm ...
	static constexpr float PostHeight = 110.f;   // ... 1.1 m tall
	static constexpr float HingeHeight = 100.f;  // the arm's hinge, on the post's face towards the road
	static constexpr float ArmHalf = 6.f;        // arm 12 x 12 cm
	static constexpr float StripeLength = 50.f;
	static constexpr float OpenAngle = 85.f;
	static constexpr float MinArm = 200.f;
	static constexpr float MaxArm = 2000.f;
	static constexpr float ColumnInset = 35.f;  // the post stands on the fence line

	// --- Automated Machines (found by name; keep the names: rules store them) ---
	UFUNCTION() bool AM_Condition_Vehicle_Waiting() const { return mVehicleNear; }
	UFUNCTION() bool AM_Condition_Open() const { return ShouldBeOpen(); }
	UFUNCTION() void AM_Action_Open(bool On) { SetMode(On ? ERMBarrierMode::Open : ERMBarrierMode::Closed); }
	UFUNCTION() void AM_Action_Automatic() { SetMode(ERMBarrierMode::Automatic); }
	UFUNCTION() FText AM_Info_Vehicle_Waiting() const;
	UFUNCTION() FText AM_Info_Open() const;
	UFUNCTION() FText AM_Info_Automatic() const;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mPostMaterial = nullptr;

protected:
	/** Server, 4 times a second: a vehicle within the barrier's reach (either side of the arm). */
	void DetectVehicles();
	/** Arm towards its target angle; the vehicle block follows the mode. */
	void UpdateArm(bool bSnap);
	void StepArm();

	UFUNCTION()
	void OnRep_State() { UpdateArm(false); }
	UFUNCTION()
	void OnRep_Shape() { RebuildMesh(); }

	UPROPERTY(VisibleAnywhere, Category = "Road Maker")
	UProceduralMeshComponent* mArmMesh;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	float mArmLength = 600.f;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_State)
	ERMBarrierMode mMode = ERMBarrierMode::Automatic;

	UPROPERTY(ReplicatedUsing = OnRep_State)
	bool mVehicleNear = false;

	float mArmAngle = 0.f;
	FTimerHandle mDetectTimer;
	FTimerHandle mArmTimer;
};
