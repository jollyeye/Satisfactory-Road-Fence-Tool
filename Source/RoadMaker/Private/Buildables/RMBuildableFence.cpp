#include "Buildables/RMBuildableFence.h"
#include "ProceduralMeshComponent.h"

ARMBuildableFence::ARMBuildableFence(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	mMeshStep = 50.f;
}

void ARMBuildableFence::RebuildMesh()
{
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, mMeshStep);
	mMesh->ClearAllMeshSections();
	if (Frames.Num() < 2)
	{
		return;
	}

	FRMMesh Main, Panel;
	switch (mFenceType)
	{
	case ERMFenceType::GuardRail:
		RMGeometry::BuildGuardRail(Frames, mUVScale, Main);
		break;
	case ERMFenceType::JerseyBarrier:
		RMGeometry::BuildJerseyBarrier(Frames, mUVScale, Main);
		break;
	case ERMFenceType::GridFence:
		RMGeometry::BuildGridFence(Frames, mUVScale, Main, Panel);
		break;
	}

	Main.ApplyToSection(mMesh, 0, true);
	Panel.ApplyToSection(mMesh, 1, true);
	ApplyMaterial(0, mMainMaterial);
	ApplyMaterial(1, mPanelMaterial);
}
