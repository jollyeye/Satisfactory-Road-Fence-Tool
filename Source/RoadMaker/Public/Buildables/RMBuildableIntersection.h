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

	/** Ends in actor-local space, plus the slab thickness taken from the connected roads. */
	void SetEnds(const TArray<FRMRoadEnd>& LocalEnds, float Height);

	float GetSlabHeight() const { return mHeight; }

	/** Saved ends converted to world space, used to treat those road ends as occupied. */
	void GetWorldEnds(TArray<FRMRoadEnd>& OutEnds) const;

	/** Curved curbs between neighbouring roads, in world space, at the top of the slab. */
	void GetWorldCurbs(TArray<TArray<FVector>>& OutCurbs) const;

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
};
