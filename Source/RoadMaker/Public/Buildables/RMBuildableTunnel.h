// Tunnel (1.5.0): a rounded concrete arch over a road, made with the tunnel tool (ARMTunnelHologram): click the two
// sides (a road or walking path edge, or the top of a fence wall), set the arc (up to a semicircle). The arch springs
// from the two sides along the whole stretch where they run side by side; a shorter tunnel = cut the road or fence.
// Saved: the two spring lines (actor space, one point per cross-section) and the arc; the mesh is rebuilt from them.
// Local space: the actor sits at the middle of the first cross-section.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableTunnel.generated.h"

UCLASS()
class ROADMAKER_API ARMBuildableTunnel : public ARMBuildableProcedural
{
	GENERATED_BODY()

public:
	ARMBuildableTunnel(const FObjectInitializer& ObjectInitializer);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void RebuildMesh() override;
	virtual void ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData) override;
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData) override;

	/** Left / Right: the middle of the shell's foot on each side, one point per cross-section (actor space). */
	void SetShape(const TArray<FVector>& Left, const TArray<FVector>& Right, float ArcDeg);
	float GetArcDeg() const { return mArcDeg; }

	/** Shell thickness (cm), pieces around the arch, arc limits and step (degrees). */
	static constexpr float Thickness = 50.f;
	static constexpr int32 Segments = 18;
	static constexpr int32 FarSegments = 9;
	static constexpr float MinArc = 10.f;
	static constexpr float MaxArc = 180.f;
	static constexpr float ArcStep = 5.f;

	/**
	 * The shell (inner and outer faces, the two feet and the cut ends) for these spring lines, in their space. The
	 * concrete blocks material lays its blocks by UV1 (metres along / around, the walking path's blocks turned 90).
	 */
	static void BuildTunnelMesh(const TArray<FVector>& Left, const TArray<FVector>& Right, float ArcDeg, int32 NumSegments, FRMMesh& Out);

	/** Highest point of the arch above the middle of its chord (cm), for a chord of Width. */
	static float RiseFor(float Width, float ArcDeg);
	/** The arc whose rise over a chord of Width is Rise (degrees, not rounded or clamped). */
	static float ArcForRise(float Width, float Rise);

	/** Recipe units: one per 16 m^2 of shell (the recipe is what a 4 x 4 m piece of the game's concrete wall costs). */
	static int32 CostUnits(const TArray<FVector>& Left, const TArray<FVector>& Right, float ArcDeg);

	/** The tunnel's direction (level) at the cross-section nearest a world point: ceiling lights hang along it. */
	FVector GetDirectionAt(const FVector& World) const;

	/** Shell material (the walking path's concrete blocks). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mShellMaterial = nullptr;

protected:
	void ApplyPaint(const FFactoryCustomizationData& Data);

	UFUNCTION()
	void OnRep_Shape() { RebuildMesh(); }

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	TArray<FVector> mLeft;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	TArray<FVector> mRight;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	float mArcDeg = 180.f;
};
