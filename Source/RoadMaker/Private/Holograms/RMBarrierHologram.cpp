#include "Holograms/RMBarrierHologram.h"
#include "RMRoadUtils.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMHologramLook.h"
#include "Holograms/RMOverlayLines.h"
#include "Buildables/RMBuildableBarrier.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "RoadMakerModule.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "UI/RMScreenHint.h"
#include "HAL/PlatformTime.h"

ARMBarrierHologram::ARMBarrierHologram()
{
	mPreviewMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PreviewMesh"));
	if (RootComponent)
	{
		mPreviewMesh->SetupAttachment(RootComponent);
	}
	else
	{
		SetRootComponent(mPreviewMesh);
	}
	mPreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mPreviewMesh->SetCastShadow(false);
	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("BarrierOverlay"));
	mOverlayMesh->SetupAttachment(GetRootComponent());
	mOverlayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mOverlayMesh->SetCastShadow(false);
	mCanLockHologram = false;
	mCanNudgeHologram = false;
}

void ARMBarrierHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBarrierHologram, mStep);
	DOREPLIFETIME(ARMBarrierHologram, mArmLength);
}

bool ARMBarrierHologram::IsValidHitResult(const FHitResult& hitResult) const
{
	const FHitResult Hit = RMRoadUtils::ThroughMarking(hitResult);
	return Cast<ARMBuildableProcedural>(Hit.GetActor()) != nullptr || Super::IsValidHitResult(hitResult);
}

void ARMBarrierHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RMScreenHint::Hide();
	Super::EndPlay(EndPlayReason);
}

bool ARMBarrierHologram::PairEnd(const FRMTunnelSide& Side, FVector& OutEnd) const
{
	int32 Seg;
	float T;
	if (Side.Line.Num() < 2 || (Side.Source == mFirst.Source && Side.Key == mFirst.Key)
		|| !RMOverlayLines::Nearest2D(Side.Line, mPost, Seg, T, OutEnd))
	{
		return false;
	}
	const FVector D = OutEnd - mPost;
	const float W = D.Size2D() - ARMBuildableBarrier::PostHalf;
	return W >= ARMBuildableBarrier::MinArm && W <= ARMBuildableBarrier::MaxArm && FMath::Abs(D.Z) <= 300.f
		&& FMath::Abs(FVector::DotProduct(D.GetSafeNormal2D(), mFirstFwd)) < 0.2f; // straight across
}

void ARMBarrierHologram::ResetTool()
{
	mStep = 0;
	mFirst = FRMTunnelSide();
	mFirstOnFoundation = false;
	mHasHover = false;
	mAcross = false;
	mOverlayMesh->ClearAllMeshSections();
}

bool ARMBarrierHologram::FoundationPoint(const FHitResult& Hit, FVector& OutPoint, FVector& OutAxis) const
{
	FTransform Piece;
	FBox Box;
	if (!RMRoadUtils::GetFoundationPiece(Hit, Piece, Box))
	{
		return false;
	}
	// The grid in the piece's own space, from its centre (its edges and centre are grid points), on its top.
	const FVector Scale = Piece.GetScale3D().GetAbs();
	const FVector Center = Box.GetCenter();
	const FVector Local = Piece.InverseTransformPosition(Hit.Location);
	auto SnapAxis = [](float Value, float Mid, float Min, float Max, float AxisScale)
	{
		const float Step = FoundationGrid / FMath::Max(AxisScale, KINDA_SMALL_NUMBER);
		return FMath::Clamp(Mid + FMath::RoundToFloat((Value - Mid) / Step) * Step, Min, Max);
	};
	OutPoint = Piece.TransformPosition(FVector(SnapAxis(Local.X, Center.X, Box.Min.X, Box.Max.X, Scale.X),
		SnapAxis(Local.Y, Center.Y, Box.Min.Y, Box.Max.Y, Scale.Y), Box.Max.Z));
	FVector View = Hit.TraceEnd - Hit.TraceStart;
	View = View.GetSafeNormal2D();
	float BestDot = -2.f;
	for (const FVector& A : { FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0) })
	{
		const FVector W = Piece.TransformVectorNoScale(A).GetSafeNormal2D();
		const float D = FVector::DotProduct(W, View);
		if (D > BestDot)
		{
			BestDot = D;
			OutAxis = W;
		}
	}
	return true;
}

FVector ARMBarrierHologram::FoundationArmDir() const
{
	const FVector Base = mStep == 1 ? mFirstAxis : mHoverAxis;
	return Base.RotateAngleAxis(90.f * mQuarterTurns, FVector::UpVector).GetSafeNormal2D();
}

bool ARMBarrierHologram::ArmTo(const FVector& End, const FVector& Across)
{
	const FVector D = End - mPost;
	const float W = float(FVector::DotProduct(D, Across)) - ARMBuildableBarrier::PostHalf;
	if (W < ARMBuildableBarrier::MinArm || W > ARMBuildableBarrier::MaxArm || FMath::Abs(D.Z) > 300.f)
	{
		return false;
	}
	const FVector Fwd = FVector::CrossProduct(Across, FVector::UpVector);
	SetActorLocationAndRotation(mPost, FRotator(0.f, Fwd.Rotation().Yaw, 0.f));
	mArmLength = W;
	return true;
}

void ARMBarrierHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult hitResult = RMRoadUtils::ThroughMarking(InHit); // aiming at a lane marking = at its road
	if (mStep > 0 && !mFirstOnFoundation && !mFirst.Source.IsValid())
	{
		ResetTool(); // the first road is gone
	}
	// The scroll wheel: a quarter turn per step (foundation posts).
	const int32 Scroll = GetScrollRotateValue();
	if (mLastScroll != TNumericLimits<int32>::Min() && Scroll != mLastScroll)
	{
		mQuarterTurns = (mQuarterTurns + (Scroll > mLastScroll ? 1 : 3)) % 4;
	}
	mLastScroll = Scroll;

	mHasHover = ARMTunnelHologram::FindSideAt(hitResult, mHover, ARMBuildableBarrier::ColumnInset, false);
	FVector FoundationAt;
	mHoverOnFoundation = !mHasHover && FoundationPoint(hitResult, FoundationAt, mHoverAxis);
	mAcross = false;
	if (mHasHover)
	{
		int32 Seg;
		float T;
		RMOverlayLines::Nearest2D(mHover.Line, mStep == 1 ? mPost : FVector(hitResult.Location), Seg, T, mHoverPoint);
	}
	else if (mHoverOnFoundation)
	{
		mHoverPoint = FoundationAt;
	}
	if (mStep == 0)
	{
		if (mHoverOnFoundation)
		{
			// The post on the grid, its arm shown short along the chosen axis.
			mPost = mHoverPoint;
			const FVector Fwd = FVector::CrossProduct(FoundationArmDir(), FVector::UpVector);
			SetActorLocationAndRotation(mPost, FRotator(0.f, Fwd.Rotation().Yaw, 0.f));
			mArmLength = ARMBuildableBarrier::MinArm;
		}
		else
		{
			SetActorLocationAndRotation(mHasHover ? mHoverPoint : FVector(hitResult.Location), FRotator::ZeroRotator);
		}
	}
	else
	{
		FVector End;
		if (mFirstOnFoundation)
		{
			// Along the post's axis to the aimed point (a foundation grid point or a road edge), measured along the arm.
			mAcross = (mHasHover || mHoverOnFoundation) && ArmTo(mHoverPoint, FoundationArmDir());
		}
		else if (mHasHover && PairEnd(mHover, End))
		{
			mAcross = ArmTo(End, (End - mPost).GetSafeNormal2D());
		}
		else if (mHoverOnFoundation)
		{
			// From a road's post to a foundation: straight across the road, towards the aimed side.
			FVector Across = FVector::CrossProduct(FVector::UpVector, mFirstFwd).GetSafeNormal2D();
			if (FVector::DotProduct(mHoverPoint - mPost, Across) < 0.f)
			{
				Across = -Across;
			}
			mAcross = ArmTo(mHoverPoint, Across);
		}
		if (!mAcross)
		{
			SetActorLocationAndRotation(mPost, GetActorRotation());
		}
	}
	RebuildPreview();
	RebuildOverlay();
	UpdateHint();
}

bool ARMBarrierHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	// One action per click, even if the game reports the click twice (press and release).
	const double Now = FPlatformTime::Seconds();
	if (Now - mLastClickTime < 0.25)
	{
		return false;
	}
	mLastClickTime = Now;
	if (mStep == 0)
	{
		if (mHoverOnFoundation)
		{
			mFirstOnFoundation = true;
			mFirst = FRMTunnelSide();
			mFirstAxis = mHoverAxis; // mPost: set while aiming
			mFirstFwd = FVector::CrossProduct(FoundationArmDir(), FVector::UpVector);
			mStep = 1;
		}
		else if (mHasHover)
		{
			mFirst = mHover;
			int32 Seg = 0;
			float T = 0.f;
			RMOverlayLines::Nearest2D(mFirst.Line, mHoverPoint, Seg, T, mPost);
			mFirstFwd = (mFirst.Line[Seg + 1] - mFirst.Line[Seg]).GetSafeNormal2D();
			mStep = 1;
		}
		return false;
	}
	if (IsOnFirst())
	{
		ResetTool();
		return false;
	}
	return mAcross; // builds
}

void ARMBarrierHologram::CheckValidPlacement()
{
	// No Super call on purpose (as the gantry): the vanilla floor check is for boxes.
	if (mStep == 1 && !mAcross && !IsOnFirst())
	{
		AddConstructDisqualifier(URMCD_GantryNotAcross::StaticClass());
	}
	else if (mStep == 0 && !mHasHover && !mHoverOnFoundation)
	{
		AddConstructDisqualifier(URMCD_AimAtRoad::StaticClass());
	}
}

