// Junction slab joining 2..6 road ends. Only the road ends (a few vectors each) are saved;
// the variable mesh is rebuilt from them on load, so the save file stays tiny and robust.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableIntersection.generated.h"

UCLASS()
class ROADMAKER_API ARMBuildableIntersection : public ARMBuildableProcedural
{
	GENERATED_BODY()

public:
	ARMBuildableIntersection(const FObjectInitializer& ObjectInitializer);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void RebuildMesh() override;
	virtual bool DismantlesRiders() const override { return true; }

	/** Ends in actor-local space, plus the slab thickness taken from the connected roads. */
	void SetEnds(const TArray<FRMRoadEnd>& LocalEnds, float Height);

	float GetSlabHeight() const { return mHeight; }

	/** Saved ends converted to world space, used to treat those road ends as occupied. */
	void GetWorldEnds(TArray<FRMRoadEnd>& OutEnds) const;

	/** Curved curbs between neighbouring roads, in world space, at the top of the slab. */
	void GetWorldCurbs(TArray<TArray<FVector>>& OutCurbs) const;

	/** Top outline in world space (closed, last point joins the first); OutIsMouth[i]: edge i -> i+1 is a road mouth. */
	bool GetWorldOutline(TArray<FVector>& OutOutline, TArray<bool>& OutIsMouth) const;
	/** Cheap value that changes whenever the shape or look changes (map layer). */
	uint32 GetShapeKey() const;

	/** Side lines along the curbs (Road Lines > Sidelines in the customizer). */
	void SetSideLines(bool bOn);
	bool HasSideLines() const { return mSideLines; }

	/** Local-only look while the customizer hovers this intersection; unset = show the real state. */
	void SetPreviewSideLines(TOptional<bool> Preview);

	/** Customizer: side lines use the secondary colour, like on roads. */
	virtual void ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData) override;
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData) override;
	void ApplyMarkingColors(const FFactoryCustomizationData& Data);

	/** Same as roads: side lines sit this far in from the curb (cm). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	float mEdgeLineInset = 90.f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mTopMaterial = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mBodyMaterial = nullptr;

	/** Points per curb fillet. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	int32 mFilletSegments = 10;

protected:
	UFUNCTION()
	void OnRep_Ends();

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Ends)
	TArray<FRMRoadEnd> mEnds;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Ends)
	float mHeight = 200.f;

	/** Off by default, so intersections from older saves look as before. */
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Ends)
	bool mSideLines = false;

	TOptional<bool> mPreviewSideLines;

	/**
	 * Build animation: the surface spreads from the road mouths to the middle with a glowing front
	 * (instead of the rise). The real mesh (with its collision) stays, hidden; a temporary mesh
	 * without collision shows the part built so far, cut smoothly along the front.
	 */
	virtual void StartRiseAnimation() override;
	virtual float GetBuildAnimDuration() const override { return RevealDuration; }
	void StepReveal();
	void EndReveal();
	/** Distance (cm, seen from above) from P (actor space) to the nearest road mouth. */
	float DistanceToMouths(const FVector& P) const;

	bool mRevealing = false;
	FRMMesh mRevealTop, mRevealBody, mRevealLines;
	float mRevealMaxDist = 0.f;
	double mRevealStart = 0.0;
	FTimerHandle mRevealTimer;

	UPROPERTY(Transient)
	UProceduralMeshComponent* mRevealMesh = nullptr;

	static constexpr float RevealDuration = 2.5f;
	static constexpr float RevealGlowWidth = 150.f; // cm
};
