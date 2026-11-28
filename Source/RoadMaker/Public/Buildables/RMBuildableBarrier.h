// Boom barrier (1.6.0): a post on a road's fence line and a hologram arm across the road, sent out from the hinge block
// on the post's side. Three modes, set in its window (E: URMBarrierWindow): Automatic (the arm draws back into the post
// while a vehicle is near), Always open (drawn back), Always closed (out). The light band under the post's cap shows the
// mode: green open, orange automatic, red closed.
// The arm is a hologram: nothing collides with it (players, creatures, vehicles) and the vehicle paths stay as they are.
// Always closed, the barrier turns off the autopilot of self-driving vehicles coming towards the arm (never a vehicle
// with a player at the wheel); when it opens again (or leaves always closed, or is dismantled) it turns the autopilot of
// those vehicles back on, the ones still nearby.
// Placed like the Sign Gantry: click the side for the post, then the other side (the arm reaches it).
// Automated Machines hooks (AM_ functions, found by name: no dependency): vehicle waiting / open conditions, the mode
// action.
// Local space: X along the road, Y across it towards the arm's end, Z up from the road surface at the post.
// Materials: steel (base plate, bolts, cap, hinge, vent: the grid fence's posts), the body (painted plastic, primary
// colour), the light band (emissive, colour per mode) and the arm (translucent red and white hologram).
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableBarrier.generated.h"

// Saved by value (the barrier and Automated Machines rules): new entries go at the end. Automated Machines shows the
// entry names (split into words), so they must read well as they are.
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


	void SetArmLength(float Length);
	float GetArmLength() const { return mArmLength; }
	ERMBarrierMode GetMode() const { return mMode; }
	/** Server: the mode (saved, sent to everyone). */
	void SetMode(ERMBarrierMode Mode);
	/** Whether the arm should be up now. */
	bool ShouldBeOpen() const { return mMode == ERMBarrierMode::Open || (mMode == ERMBarrierMode::Automatic && mVehicleNear); }
	static FText ModeName(ERMBarrierMode Mode);

	/** The post, local space: steel parts, the painted body, the light band. The hologram shows the same. */
	static void BuildPostMesh(FRMMesh& OutSteel, FRMMesh& OutBody, FRMMesh& OutLight);
	/** The arm, arm space (origin on the hinge block's face, along +Y): Length is from the post's centre line. */
	static void BuildArmMesh(float Length, FRMMesh& Out);
	/** The light band's colour for a mode (linear, before the material's glow). */
	static FLinearColor ModeColor(ERMBarrierMode Mode);

	static constexpr float PostHalf = 20.f;      // the arm's length is measured from here (the post's old face)
	static constexpr float BodyHalf = 22.f;      // body 44 x 44 cm ...
	static constexpr float PlateHalf = 32.f;     // ... on a 64 x 64 cm steel plate
	static constexpr float PlateHeight = 4.f;
	static constexpr float BodyTop = 102.f;      // body from the plate to here, then the light band ...
	static constexpr float LightTop = 108.f;     // ... and the cap
	static constexpr float PostHeight = 116.f;
	static constexpr float HingeHeight = 88.f;   // the arm's centre
	static constexpr float HingeFace = BodyHalf + 10.f; // the hinge block's face, where the arm comes out
	static constexpr float ArmHalfHeight = 6.f;  // arm 12 cm tall ...
	static constexpr float ArmHalfThickness = 3.f; // ... 6 cm thick
	static constexpr float StripeLength = 50.f;
	static constexpr float MinArm = 200.f;
	static constexpr float MaxArm = 2000.f;
	static constexpr float ColumnInset = 35.f;  // the post stands on the fence line
	/** Custom primitive data index of the light band's colour (after the paint's 24..31). */
	static constexpr int32 LightColorIndex = 32;

	// --- Automated Machines (found by name; keep the names: rules store them) ---
	UFUNCTION() bool AM_Condition_Vehicle_Waiting() const { return mVehicleNear; }
	UFUNCTION() bool AM_Condition_Open() const { return ShouldBeOpen(); }
	/** One action for the mode: Automated Machines shows the enum's entries as a dropdown (Automatic, Open, Closed). */
	UFUNCTION() void AM_Action_Mode(ERMBarrierMode Mode) { SetMode(Mode); }
	UFUNCTION() FText AM_Info_Vehicle_Waiting() const;
	UFUNCTION() FText AM_Info_Open() const;
	UFUNCTION() FText AM_Info_Mode() const;

	/** Base plate, bolts, cap, hinge block and vent (the grid fence's posts). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mPostMaterial = nullptr;
	/** The body: plastic in the paint's primary colour. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mBodyMaterial = nullptr;
	/** The light band: glows in the colour at LightColorIndex. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mLightMaterial = nullptr;
	/** The arm: translucent red and white hologram. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mHoloMaterial = nullptr;

	virtual void ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData) override;
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData) override;

protected:
	/**
	 * Server, 4 times a second: a vehicle within the barrier's reach (either side of the arm); always closed, the
	 * autopilot of self-driving vehicles coming towards the arm is turned off (and they are remembered).
	 */
	void DetectVehicles();
	/** Server: autopilot back on for the vehicles this barrier stopped, the ones still nearby and without a driver. */
	void ReleaseVehicles();
	/** Arm towards its target (out or drawn back); the light band follows the mode. */
	void UpdateArm(bool bSnap);
	void StepArm();
	void ApplyArm();
	void ApplyPaint(const FFactoryCustomizationData& Data);

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

	/** Server: the vehicle identifiers whose autopilot this barrier turned off (saved: they are let go after a load). */
	UPROPERTY(SaveGame)
	TArray<AActor*> mStoppedVehicles;

	/** 1 = the arm all the way out, 0 = drawn back into the hinge block. */
	float mArmOut = 1.f;
	FTimerHandle mDetectTimer;
	FTimerHandle mArmTimer;

	static constexpr float DetectInterval = 0.1f;     // a vehicle within reach
	static constexpr float IdleDetectInterval = 0.5f; // none
	static constexpr float MaxDrawDistance = 25000.f;
	bool mDetectFast = false;
	static constexpr float ApproachReach = 3000.f; // vehicles looked at up to 30 m (centre) before the arm ...
	static constexpr float StopGap = 150.f;        // ... and stopped with their front about 1.5 m before it
	static constexpr float ReleaseReach = 5000.f;  // and back on for the stopped ones within 50 m
};
