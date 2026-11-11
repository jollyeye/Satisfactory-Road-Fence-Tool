// Sign gantry: two steel columns standing on the road on the fence lines, and a beam across the road
// at least 8 m above it, wide enough for the game's signs (they snap to the beam's faces with the snap
// key). Only two numbers are saved (half span, beam height); the mesh is rebuilt from them.
// Local space: X along the road, Y across it, Z up from the road surface at the gantry's centre.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableGantry.generated.h"

UCLASS()
class ROADMAKER_API ARMBuildableGantry : public ARMBuildableProcedural
{
	GENERATED_BODY()

public:
	ARMBuildableGantry(const FObjectInitializer& ObjectInitializer);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void RebuildMesh() override;

	/** Columns at +-HalfSpan across the road; BeamBottom = height of the beam's lower edge above the road (cm). */
	void SetShape(float HalfSpan, float BeamBottom);
	float GetHalfSpan() const { return mHalfSpan; }
	float GetBeamBottom() const { return mBeamBottom; }

	static constexpr float MinClearance = 800.f;   // beam at least 8 m above the road
	static constexpr float MaxClearance = 2400.f;
	static constexpr float HeightStep = 200.f;     // the mouse sets the height in 2 m steps
	static constexpr float ColumnInset = 35.f;     // columns stand on the road fence line
	static constexpr float BeamDepth = 150.f;
	static constexpr float BeamHalfThickness = 30.f;

	/** Steel mesh of a gantry in local space (also used by the hologram). */
	static void BuildGantryMesh(float HalfSpan, float BeamBottom, FRMMesh& Out);

	/** Recipe units to pay: one per 8 m of steel (both columns and the beam). */
	static int32 CostUnits(float HalfSpan, float BeamBottom);

	/**
	 * Where a sign goes on the beam when you aim at the world point Hit (with the face normal): flat
	 * on the front or back face, free along the beam, at the bottom edge, the middle or the top edge.
	 * False if the point is not on the beam.
	 */
	bool GetSignFace(const FVector& Hit, const FVector& HitNormal, FVector& OutLocation, FVector& OutNormal) const;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mMainMaterial = nullptr;

protected:
	UFUNCTION()
	void OnRep_Shape() { RebuildMesh(); }

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	float mHalfSpan = 400.f;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	float mBeamBottom = 800.f;
};
