// Sign gantry: two H-beam columns standing on upright connection cubes on the road's fence lines, and a
// beam across the road at least 8 m above it (H-beam with a shelf-beam end piece each side), resting
// on top of the columns (the game's own meshes, beams stretched to length). Signs snap to the beam's faces with the snap key. Only two numbers are saved (half span,
// beam height); the parts are rebuilt from them. An invisible box shape gives the collision.
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
	static constexpr float BeamDepth = 100.f;         // H-beam: 1 m tall ...
	static constexpr float BeamHalfThickness = 40.f;  // ... and 0.8 m wide
	static constexpr float ColumnHalfAlong = 50.f;    // column: 1 m along the road, 0.8 m across it
	static constexpr float ColumnHalfAcross = 40.f;
	static constexpr float SignSpacing = 200.f;       // sign snap points every 2 m
	static constexpr float FootHeight = 210.f;        // connection cube under each column, stood up (about 2.1 m)
	static constexpr float FootOverlap = 10.f;        // the column sinks 10 cm into the foot (its model is a little lower than its bounds)
	static constexpr float FootHalfAlong = 55.5f;
	static constexpr float FootHalfAcross = 55.5f;
	static constexpr float EndPiece = 100.f;          // the cross beam's two shelf-beam ends are 1 m each
	static constexpr float BeamOverhang = 40.f;       // the beam ends flush with the columns' outer faces

	/** Boxes around the columns and the beam in local space: collision, and the hologram's preview shape. */
	static void BuildGantryMesh(float HalfSpan, float BeamBottom, FRMMesh& Out);

	/** Recipe units to pay: one per 8 m of steel (both columns and the beam). */
	static int32 CostUnits(float HalfSpan, float BeamBottom);

	/**
	 * Where a sign goes when you aim at the world point Hit (with the face normal): flat on the face
	 * you aim at, on the beam (front or back, a point every 2 m along it, at the bottom edge, middle or
	 * top edge) or on a column (any of its four faces, a point every 2 m up). False if not on the gantry.
	 * OutDepth: distance from the face to the middle of the beam / column.
	 */
	bool GetSignFace(const FVector& Hit, const FVector& HitNormal, FVector& OutLocation, FVector& OutNormal, float* OutDepth = nullptr) const;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mMainMaterial = nullptr; // unused now (the game's H-beam material is used)

protected:
	/** The game's H-beam parts: two columns and the cross beam. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> mParts;

	UFUNCTION()
	void OnRep_Shape() { RebuildMesh(); }

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	float mHalfSpan = 400.f;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	float mBeamBottom = 800.f;
};
