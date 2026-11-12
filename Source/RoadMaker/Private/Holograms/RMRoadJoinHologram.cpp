#include "Holograms/RMRoadJoinHologram.h"
#include "RMRoadUtils.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableRoad.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"

namespace
{
	/** Camera-facing lines for the join outlines, in actor space. */
	struct FRMJoinOverlay
	{
		FVector Base;
		FVector Camera;
		float WorldPerPixel = 0.f;
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FLinearColor> Colors;

		void Line(FVector A, FVector B, float Pixels, const FLinearColor& Color)
		{
			const FVector Dir = (B - A).GetSafeNormal();
			const float HA = Pixels * WorldPerPixel * FMath::Max(FVector::Dist(A, Camera), 10.f) * 0.5f;
			const float HB = Pixels * WorldPerPixel * FMath::Max(FVector::Dist(B, Camera), 10.f) * 0.5f;
			A -= Dir * HA;
			B += Dir * HB;
			const FVector SA = FVector::CrossProduct(Dir, Camera - A).GetSafeNormal();
			const FVector SB = FVector::CrossProduct(Dir, Camera - B).GetSafeNormal();
			const int32 I = Vertices.Num();
			for (const FVector& P : { A - SA * HA, A + SA * HA, B + SB * HB, B - SB * HB })
			{
				Vertices.Add(P - Base);
				Colors.Add(Color);
			}
			Triangles.Append({ I, I + 1, I + 2, I, I + 2, I + 3 });
		}

		/** The outline of a road's top seen from above: both edges and both ends. */
		void Outline(const TArray<FRMFrame>& Frames, float Width, float Pixels, const FLinearColor& Color)
		{
			if (Frames.Num() < 2)
			{
				return;
			}
			const float Half = Width * 0.5f;
			auto Edge = [&](const FRMFrame& F, float Side) { return F.Pos + F.Right * (Half * Side) + F.Up * 6.f; };
			for (int32 I = 1; I < Frames.Num(); ++I)
			{
				Line(Edge(Frames[I - 1], -1.f), Edge(Frames[I], -1.f), Pixels, Color);
				Line(Edge(Frames[I - 1], 1.f), Edge(Frames[I], 1.f), Pixels, Color);
			}
			Line(Edge(Frames[0], -1.f), Edge(Frames[0], 1.f), Pixels, Color);
			Line(Edge(Frames.Last(), -1.f), Edge(Frames.Last(), 1.f), Pixels, Color);
		}
	};

	bool JoinUsable(const ARMBuildableRoad* Road)
	{
		return Road && !Road->GetIsDismantled() && !Road->IsActorBeingDestroyed() && !Road->IsHidden();
	}
}

ARMRoadJoinHologram::ARMRoadJoinHologram()
{
	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("JoinOverlay"));
	if (RootComponent)
	{
		mOverlayMesh->SetupAttachment(RootComponent);
	}
	else
	{
		SetRootComponent(mOverlayMesh);
	}
	mOverlayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mOverlayMesh->SetCastShadow(false);
	mCanLockHologram = false;
	mCanNudgeHologram = false;
}

void ARMRoadJoinHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMRoadJoinHologram, mRoad);
	DOREPLIFETIME(ARMRoadJoinHologram, mOther);
	DOREPLIFETIME(ARMRoadJoinHologram, mRoadEnd);
	DOREPLIFETIME(ARMRoadJoinHologram, mOtherEnd);
}

bool ARMRoadJoinHologram::IsValidHitResult(const FHitResult& InHit) const
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit);
	return Cast<ARMBuildableRoad>(hitResult.GetActor()) != nullptr || Super::IsValidHitResult(hitResult);
}

void ARMRoadJoinHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit); // aiming at a lane marking = at its road
	ARMBuildableRoad* Hit = Cast<ARMBuildableRoad>(hitResult.GetActor());
	mRoad = nullptr;
	mOther = nullptr;
	if (JoinUsable(Hit) && GetWorld())
	{
		// The aimed road's end nearer the aim, and the road whose end meets it (facing the other way).
		FVector Loc0, Out0, Loc1, Out1;
		Hit->GetEndWorld(0, Loc0, Out0);
		Hit->GetEndWorld(1, Loc1, Out1);
		const int32 End = FVector::DistSquared(hitResult.Location, Loc0) <= FVector::DistSquared(hitResult.Location, Loc1) ? 0 : 1;
		const FVector EndLoc = End == 0 ? Loc0 : Loc1;
		const FVector EndOut = End == 0 ? Out0 : Out1;
		TArray<FRMRoadEndRef> Near;
		RMRoadUtils::FindFreeEnds(GetWorld(), EndLoc, 200.f, 0, nullptr, Near, true);
		float Best = 50.f;
		for (const FRMRoadEndRef& R : Near)
		{
			if (!R.Road || R.Road == Hit || !JoinUsable(R.Road))
			{
				continue;
			}
			const float D = FVector::Dist2D(R.Location, EndLoc);
			if (D <= Best && FVector::DotProduct(R.Outward.GetSafeNormal2D(), EndOut.GetSafeNormal2D()) < -0.9f)
			{
				Best = D;
				mRoad = Hit;
				mRoadEnd = End;
				mOther = R.Road;
				mOtherEnd = R.EndIndex;
			}
		}
	}
	SetActorLocationAndRotation(hitResult.Location, FRotator::ZeroRotator);
	RebuildOverlay();
}

void ARMRoadJoinHologram::CheckValidPlacement()
{
	// No Super: nothing is placed, so the game's floor / clearance checks do not apply.
	if (!mRoad || !mOther)
	{
		AddConstructDisqualifier(URMCD_JoinAimAtJoint::StaticClass());
		return;
	}
	switch (mRoad->CheckJoin(mOther, mRoadEnd, mOtherEnd))
	{
	case ARMBuildableRoad::EJoinCheck::Ok:
		break;
	case ARMBuildableRoad::EJoinCheck::Width:
		AddConstructDisqualifier(URMCD_JoinWidth::StaticClass());
		break;
	case ARMBuildableRoad::EJoinCheck::Kink:
		AddConstructDisqualifier(URMCD_JoinKink::StaticClass());
		break;
	case ARMBuildableRoad::EJoinCheck::Curve:
		AddConstructDisqualifier(URMCD_JoinCurve::StaticClass());
		break;
	case ARMBuildableRoad::EJoinCheck::TooLong:
		AddConstructDisqualifier(URMCD_TooLong::StaticClass());
		break;
	default:
		AddConstructDisqualifier(URMCD_JoinAimAtJoint::StaticClass());
		break;
	}
}

AActor* ARMRoadJoinHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	// The join itself (server): the new road replaces the two; it is handed back as "what was built".
	ARMBuildableRoad* Joined = nullptr;
	if (JoinUsable(mRoad) && JoinUsable(mOther) && mRoad != mOther)
	{
		Joined = mRoad->JoinWith(mOther, mRoadEnd, mOtherEnd);
	}
	mRoad = nullptr;
	mOther = nullptr;
	mOverlayMesh->ClearAllMeshSections();
	return Joined;
}

void ARMRoadJoinHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || !mRoad || !mOther)
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	FRMJoinOverlay B;
	B.Base = GetActorLocation();
	B.Camera = PC->PlayerCameraManager->GetCameraLocation();
	B.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);

	TArray<FRMFrame> Frames;
	if (mRoad->CheckJoin(mOther, mRoadEnd, mOtherEnd, &Frames) == ARMBuildableRoad::EJoinCheck::Ok)
	{
		// The joined road: one outline around both.
		B.Outline(Frames, mRoad->GetRoadWidth(), 10.f, FLinearColor(0.f, 0.9f, 1.f));
	}
	else
	{
		Frames.Reset();
		mRoad->GetWorldFrames(Frames, 200.f);
		B.Outline(Frames, mRoad->GetRoadWidth(), 8.f, FLinearColor(1.f, 1.f, 1.f));
		Frames.Reset();
		mOther->GetWorldFrames(Frames, 200.f);
		B.Outline(Frames, mOther->GetRoadWidth(), 8.f, FLinearColor(1.f, 0.05f, 0.05f));
	}

	// Kept loaded for good: a plain static pointer would dangle once the garbage collector frees it.
	static UMaterialInterface* Material = [] { UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay")); if (M) { M->AddToRoot(); } return M; }();
	mOverlayMesh->CreateMeshSection_LinearColor(0, B.Vertices, B.Triangles, TArray<FVector>(), TArray<FVector2D>(), B.Colors, TArray<FProcMeshTangent>(), false);
	if (Material)
	{
		mOverlayMesh->SetMaterial(0, Material);
	}
	mOverlayMesh->SetRenderCustomDepth(false);
}
