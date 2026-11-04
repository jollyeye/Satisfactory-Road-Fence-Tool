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
	DOREPLIFETIME(ARMBuildableFence, mWallLook);
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
	const ERMFenceType Type = GetEffectiveType();
	UMaterialInterface* MainMaterial = GetEffectiveMainMaterial();
	BuildFenceMesh(Type, Frames, mUVScale, mInwardSign, Main, Panel, GetWallLayout());

	Main.ApplyToSection(mMesh, 0, true);
	Panel.ApplyToSection(mMesh, 1, true);
	ApplyMaterial(0, MainMaterial);
	ApplyMaterial(1, mPanelMaterial);

	// Far version: the same fence sampled every 4 m instead of every 50 cm.
	TArray<FRMFrame> FarFrames;
	GetLocalFrames(FarFrames, 400.f);
	if (FarFrames.Num() >= 2)
	{
		FRMMesh FarMain, FarPanel;
		BuildFenceMesh(Type, FarFrames, mUVScale, mInwardSign, FarMain, FarPanel, GetWallLayout());
		ApplyFarSection(FarMain, 0, MainMaterial);
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

uint8 ARMBuildableFence::LookOfType(ERMFenceType Type)
{
	switch (Type)
	{
	case ERMFenceType::ConcreteWall: return 1;
	case ERMFenceType::FicsitWall: return 2;
	case ERMFenceType::SteelWall: return 3;
	default: return 0;
	}
}

ERMFenceType ARMBuildableFence::GetEffectiveType() const
{
	if (!IsWall())
	{
		return mFenceType;
	}
	switch (mPreviewWallLook ? mPreviewWallLook : mWallLook)
	{
	case 1: return ERMFenceType::ConcreteWall;
	case 2: return ERMFenceType::FicsitWall;
	case 3: return ERMFenceType::SteelWall;
	default: return mFenceType;
	}
}

UMaterialInterface* ARMBuildableFence::GetEffectiveMainMaterial() const
{
	const uint8 Look = IsWall() ? (mPreviewWallLook ? mPreviewWallLook : mWallLook) : 0;
	if (Look > 0 && mWallLookMaterials.IsValidIndex(Look - 1) && mWallLookMaterials[Look - 1])
	{
		return mWallLookMaterials[Look - 1];
	}
	return mMainMaterial;
}

void ARMBuildableFence::SetWallLook(uint8 Look)
{
	if (!IsWall() || Look > 3)
	{
		return;
	}
	// Back to the look it was built with: store 0, so the saved value stays "as built".
	mWallLook = Look == LookOfType(mFenceType) ? 0 : Look;
	RebuildMesh();
	ForceNetUpdate();
}

void ARMBuildableFence::SetPreviewWallLook(uint8 Look)
{
	if (!IsWall() || Look == mPreviewWallLook)
	{
		return;
	}
	mPreviewWallLook = Look;
	RebuildMesh();
}

FRMWallTexLayout ARMBuildableFence::GetWallLayout() const
{
	// The layout belongs to the game material of the look shown (a wall can switch looks).
	FRMWallTexLayout L;
	if (GetEffectiveType() == ERMFenceType::FicsitWall)
	{
		// FICSIT look: the game's 1 m FICSIT wall material (MI_WallSetOrange_8x1), one 8 m x 1 m
		// texture repeated up the 4 m wall.
		L.UCentre = 0.5f;
		L.UPerMetre = 0.125f;
		L.VTop = 0.f;
		L.VPerMetre = 1.f;
		L.bMirror = false;
		return L;
	}
	L.UCentre = mWallUCentre;
	L.UPerMetre = mWallUPerMetre;
	L.VTop = mWallVTop;
	L.VPerMetre = mWallVPerMetre;
	L.bMirror = mWallMirror;
	if (mFenceType != ERMFenceType::ConcreteWall && GetEffectiveType() == ERMFenceType::ConcreteWall)
	{
		L = FRMWallTexLayout(); // concrete look on another variant: the concrete layout (the defaults)
	}
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
	Other->mWallLook = mWallLook;
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
