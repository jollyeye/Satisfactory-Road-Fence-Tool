// Gap Fill (1.4.0, asset Build_RoadFill): a slab filling the area between road edges, at the height of the roads around it. Made with the
// fill tool (ARMFillHologram): road / walking path edges and intersection curbs picked around an area, joined where
// they cross or by straight lines. Only the outline is saved; the mesh is rebuilt from it (like an intersection).
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableFill.generated.h"

UCLASS()
class ROADMAKER_API ARMBuildableFill : public ARMBuildableProcedural
{
	GENERATED_BODY()

public:
	ARMBuildableFill(const FObjectInitializer& ObjectInitializer);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void RebuildMesh() override;

	/** The top outline in actor space (closed: the last point joins the first) and the slab thickness. */
	void SetOutline(const TArray<FVector>& LocalOutline, float Height);

	/** Top outline in world space (closed). */
	void GetWorldOutline(TArray<FVector>& OutOutline) const;
	float GetSlabHeight() const { return mHeight; }

	/** Cheap value that changes whenever the shape or look changes (map layer). */
	uint32 GetShapeKey() const;

	/** Paint: the look materials read it (the cyberpunk surface its primary colour). */
	virtual void ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData) override;
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData) override;
	void ApplyPaint(const FFactoryCustomizationData& Data);

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mTopMaterial = nullptr;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mBodyMaterial = nullptr;

protected:
	UFUNCTION()
	void OnRep_Outline() { RebuildMesh(); }

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Outline)
	TArray<FVector> mOutline;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Outline)
	float mHeight = 100.f;
};
