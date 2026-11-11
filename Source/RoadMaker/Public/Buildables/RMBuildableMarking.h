// Lane marking painted on a road: an arrow or a single digit, flat geometry (no textures or decals)
// lying on the surface, with the top of the arrow / digit pointing the way the piece faces. Saved:
// the variant (which arrow / which digit) and the lane width it was sized for; the mesh is rebuilt.
// The colour is the road's line colour at the time it was built (a copy of the road's paint, so it
// can be repainted on its own); the cyberpunk look glows like the road's lines do.
// Local space: X = direction of travel, Y = right, Z = up; the origin is the middle of the marking on the road surface.
#pragma once

#include "CoreMinimal.h"
#include "Buildables/RMBuildableProcedural.h"
#include "RMBuildableMarking.generated.h"

UENUM()
enum class ERMMarkingKind : uint8
{
	Arrow,
	Digit,
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
	/**
	 * The colour it is painted with when built (the road's line colour), and the colour the game's paint
	 * data resolves to at that moment: when the paint data later resolves to something else, the piece
	 * was repainted and takes that colour instead.
	 */
	void SetPlacementColour(const FLinearColor& Colour, const FLinearColor& DataPrimary);
	uint8 GetVariant() const { return mVariant; }
	float GetLaneWidth() const { return mLaneWidth; }

	/** Arrow: 0 straight, 1 left, 2 right, 3 straight or left, 4 straight or right, 5 left or right. Digit: 0 to 9. */
	static int32 VariantCount(ERMMarkingKind Kind) { return Kind == ERMMarkingKind::Arrow ? 6 : 10; }

	/** Builds the flat mesh (top face only, a hair above the road) in the marking's local space. */
	static void BuildMarkingMesh(ERMMarkingKind Kind, uint8 Variant, float LaneWidth, FRMMesh& Out);

	/** 1 for the small markings, 2 for the medium ones (double width and length). */
	UPROPERTY(EditDefaultsOnly, Category = "Road Maker")
	float mSizeMultiplier = 1.f;

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

	UFUNCTION()
	void OnRep_Colour() { ApplyMarkingColors(mCustomizationData); }

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Colour)
	FLinearColor mColour = FLinearColor::White;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Colour)
	FLinearColor mDataPrimary = FLinearColor::White;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Colour)
	bool mHasColour = false;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	uint8 mVariant = 0;

	UPROPERTY(SaveGame, ReplicatedUsing = OnRep_Shape)
	float mLaneWidth = 200.f;
};
