#include "Buildables/RMBuildableRoad.h"
#include "ProceduralMeshComponent.h"
#include "Math/RotationMatrix.h"

ARMBuildableRoad::ARMBuildableRoad(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
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
	RMGeometry::BuildRoad(Frames, GetRoadWidth(), mRoadHeight, mUVScale, Top, Body);
	Top.ApplyToSection(mMesh, 0, true);
	Body.ApplyToSection(mMesh, 1, true);
	ApplyMaterial(0, mTopMaterial);
	ApplyMaterial(1, mBodyMaterial);

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
