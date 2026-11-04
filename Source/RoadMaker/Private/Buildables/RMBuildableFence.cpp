#include "Buildables/RMBuildableFence.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Buildables/RMBuildableRoad.h"
#include "FGFactoryColoringTypes.h"

ARMBuildableFence::ARMBuildableFence(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	mMeshStep = 50.f;
	mLODDistance = 6000.f; // 60 m: fences are thin, details vanish sooner
}

void ARMBuildableFence::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableFence, mInwardSign);
}

void ARMBuildableFence::RebuildMesh()
{
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, mMeshStep);
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	if (Frames.Num() < 2)
	{
		return;
	}

	FRMMesh Main, Panel;
	BuildFenceMesh(mFenceType, Frames, mUVScale, mInwardSign, Main, Panel);

	Main.ApplyToSection(mMesh, 0, true);
	Panel.ApplyToSection(mMesh, 1, true);
	ApplyMaterial(0, mMainMaterial);
	ApplyMaterial(1, mPanelMaterial);

	// Far version: the same fence sampled every 4 m instead of every 50 cm.
	TArray<FRMFrame> FarFrames;
	GetLocalFrames(FarFrames, 400.f);
	if (FarFrames.Num() >= 2)
	{
		FRMMesh FarMain, FarPanel;
		BuildFenceMesh(mFenceType, FarFrames, mUVScale, mInwardSign, FarMain, FarPanel);
		ApplyFarSection(FarMain, 0, mMainMaterial);
		ApplyFarSection(FarPanel, 1, mPanelMaterial);
	}
	ApplyPaintColors(mCustomizationData);
}

void ARMBuildableFence::GetWorldSplinePoints(TArray<FRMSplinePoint>& Out) const
{
	const FTransform T = GetActorTransform();
	Out.Reset();
	for (const FRMSplinePoint& P : mSplinePoints)
	{
		FRMSplinePoint W = P;
		W.Location = T.TransformPosition(P.Location);
		W.Tangent = T.TransformVector(P.Tangent);
		Out.Add(W);
	}
}

void ARMBuildableFence::ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Implementation(customizationData);
	ApplyPaintColors(customizationData);
}

void ARMBuildableFence::ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Native(customizationData);
	ApplyPaintColors(customizationData);
}

void ARMBuildableFence::ApplyPaintColors(const FFactoryCustomizationData& Data)
{
	if (!mMesh || !mAllowColoring)
	{
		return;
	}
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	const FLinearColor P = Resolved.PrimaryColor;
	const FLinearColor S = Resolved.SecondaryColor;
	for (UProceduralMeshComponent* M : { mMesh, mFarMesh })
	{
		if (M)
		{
			M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
			M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
		}
	}
}

void ARMBuildableFence::BuildFenceMesh(ERMFenceType Type, TArray<FRMFrame> Frames, float UVScale, float InwardSign, FRMMesh& Main, FRMMesh& Panel)
{
	if (InwardSign < 0.f)
	{
		RMGeometry::MirrorFrames(Frames);
	}
	switch (Type)
	{
	case ERMFenceType::GuardRail:
		RMGeometry::BuildGuardRail(Frames, UVScale, Main);
		break;
	case ERMFenceType::JerseyBarrier:
		RMGeometry::BuildJerseyBarrier(Frames, UVScale, Main);
		break;
	case ERMFenceType::GridFence:
		RMGeometry::BuildGridFence(Frames, UVScale, Main, Panel);
		break;
	case ERMFenceType::GridFenceTall:
		RMGeometry::BuildGridFence(Frames, UVScale, Main, Panel, 2.f);
		break;
	case ERMFenceType::ConcreteWall:
		RMGeometry::BuildFenceWall(Frames, UVScale, Main, GetWallHeight(), 50.f);
		break;
	}
}
