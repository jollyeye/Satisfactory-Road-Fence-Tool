#include "Buildables/RMBuildableRoad.h"
#include "ProceduralMeshComponent.h"
#include "Math/RotationMatrix.h"
#include "Net/UnrealNetwork.h"
#include "FGFactoryColoringTypes.h"
#include "Buildables/RMSwatchGroupRoads.h"

ARMBuildableRoad::ARMBuildableRoad(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Paintable with the customizer, with its own default-swatch group ("Roads").
	mAllowColoring = true;
	mSwatchGroup = URMSwatchGroup_Roads::StaticClass();

	mStartAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("StartAnchor"));
	mStartAnchor->SetupAttachment(mMesh);
	mStartAnchor->ComponentTags.Add(TEXT("RMRoadAnchor"));

	mEndAnchor = CreateDefaultSubobject<USceneComponent>(TEXT("EndAnchor"));
	mEndAnchor->SetupAttachment(mMesh);
	mEndAnchor->ComponentTags.Add(TEXT("RMRoadAnchor"));
}

void ARMBuildableRoad::RebuildMesh()
{
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, mMeshStep);
	if (Frames.Num() < 2)
	{
		mMesh->ClearAllMeshSections();
		return;
	}

	FRMMesh Top, Body;
	RMGeometry::BuildRoad(Frames, GetRoadWidth(), mRoadHeight, mUVScale, mMarkings, mEdgeLineInset, Top, Body);
	Top.ApplyToSection(mMesh, 0, true);
	Body.ApplyToSection(mMesh, 1, true);
	ApplyMaterial(0, mTopMaterial);
	ApplyMaterial(1, mBodyMaterial);
	ApplyMarkingColors(mCustomizationData);

	// Anchors sit on the top-centre of each end, facing outward.
	const FRMFrame& First = Frames[0];
	const FRMFrame& Last = Frames.Last();
	mStartAnchor->SetRelativeLocationAndRotation(First.Pos, FRotationMatrix::MakeFromXZ(-First.Fwd, FVector::UpVector).Rotator());
	mEndAnchor->SetRelativeLocationAndRotation(Last.Pos, FRotationMatrix::MakeFromXZ(Last.Fwd, FVector::UpVector).Rotator());
}

void ARMBuildableRoad::GetEndWorld(int32 EndIndex, FVector& OutLocation, FVector& OutOutward) const
{
	TArray<FRMFrame> Frames;
	GetWorldFrames(Frames, 200.f);
	if (Frames.Num() < 2)
	{
		OutLocation = GetActorLocation();
		OutOutward = GetActorForwardVector();
		return;
	}
	const FRMFrame& F = EndIndex == 0 ? Frames[0] : Frames.Last();
	OutLocation = F.Pos;
	OutOutward = EndIndex == 0 ? -F.Fwd : F.Fwd;
}

void ARMBuildableRoad::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableRoad, mMarkings);
}

void ARMBuildableRoad::SetMarkings(const FRMMarkings& InMarkings)
{
	FRMMarkings Clean = InMarkings;
	Clean.Lanes = FMath::Clamp<uint8>(Clean.Lanes, 1, 3);
	if (Clean == mMarkings)
	{
		return;
	}
	mMarkings = Clean;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableRoad::ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Implementation(customizationData);
	ApplyMarkingColors(customizationData);
}

void ARMBuildableRoad::ApplyMarkingColors(const FFactoryCustomizationData& Data)
{
	if (!mMesh)
	{
		return;
	}
	// Resolves swatch / colour slot / custom colours into the actual primary and secondary colour.
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	const FLinearColor P = Resolved.PrimaryColor;
	const FLinearColor S = Resolved.SecondaryColor;
	mMesh->SetCustomPrimitiveDataVector4(PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
	mMesh->SetCustomPrimitiveDataVector4(SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
}

void ARMBuildableRoad::ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Native(customizationData);
	ApplyMarkingColors(customizationData);
}
