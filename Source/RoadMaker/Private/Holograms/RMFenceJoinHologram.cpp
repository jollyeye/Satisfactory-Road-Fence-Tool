#include "Holograms/RMFenceJoinHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableFence.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	/** Camera-facing lines for the fence join outlines, in actor space. */
	struct FRMFenceJoinOverlay
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

		/** A fence's box: the bottom and top edges along both sides, and the two end faces. */
		void Outline(const TArray<FRMFrame>& Frames, const ARMBuildableFence* Fence, float Pixels, const FLinearColor& Color)
		{
			if (Frames.Num() < 2 || !Fence)
			{
				return;
			}
			float Half = 50.f, Bottom = 0.f, Top = 100.f;
			Fence->GetCutSection(Half, Bottom, Top);
			Half += 5.f;
			Top += 5.f;
			const bool bUpright = Fence->IsCutSectionUpright();
			auto Corner = [&](const FRMFrame& F, float Side, float Height)
			{
				const FVector Up = bUpright ? FVector::UpVector : F.Up;
				const FVector Right = bUpright ? FVector(F.Right.X, F.Right.Y, 0.f).GetSafeNormal() : F.Right;
				return F.Pos + Right * (Half * Side) + Up * Height;
			};
			for (int32 I = 1; I < Frames.Num(); ++I)
			{
				for (const float Side : { -1.f, 1.f })
				{
					Line(Corner(Frames[I - 1], Side, Bottom), Corner(Frames[I], Side, Bottom), Pixels, Color);
					Line(Corner(Frames[I - 1], Side, Top), Corner(Frames[I], Side, Top), Pixels, Color);
				}
			}
			for (const FRMFrame* F : { &Frames[0], &Frames.Last() })
			{
				Line(Corner(*F, -1.f, Bottom), Corner(*F, 1.f, Bottom), Pixels, Color);
				Line(Corner(*F, -1.f, Top), Corner(*F, 1.f, Top), Pixels, Color);
				Line(Corner(*F, -1.f, Bottom), Corner(*F, -1.f, Top), Pixels, Color);
				Line(Corner(*F, 1.f, Bottom), Corner(*F, 1.f, Top), Pixels, Color);
			}
		}
	};

	bool FenceJoinUsable(const ARMBuildableFence* Fence)
	{
		return Fence && !Fence->GetIsDismantled() && !Fence->IsActorBeingDestroyed() && !Fence->IsHidden();
	}

	/** A fence end: where it is and the way out of the fence there (seen from above). */
	void FenceEnd(const ARMBuildableFence* Fence, int32 End, FVector& OutLoc, FVector& OutOut)
	{
		TArray<FRMFrame> Frames;
		Fence->GetWorldFrames(Frames, 100.f);
		if (Frames.Num() < 2)
		{
			OutLoc = Fence->GetActorLocation();
			OutOut = FVector::ZeroVector;
			return;
		}
		OutLoc = End == 0 ? Frames[0].Pos : Frames.Last().Pos;
		OutOut = (End == 0 ? -Frames[0].Fwd : Frames.Last().Fwd).GetSafeNormal2D();
	}
}

ARMFenceJoinHologram::ARMFenceJoinHologram()
{
	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("FenceJoinOverlay"));
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

void ARMFenceJoinHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMFenceJoinHologram, mFence);
	DOREPLIFETIME(ARMFenceJoinHologram, mOther);
	DOREPLIFETIME(ARMFenceJoinHologram, mFenceEnd);
	DOREPLIFETIME(ARMFenceJoinHologram, mOtherEnd);
}

bool ARMFenceJoinHologram::IsValidHitResult(const FHitResult& hitResult) const
{
	return Cast<ARMBuildableFence>(hitResult.GetActor()) != nullptr || Super::IsValidHitResult(hitResult);
}

void ARMFenceJoinHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	ARMBuildableFence* Hit = Cast<ARMBuildableFence>(hitResult.GetActor());
	mFence = nullptr;
	mOther = nullptr;
	if (FenceJoinUsable(Hit) && GetWorld())
	{
		// The aimed fence's end nearer the aim, and the fence whose end meets it (facing the other way).
		FVector Loc0, Out0, Loc1, Out1;
		FenceEnd(Hit, 0, Loc0, Out0);
		FenceEnd(Hit, 1, Loc1, Out1);
		const int32 End = FVector::DistSquared(hitResult.Location, Loc0) <= FVector::DistSquared(hitResult.Location, Loc1) ? 0 : 1;
		const FVector EndLoc = End == 0 ? Loc0 : Loc1;
		const FVector EndOut = End == 0 ? Out0 : Out1;
		float Best = 50.f;
		for (TActorIterator<ARMBuildableFence> It(GetWorld()); It; ++It)
		{
			ARMBuildableFence* Other = *It;
			if (Other == Hit || !FenceJoinUsable(Other))
			{
				continue;
			}
			// Quick reject: fences far away (bounds of the whole fence).
			if (Other->GetComponentsBoundingBox().ComputeSquaredDistanceToPoint(EndLoc) > 200.f * 200.f)
			{
				continue;
			}
			for (int32 OtherEnd = 0; OtherEnd < 2; ++OtherEnd)
			{
				FVector Loc, Out;
				FenceEnd(Other, OtherEnd, Loc, Out);
				const float D = FVector::Dist2D(Loc, EndLoc);
				if (D <= Best && FMath::Abs(Loc.Z - EndLoc.Z) < 50.f && FVector::DotProduct(Out, EndOut) < -0.9f)
				{
					Best = D;
					mFence = Hit;
					mFenceEnd = End;
					mOther = Other;
					mOtherEnd = OtherEnd;
				}
			}
		}
	}
	SetActorLocationAndRotation(hitResult.Location, FRotator::ZeroRotator);
	RebuildOverlay();
}

void ARMFenceJoinHologram::CheckValidPlacement()
{
	// No Super: nothing is placed, so the game's floor / clearance checks do not apply.
	if (!mFence || !mOther)
	{
		AddConstructDisqualifier(URMCD_FenceJoinAimAtJoint::StaticClass());
		return;
	}
	switch (mFence->CheckJoin(mOther, mFenceEnd, mOtherEnd))
	{
	case ARMBuildableFence::EJoinCheck::Ok:
		break;
	case ARMBuildableFence::EJoinCheck::Type:
		AddConstructDisqualifier(URMCD_FenceJoinType::StaticClass());
		break;
	case ARMBuildableFence::EJoinCheck::Side:
		AddConstructDisqualifier(URMCD_FenceJoinSide::StaticClass());
		break;
	case ARMBuildableFence::EJoinCheck::Kink:
		AddConstructDisqualifier(URMCD_FenceJoinKink::StaticClass());
		break;
	case ARMBuildableFence::EJoinCheck::Curve:
		AddConstructDisqualifier(URMCD_FenceJoinCurve::StaticClass());
		break;
	case ARMBuildableFence::EJoinCheck::TooLong:
		AddConstructDisqualifier(URMCD_FenceJoinTooLong::StaticClass());
		break;
	default:
		AddConstructDisqualifier(URMCD_FenceJoinAimAtJoint::StaticClass());
		break;
	}
}

AActor* ARMFenceJoinHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	// The join itself (server): the new fence replaces the two; it is handed back as "what was built".
	ARMBuildableFence* Joined = nullptr;
	if (FenceJoinUsable(mFence) && FenceJoinUsable(mOther) && mFence != mOther)
	{
		Joined = mFence->JoinWith(mOther, mFenceEnd, mOtherEnd);
	}
	mFence = nullptr;
	mOther = nullptr;
	mOverlayMesh->ClearAllMeshSections();
	return Joined;
}

void ARMFenceJoinHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || !mFence || !mOther)
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	FRMFenceJoinOverlay B;
	B.Base = GetActorLocation();
	B.Camera = PC->PlayerCameraManager->GetCameraLocation();
	B.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);

	TArray<FRMFrame> Frames;
	if (mFence->CheckJoin(mOther, mFenceEnd, mOtherEnd, &Frames) == ARMBuildableFence::EJoinCheck::Ok)
	{
		// The joined fence: one outline around both.
		B.Outline(Frames, mFence, 10.f, FLinearColor(0.f, 1.f, 0.f) /* #00FF00 */);
	}
	else
	{
		Frames.Reset();
		mFence->GetWorldFrames(Frames, 200.f);
		B.Outline(Frames, mFence, 8.f, FLinearColor(1.f, 1.f, 1.f));
		Frames.Reset();
		mOther->GetWorldFrames(Frames, 200.f);
		B.Outline(Frames, mOther, 8.f, FLinearColor(1.f, 0.05f, 0.05f));
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
