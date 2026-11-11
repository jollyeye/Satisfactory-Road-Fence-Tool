// Lane marking painted on a road: an arrow or a single digit, flat geometry (no textures or decals)
// lying on the surface (cut into slices that follow the road, see FRMMarkingDrape; a flat copy far away), with the top of the arrow / digit pointing the way the piece faces. Saved:
// the variant (which arrow / which digit) and the lane width it was sized for; the mesh is rebuilt.
// The colour is the road's line colour at the time it was built (a copy of the road's paint, so it
// can be repainted on its own); the cyberpunk look glows like the road's lines do.
// Local space: X = direction of travel, Y = right, Z = up; the origin is the middle of the marking on the road surface.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableMarking.generated.h"

class ARMBuildableRoad;
class UFGRecipe;

UENUM()
enum class ERMMarkingKind : uint8
{
	Arrow,
	Digit,
};

/**
 * The road surface under a marking, sampled along its length (local space of the marking): the mesh is
 * cut into slices at these samples and each slice lies on the road, so markings follow slopes, banking
 * and curves. Sample K is at X = Start + K * Step along the marking.
 */
struct ROADMAKER_API FRMMarkingDrape
{
	float Start = 0.f;
	float Step = 50.f;
	TArray<FVector> Pos;   // the lane's centre line on the road surface
	TArray<FVector> Right; // the marking's right (Y) on the road
	TArray<FVector> Up;    // the road surface normal

	bool IsValid() const { return Pos.Num() >= 2 && Right.Num() == Pos.Num() && Up.Num() == Pos.Num() && Step > 1.f; }
	/** A point of the marking's flat layout (X along, Y across, Z above the surface) on the road. */
	FVector Map(float X, float Y, float Z, FVector* OutNormal = nullptr) const;
};

UCLASS()
class ROADMAKER_API ARMBuildableMarking : public ARMBuildableProcedural
{
	GENERATED_BODY()

public:
	ARMBuildableMarking(const FObjectInitializer& ObjectInitializer);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void RebuildMesh() override;

	virtual void ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData) override;
	virtual void ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData) override;

	/** Which arrow / digit, and the width it is drawn at (cm; the lane width times the size of the entry). */
	void SetShape(uint8 Variant, float LaneWidth);
	uint8 GetVariant() const { return mVariant; }
	float GetLaneWidth() const { return mLaneWidth; }

	/** Arrow: 0 straight, 1 left, 2 right, 3 straight or left, 4 straight or right, 5 left or right. Digit: 0 to 9. */
	static int32 VariantCount(ERMMarkingKind Kind) { return Kind == ERMMarkingKind::Arrow ? 6 : 10; }

	/**
	 * Builds the mesh (top face only, a hair above the road) in the marking's local space: flat, or with
	 * Drape cut into slices lying on the road.
	 */
	static void BuildMarkingMesh(ERMMarkingKind Kind, uint8 Variant, float LaneWidth, FRMMesh& Out, const FRMMarkingDrape* Drape = nullptr);

	/** Half the length of a marking drawn for this width (cm). */
	static float HalfLength(ERMMarkingKind Kind, float LaneWidth);

	/** Markings grow with their lane up to this lane width (1.5 times the 2 m lane of the smallest ones). */
	static constexpr float MaxLaneWidth = 300.f;

	/** Detailed (draped) up to this distance, then the flat copy, gone beyond MaxDrawDistance. */
	static constexpr float DrapedDistance = 5000.f;
	static constexpr float MaxDrawDistance = 15000.f;

	/** Size of the entry (1; kept for the hologram). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mSizeMultiplier = 1.f;

	/**
	 * Everything about a marking on Road where the aim hits it: transform, the width it is drawn at (lane width,
	 * capped, times SizeMultiplier) and the road surface under it.
	 */
	static bool PlanOn(const ARMBuildableRoad* Road, const FVector& Aim, const FVector& LookDir, ERMMarkingKind Kind, float SizeMultiplier,
		FTransform& OutTransform, float& OutWidth, FRMMarkingDrape& OutDrape);

	/** Stores the road surface under it (set before it is spawned). */
	void SetDrape(const FRMMarkingDrape& Drape);

	/**
	 * Server: a road is replaced by a reshaped one (Adjust Curvature). The markings on From are painted again
	 * on To at the same share of its length, the same side and direction, in the nearest lane, with their own
	 * paint and look, and the old ones go. Returns how many moved.
	 */
	static int32 MoveMarkings(ARMBuildableRoad* From, ARMBuildableRoad* To);

	/** The road a point on its surface belongs to (nearest road frame), else null. */
	static ARMBuildableRoad* FindRoadAt(UWorld* World, const FVector& Location);

	/**
	 * Where a marking goes when the aim hits Road at Aim: the middle of the nearest lane, at that spot along the
	 * road, facing the way LookDir runs along it. OutLaneWidth is the width of that lane (cm).
	 */
	static bool FindPlacement(const ARMBuildableRoad* Road, const FVector& Aim, const FVector& LookDir, FTransform& OutTransform, float& OutLaneWidth);

	/** The marking material for a look (0 asphalt, 1 concrete, 2 cyberpunk). */
	UMaterialInterface* GetPaintMaterial(uint8 Look) const;

	/**
	 * Server: paints a marking on Road (the customizer's Road Lines options do this). Takes the road's look
	 * and line colour. Returns null if there is no valid spot.
	 */
	static ARMBuildableMarking* SpawnOn(TSubclassOf<ARMBuildableMarking> Class, TSubclassOf<UFGRecipe> Recipe, ARMBuildableRoad* Road,
		uint8 Variant, float SizeMultiplier, const FVector& Aim, const FVector& LookDir,
		const struct FFactoryCustomizationData* Paint = nullptr, int32 Look = -1);

	/** Hair above the road surface so the paint does not fight the road's own surface. */
	static constexpr float Lift = 1.5f;

	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	ERMMarkingKind mKind = ERMMarkingKind::Arrow;

	/** Paint material (looks asphalt and concrete). The cyberpunk look is the second entry of mLookTopMaterials. */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker|Look")
	UMaterialInterface* mMarkingMaterial = nullptr;

protected:
	void ApplyMarkingColors(const FFactoryCustomizationData& Data);

	UFUNCTION()
	void OnRep_Shape() { RebuildMesh(); }

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	uint8 mVariant = 0;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	float mLaneWidth = 200.f;

	// The road surface under it (FRMMarkingDrape); empty = flat (markings from before draping).
	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	float mDrapeStart = 0.f;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	float mDrapeStep = 50.f;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	TArray<FVector> mDrapePos;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	TArray<FVector> mDrapeRight;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	TArray<FVector> mDrapeUp;
};
