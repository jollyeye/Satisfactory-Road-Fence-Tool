#include "Buildables/RMBuildableIntersection.h"
#include "ProceduralMeshComponent.h"
#include "RoadMakerModule.h"
#include "Net/UnrealNetwork.h"

ARMBuildableIntersection::ARMBuildableIntersection(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void ARMBuildableIntersection::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableIntersection, mEnds);
	DOREPLIFETIME(ARMBuildableIntersection, mHeight);
}

void ARMBuildableIntersection::SetEnds(const TArray<FRMRoadEnd>& LocalEnds, float Height)
{
	mEnds = LocalEnds;
	mHeight = Height;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

void ARMBuildableIntersection::OnRep_Ends()
{
	RebuildMesh();
}

void ARMBuildableIntersection::RebuildMesh()
{
	mMesh->ClearAllMeshSections();
	FRMMesh Top, Body;
	if (!RMGeometry::BuildIntersection(mEnds, mHeight, mUVScale, mFilletSegments, Top, Body))
	{
		UE_LOG(LogRoadMaker, Warning, TEXT("%s: could not rebuild intersection from %d saved ends"), *GetName(), mEnds.Num());
		return;
	}
	Top.ApplyToSection(mMesh, 0, true);
	Body.ApplyToSection(mMesh, 1, true);
	ApplyMaterial(0, mTopMaterial);
	ApplyMaterial(1, mBodyMaterial);
}

void ARMBuildableIntersection::GetWorldEnds(TArray<FRMRoadEnd>& OutEnds) const
{
	OutEnds.Reset();
	const FTransform& T = GetActorTransform();
	for (const FRMRoadEnd& E : mEnds)
	{
		FRMRoadEnd W = E;
		W.Center = T.TransformPosition(E.Center);
		W.Inward = T.TransformVectorNoScale(E.Inward);
		OutEnds.Add(W);
	}
}
