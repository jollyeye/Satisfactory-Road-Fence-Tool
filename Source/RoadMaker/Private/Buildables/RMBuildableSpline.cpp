#include "Buildables/RMBuildableSpline.h"
#include "Net/UnrealNetwork.h"

ARMBuildableSpline::ARMBuildableSpline(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ARMBuildableSpline::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableSpline, mSplinePoints);
}

void ARMBuildableSpline::SetSplinePoints(const TArray<FRMSplinePoint>& LocalPoints)
{
	mSplinePoints = LocalPoints;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableSpline::OnRep_SplinePoints()
{
	RebuildMesh();
}

void ARMBuildableSpline::GetLocalFrames(TArray<FRMFrame>& OutFrames, float Step) const
{
	RMGeometry::SampleSpline(mSplinePoints, Step, OutFrames);
}

void ARMBuildableSpline::GetWorldFrames(TArray<FRMFrame>& OutFrames, float Step) const
{
	GetLocalFrames(OutFrames, Step);
	const FTransform& T = GetActorTransform();
	for (FRMFrame& F : OutFrames)
	{
		F.Pos = T.TransformPosition(F.Pos);
		F.Fwd = T.TransformVectorNoScale(F.Fwd);
		F.Right = T.TransformVectorNoScale(F.Right);
		F.Up = T.TransformVectorNoScale(F.Up);
	}
}

float ARMBuildableSpline::GetLength() const
{
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, 100.f);
	return Frames.Num() > 0 ? Frames.Last().Dist : 0.f;
}
