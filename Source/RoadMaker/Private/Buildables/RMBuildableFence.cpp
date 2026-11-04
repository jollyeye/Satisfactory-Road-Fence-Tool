#include "Buildables/RMBuildableFence.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Buildables/RMBuildableRoad.h"
#include "FGFactoryColoringTypes.h"
#include "HAL/PlatformTime.h"

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
	BuildFenceMesh(mFenceType, Frames, mUVScale, mInwardSign, Main, Panel, GetWallLayout());

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
		BuildFenceMesh(mFenceType, FarFrames, mUVScale, mInwardSign, FarMain, FarPanel, GetWallLayout());
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
			// The game's own way of passing paint (colours, finish, pattern) to its materials: used by
			// the game's wall material on the fence wall. Road Maker's materials read 24 / 28 below.
			Resolved.ApplyToMeshComponent(M);
			M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
			M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
		}
	}
}

namespace
{
	/** Shape of a fence wall being swapped for another material, kept a moment for the new wall. */
	struct FPendingWallSwap
	{
		FTransform Transform;
		TArray<FRMSplinePoint> Points;
		float InwardSign = 1.f;
		int32 CostMultiplier = 1;
		double Time = 0.0;
	};
	TArray<FPendingWallSwap> GPendingWallSwaps;
}

FRMWallTexLayout ARMBuildableFence::GetWallLayout() const
{
	FRMWallTexLayout L;
	L.UCentre = mWallUCentre;
	L.UPerMetre = mWallUPerMetre;
	L.VTop = mWallVTop;
	L.VPerMetre = mWallVPerMetre;
	L.bMirror = mWallMirror;
	return L;
}

void ARMBuildableFence::CopyShapeTo(ARMBuildableFence* Other) const
{
	if (!Other || Other == this)
	{
		return;
	}
	Other->SetActorTransform(GetActorTransform());
	Other->SetInwardSign(mInwardSign);
	Other->SetCostMultiplier(mCostMultiplier);
	Other->SetSplinePoints(mSplinePoints);
	Other->RebuildMesh();
	Other->ForceNetUpdate();
}

void ARMBuildableFence::PreUpgrade_Implementation()
{
	Super::PreUpgrade_Implementation();
	if (!IsWall())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	GPendingWallSwaps.RemoveAll([Now](const FPendingWallSwap& S) { return Now - S.Time > 5.0; });
	FPendingWallSwap Swap;
	Swap.Transform = GetActorTransform();
	Swap.Points = mSplinePoints;
	Swap.InwardSign = mInwardSign;
	Swap.CostMultiplier = mCostMultiplier;
	Swap.Time = Now;
	GPendingWallSwaps.Add(Swap);
}

void ARMBuildableFence::Upgrade_Implementation(AActor* newActor)
{
	Super::Upgrade_Implementation(newActor);
	if (ARMBuildableFence* Other = Cast<ARMBuildableFence>(newActor))
	{
		CopyShapeTo(Other);
	}
}

void ARMBuildableFence::BeginPlay()
{
	// A fence wall built by a material swap may arrive without a shape: take the swapped wall's.
	if (HasAuthority() && IsWall() && mSplinePoints.Num() < 2)
	{
		const double Now = FPlatformTime::Seconds();
		const int32 Index = GPendingWallSwaps.IndexOfByPredicate([this, Now](const FPendingWallSwap& S)
		{
			return Now - S.Time < 5.0 && FVector::Dist(S.Transform.GetLocation(), GetActorLocation()) < 100.f;
		});
		if (Index != INDEX_NONE)
		{
			const FPendingWallSwap Swap = GPendingWallSwaps[Index];
			GPendingWallSwaps.RemoveAt(Index);
			SetActorTransform(Swap.Transform);
			mInwardSign = Swap.InwardSign;
			mCostMultiplier = Swap.CostMultiplier;
			SetSplinePoints(Swap.Points);
		}
	}
	Super::BeginPlay();
}

void ARMBuildableFence::BuildFenceMesh(ERMFenceType Type, TArray<FRMFrame> Frames, float UVScale, float InwardSign, FRMMesh& Main, FRMMesh& Panel,
	const FRMWallTexLayout& WallLayout)
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
		RMGeometry::BuildFenceWall(Frames, UVScale, Main, GetWallHeight(), 50.f, WallLayout);
		break;
	case ERMFenceType::SteelWall:
		RMGeometry::BuildSteelFenceWall(Frames, Main, GetWallHeight());
		break;
	case ERMFenceType::FicsitWall:
		RMGeometry::BuildFenceWall(Frames, UVScale, Main, GetWallHeight(), 50.f, WallLayout); // shape to follow
		break;
	}
}