void ARMBarrierHologram::UpdateHint()
{
	if (mStep == 1)
	{
		if (IsOnFirst())
		{
			RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "BarrierDropFirst", "Boom Barrier: click to drop this side"));
		}
		else if (mAcross)
		{
			RMScreenHint::Show(GetWorld(), FText::Format(NSLOCTEXT("RoadMaker", "BarrierBuildHint", "Boom Barrier: arm {0} m (click to build)"),
				FText::AsNumber(FMath::RoundToInt(mArmLength / 10.f) / 10.f)));
		}
		else
		{
			RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "BarrierSecondSide", "Boom Barrier: aim at the other side (any road or walking path edge straight across, or a foundation)"));
		}
	}
	else if (mHoverOnFoundation)
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "BarrierFoundationPost", "Boom Barrier: click to place the post (scroll turns the arm by 90 degrees)"));
	}
	else
	{
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "BarrierFirstSide", "Boom Barrier: click the side for the post (a road or walking path edge, or a foundation)"));
	}
}

void ARMBarrierHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || (mStep == 0 && !mHasHover && !mHoverOnFoundation))
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	RMOverlayLines::FOverlay B;
	B.Base = GetActorLocation();
	B.Camera = PC->PlayerCameraManager->GetCameraLocation();
	B.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);
	if (mStep == 1)
	{
		if (!mFirstOnFoundation)
		{
			B.Polyline(mFirst.Line, FLinearColor(0.f, 1.f, 0.f)); // the post's side: green, and the post's arrow
		}
		B.Arrow(mPost, FLinearColor(0.f, 1.f, 0.f));
	}
	if ((mHasHover || mHoverOnFoundation) && !(mStep == 1 && IsOnFirst()))
	{
		const bool bBad = mStep == 1 && !mAcross;
		const FLinearColor Color = bBad ? FLinearColor(1.f, 0.05f, 0.05f) : FLinearColor(1.f, 1.f, 1.f);
		if (mHasHover)
		{
			B.Polyline(mHover.Line, Color);
		}
		B.Arrow(mHoverPoint, Color);
	}
	const FTransform T = mOverlayMesh->GetComponentTransform();
	for (FVector& V : B.Vertices)
	{
		V = T.InverseTransformPosition(V + B.Base);
	}
	mOverlayMesh->CreateMeshSection_LinearColor(0, B.Vertices, B.Triangles, TArray<FVector>(), TArray<FVector2D>(), B.Colors, TArray<FProcMeshTangent>(), false);
	if (UMaterialInterface* Overlay = RMOverlayLines::OverlayMaterial())
	{
		mOverlayMesh->SetMaterial(0, Overlay);
	}
	mOverlayMesh->SetRenderCustomDepth(false);
}

void ARMBarrierHologram::RebuildPreview()
{
	// The post where it will stand; the arm once the other side is known.
	const float Length = (mAcross || (mStep == 0 && mHoverOnFoundation)) ? mArmLength : 0.f;
	if (FMath::IsNearlyEqual(mShownLength, Length, 0.5f) && mPreviewMesh->GetNumSections() > 0)
	{
		return;
	}
	mShownLength = Length;
	mPreviewMesh->ClearAllMeshSections();
	FRMMesh Mesh, Body, Light;
	ARMBuildableBarrier::BuildPostMesh(Mesh, Body, Light);
	FRMMesh Arm;
	if (Length > 0.f)
	{
		ARMBuildableBarrier::BuildArmMesh(Length, Arm);
	}
	const FVector Hinge(0.f, ARMBuildableBarrier::HingeFace, ARMBuildableBarrier::HingeHeight);
	for (const TPair<const FRMMesh*, FVector>& Part : { TPair<const FRMMesh*, FVector>(&Body, FVector::ZeroVector),
		TPair<const FRMMesh*, FVector>(&Light, FVector::ZeroVector), TPair<const FRMMesh*, FVector>(&Arm, Hinge) })
	{
		const FRMMesh& P = *Part.Key;
		const int32 Base = Mesh.Vertices.Num();
		for (int32 V = 0; V < P.Vertices.Num(); ++V)
		{
			Mesh.AddVertex(P.Vertices[V] + Part.Value, P.Normals[V], P.UVs[V], P.Tangents[V].TangentX);
		}
		for (int32 I = 0; I + 2 < P.Triangles.Num(); I += 3)
		{
			Mesh.Triangles.Append({ Base + P.Triangles[I], Base + P.Triangles[I + 1], Base + P.Triangles[I + 2] });
		}
	}
	Mesh.ApplyToSection(mPreviewMesh, 0, false);
	UMaterialInterface* Material = (mPlacementMaterialState == EHologramMaterialState::HMS_ERROR && mInvalidPlacementMaterial)
		? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
	RMHologramLook::Apply(mPreviewMesh, Material, GetStencilForHologramMaterialState(mPlacementMaterialState));
}

void ARMBarrierHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableBarrier* B = Cast<ARMBuildableBarrier>(inBuildable))
	{
		B->SetArmLength(mArmLength);
	}
}

AActor* ARMBarrierHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	AActor* Built = Super::Construct(out_children, constructionID);
	if (Built)
	{
		ResetTool(); // the next one starts over
	}
	return Built;
}
