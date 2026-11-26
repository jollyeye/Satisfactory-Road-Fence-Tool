#include "Holograms/RMRoadSmoothHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMHologramLook.h"
#include "Buildables/RMBuildableRoad.h"
#include "UI/RMScreenHint.h"
#include "Holograms/RMOverlayLines.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "FGDismantleInterface.h"
#include "RMRoadUtils.h"
#include "RMGeometry.h"
#include "RoadMakerModule.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "FGRecipe.h"

namespace RMSmoothJoint
{
	constexpr float Step = 100.f;       // setback in whole metres
	constexpr float FirstSetback = 800.f;

	float SlopeOf(const FVector& Dir)
	{
		const float Flat = FVector(Dir.X, Dir.Y, 0.f).Size();
		return Flat > KINDA_SMALL_NUMBER ? Dir.Z / Flat : 0.f;
	}

	void SetTangentSlope(FVector& Tangent, float Slope)
	{
		Tangent.Z = FVector(Tangent.X, Tangent.Y, 0.f).Size() * Slope;
	}

	/** Removes a cut-away piece with what rides on it (markings, tracks; its vehicle paths go after it). */
	void RemovePiece(AActor* Piece)
	{
		if (!Piece || Piece->IsActorBeingDestroyed() || !Piece->Implements<UFGDismantleInterface>())
		{
			return;
		}
		TArray<AActor*> Children;
		IFGDismantleInterface::Execute_GetChildDismantleActors(Piece, Children);
		for (AActor* Child : Children)
		{
			if (IsValid(Child) && !Child->IsActorBeingDestroyed() && Child->Implements<UFGDismantleInterface>())
			{
				IFGDismantleInterface::Execute_Dismantle(Child);
			}
		}
		IFGDismantleInterface::Execute_Dismantle(Piece);
	}
}

ARMRoadSmoothHologram::ARMRoadSmoothHologram()
{
	mPreviewMeshC = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PreviewMeshC"));
	if (RootComponent)
	{
		mPreviewMeshC->SetupAttachment(RootComponent);
	}
	mPreviewMeshC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mPreviewMeshC->SetCastShadow(false);
}

void ARMRoadSmoothHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMRoadSmoothHologram, mSetback);
}

void ARMRoadSmoothHologram::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RMScreenHint::Hide();
	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------------------------
// Picking the joint, then the setback
// ---------------------------------------------------------------------------------------------

void ARMRoadSmoothHologram::SetHologramLocationAndRotation(const FHitResult& InHit)
{
	const FHitResult Hit = RMRoadUtils::ThroughMarking(InHit);
	if (mStep == 0)
	{
		// Only joints of exactly two road ends (Adjust Joint also offers single free ends).
		RefreshEnds();
		mEnds.RemoveAll([](const FEnd& E) { return !E.Partner.IsValid(); });
		mAimed = FindAimedEnd(Hit.Location);
		SetActorLocationAndRotation(mCircleBase, FRotator::ZeroRotator);
		if (mAimed != mDrawnAimed)
		{
			RebuildCircles();
		}
		RMScreenHint::Show(GetWorld(), NSLOCTEXT("RoadMaker", "SmoothPickHint", "Smooth Joint: click a joint where two roads meet"));
		return;
	}
	// The setback: how far the aim is from the joint (seen from above), whole metres.
	const float Max = MaxSetback();
	const float Aimed = FMath::RoundToFloat(FVector::Dist2D(Hit.Location, mJoint) / RMSmoothJoint::Step) * RMSmoothJoint::Step;
	mSetback = FMath::Clamp(Aimed, RMSmoothJoint::Step, FMath::Max(Max, RMSmoothJoint::Step));
	UpdateCurve();
	RMScreenHint::Show(GetWorld(), FText::Format(NSLOCTEXT("RoadMaker", "SmoothSetbackHint", "Smooth Joint: {0} m from the joint (aim nearer or further; click to build)"),
		FText::AsNumber(FMath::RoundToInt(mSetback / 100.f))), Max < RMSmoothJoint::Step);
}

bool ARMRoadSmoothHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	if (mStep == 0)
	{
		if (!mEnds.IsValidIndex(mAimed) || !mEnds[mAimed].Road.IsValid() || !mEnds[mAimed].Partner.IsValid())
		{
			return false;
		}
		const FEnd& End = mEnds[mAimed];
		mRoadA = End.Road.Get();
		mEndA = End.EndIndex;
		mRoadB = End.Partner.Get();
		mEndB = End.PartnerEnd;
		mJoint = End.Location;
		mStep = 1;
		mSetback = FMath::Min(RMSmoothJoint::FirstSetback, FMath::Max(MaxSetback(), RMSmoothJoint::Step));
		SetGrabbedRoadsHidden(true);
		UpdateCurve();
		return false;
	}
	return true;
}

void ARMRoadSmoothHologram::UpdateCurve()
{
	if (mStep == 0 || !mRoadA || !mRoadB)
	{
		return;
	}
	SetActorLocationAndRotation(mJoint, FRotator::ZeroRotator);
	RebuildSmoothPreviews(); // also the cut marks (no outline of the old roads: the previews show what is left)
}

// ---------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------

bool ARMRoadSmoothHologram::TrimRoad(const ARMBuildableRoad* Road, int32 JointEnd, float Setback, FTrim& Out)
{
	if (!Road)
	{
		return false;
	}
	TArray<FRMFrame> Frames;
	Road->GetWorldFrames(Frames, 25.f);
	if (Frames.Num() < 2)
	{
		return false;
	}
	Out.Length = Frames.Last().Dist;
	Out.CutDist = JointEnd == 1 ? Out.Length - Setback : Setback;
	if (Out.CutDist <= 1.f || Out.CutDist >= Out.Length - 1.f)
	{
		return false;
	}
	// The exact frame at the cut (as the Cut tool finds it).
	FRMFrame Cut = Frames.Last();
	for (int32 I = 1; I < Frames.Num(); ++I)
	{
		if (Frames[I].Dist >= Out.CutDist)
		{
			const FRMFrame& A = Frames[I - 1];
			const FRMFrame& B = Frames[I];
			const float Alpha = (Out.CutDist - A.Dist) / FMath::Max(B.Dist - A.Dist, KINDA_SMALL_NUMBER);
			Cut = A;
			Cut.Pos = FMath::Lerp(A.Pos, B.Pos, Alpha);
			Cut.Fwd = FMath::Lerp(A.Fwd, B.Fwd, Alpha).GetSafeNormal();
			Cut.Right = FMath::Lerp(A.Right, B.Right, Alpha).GetSafeNormal();
			Cut.Up = FMath::Lerp(A.Up, B.Up, Alpha).GetSafeNormal();
			Cut.Dist = Out.CutDist;
			break;
		}
	}
	// What is kept: from the far end to the cut, in the road's own direction of travel.
	TArray<FRMFrame> Kept;
	if (JointEnd == 1)
	{
		for (const FRMFrame& F : Frames)
		{
			if (F.Dist < Out.CutDist - 0.5f)
			{
				Kept.Add(F);
			}
		}
		Kept.Add(Cut);
	}
	else
	{
		Kept.Add(Cut);
		for (const FRMFrame& F : Frames)
		{
			if (F.Dist > Out.CutDist + 0.5f)
			{
				Kept.Add(F);
			}
		}
		for (FRMFrame& F : Kept)
		{
			F.Dist -= Out.CutDist;
		}
	}
	RMGeometry::OffsetCurve(Kept, 0.f, 200.f, Out.Kept);
	Out.Cut = Cut.Pos;
	Out.TowardJoint = JointEnd == 1 ? Cut.Fwd : -Cut.Fwd;
	Out.Right = Cut.Right;
	return Out.Kept.Num() >= 2;
}

bool ARMRoadSmoothHologram::ComputeSmooth(FTrim& OutA, FTrim& OutB, TArray<FRMSplinePoint>& OutCurve) const
{
	OutCurve.Reset();
	if (!mRoadA || !mRoadB || !TrimRoad(mRoadA, mEndA, mSetback, OutA) || !TrimRoad(mRoadB, mEndB, mSetback, OutB))
	{
		return false;
	}
	// From road A's cut towards the joint, round the old corner (the control point where both lines cross), out
	// along road B: flush with both.
	const FVector StartDir = OutA.TowardJoint;
	const FVector EndDir = -OutB.TowardJoint;
	RMGeometry::MakeQuadratic(OutA.Cut, StartDir, OutB.Cut, EndDir, OutCurve);
	if (OutCurve.Num() >= 2)
	{
		// Leaves and arrives with each road's slope.
		RMSmoothJoint::SetTangentSlope(OutCurve[0].Tangent, RMSmoothJoint::SlopeOf(StartDir));
		RMSmoothJoint::SetTangentSlope(OutCurve.Last().Tangent, RMSmoothJoint::SlopeOf(EndDir));
	}
	return OutCurve.Num() >= 2;
}

float ARMRoadSmoothHologram::MaxSetback() const
{
	if (!mRoadA || !mRoadB)
	{
		return 0.f;
	}
	const float A = mRoadA->GetLength() - FMath::Max(mRoadA->mMinLength, 100.f);
	const float B = mRoadB->GetLength() - FMath::Max(mRoadB->mMinLength, 100.f);
	return FMath::FloorToFloat(FMath::Min(A, B) / RMSmoothJoint::Step) * RMSmoothJoint::Step;
}

// ---------------------------------------------------------------------------------------------
// Preview and checks
// ---------------------------------------------------------------------------------------------

void ARMRoadSmoothHologram::RebuildSmoothPreviews()
{
	for (UProceduralMeshComponent* Mesh : { mPreviewMesh, mPreviewMeshB, mPreviewMeshC, mCircleMesh })
	{
		Mesh->ClearAllMeshSections();
	}
	FTrim A, B;
	TArray<FRMSplinePoint> Curve;
	if (!ComputeSmooth(A, B, Curve))
	{
		return;
	}
	RebuildCutMarks(A, B);
	auto Build = [this](const ARMBuildableRoad* Road, const TArray<FRMSplinePoint>& Points, UProceduralMeshComponent* Mesh)
	{
		TArray<FRMFrame> Frames;
		RMGeometry::SampleSpline(Points, 100.f, Frames);
		RMGeometry::ReduceRoadFrames(Frames, Road->IsWalkingPath()); // as the built road
		if (Frames.Num() < 2)
		{
			return;
		}
		for (FRMFrame& F : Frames)
		{
			F.Pos -= mJoint; // the actor sits at the joint
		}
		FRMMesh Top, Body;
		RMGeometry::BuildRoad(Frames, Road->GetRoadWidth(), Road->GetRoadHeight(), 400.f, Road->GetMarkings(), Road->mEdgeLineInset, Top, Body);
		Top.ApplyToSection(Mesh, 0, false);
		Body.ApplyToSection(Mesh, 1, false);
	};
	Build(mRoadA, A.Kept, mPreviewMesh);
	Build(mRoadB, B.Kept, mPreviewMeshB);
	Build(mRoadA, Curve, mPreviewMeshC); // the new curve is made like road A
	ApplyRoadSurfaces();
}

void ARMRoadSmoothHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state); // the game's hologram material ...
	ApplyRoadSurfaces();            // ... replaced by the roads' own surfaces, the state shown by the outline
}

void ARMRoadSmoothHologram::ApplyRoadSurfaces()
{
	const uint8 Stencil = GetStencilForHologramMaterialState(mPlacementMaterialState);
	auto Surface = [Stencil](UProceduralMeshComponent* Mesh, const ARMBuildableRoad* Road)
	{
		if (!Mesh || !IsValid(Road) || Mesh->GetNumSections() == 0)
		{
			return;
		}
		Mesh->SetMaterial(0, Road->GetLookMaterialFor(Road->GetShownLook(), true, Road->mTopMaterial));
		if (Mesh->GetNumSections() > 1)
		{
			Mesh->SetMaterial(1, Road->GetLookMaterialFor(Road->GetShownLook(), false, Road->mBodyMaterial));
		}
		// Its paint (lane line colours, the cyberpunk glow), as the road's own mesh has it.
		if (const UPrimitiveComponent* Source = Road->GetProceduralMesh())
		{
			const TArray<float>& Data = Source->GetCustomPrimitiveData().Data;
			for (int32 I = 0; I < Data.Num(); ++I)
			{
				Mesh->SetCustomPrimitiveDataFloat(I, Data[I]);
			}
		}
		RMHologramLook::Apply(Mesh, nullptr, Stencil);
	};
	Surface(mPreviewMesh, mRoadA);
	Surface(mPreviewMeshB, mRoadB);
	Surface(mPreviewMeshC, mRoadA);
}

void ARMRoadSmoothHologram::RebuildCutMarks(const FTrim& A, const FTrim& B)
{
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || !mRoadA || !mRoadB)
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	RMOverlayLines::FOverlay O;
	O.Base = GetActorLocation();
	O.Camera = PC->PlayerCameraManager->GetCameraLocation();
	O.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);
	const FLinearColor Color(1.f, 0.55f, 0.f); // the tool's orange
	for (const TPair<const FTrim*, const ARMBuildableRoad*>& Cut : { TPair<const FTrim*, const ARMBuildableRoad*>(&A, mRoadA), TPair<const FTrim*, const ARMBuildableRoad*>(&B, mRoadB) })
	{
		const float Half = Cut.Value->GetRoadWidth() * 0.5f;
		const FVector Left = Cut.Key->Cut - Cut.Key->Right * Half;
		const FVector Right = Cut.Key->Cut + Cut.Key->Right * Half;
		O.Line(Left, Right, Color);
		O.Line(Left, Left + FVector(0.f, 0.f, 250.f), Color);
		O.Line(Right, Right + FVector(0.f, 0.f, 250.f), Color);
	}
	mCircleMesh->CreateMeshSection_LinearColor(0, O.Vertices, O.Triangles, TArray<FVector>(), TArray<FVector2D>(), O.Colors, TArray<FProcMeshTangent>(), false);
	if (UMaterialInterface* Overlay = RMOverlayLines::OverlayMaterial())
	{
		mCircleMesh->SetMaterial(0, Overlay);
	}
	mCircleMesh->SetRenderCustomDepth(false);
}

void ARMRoadSmoothHologram::CheckValidPlacement()
{
	if (mStep == 0 || !mRoadA || !mRoadB)
	{
		if (!mEnds.IsValidIndex(mAimed))
		{
			AddConstructDisqualifier(URMCD_AimAtRoadEnd::StaticClass());
		}
		return;
	}
	FTrim A, B;
	TArray<FRMSplinePoint> Curve;
	if (MaxSetback() < RMSmoothJoint::Step || !ComputeSmooth(A, B, Curve))
	{
		AddConstructDisqualifier(URMCD_TooShort::StaticClass());
		return;
	}
	TArray<FRMFrame> Frames;
	RMGeometry::SampleSpline(Curve, 50.f, Frames);
	const FRMSplineStats Stats = RMGeometry::Analyze(Frames);
	if (Stats.Length < mRoadA->mMinLength)
	{
		AddConstructDisqualifier(URMCD_TooShort::StaticClass());
	}
	if (Stats.Length > mRoadA->mMaxLength)
	{
		AddConstructDisqualifier(URMCD_TooLong::StaticClass());
	}
	if (Stats.MinTurnRadius < mRoadA->GetEffectiveMinTurnRadius() * 0.95f)
	{
		AddConstructDisqualifier(URMCD_TurnTooTight::StaticClass());
	}
	if (Stats.MaxGradeDegrees > mRoadA->mMaxGradeDegrees)
	{
		AddConstructDisqualifier(URMCD_TooSteep::StaticClass());
	}
}

// ---------------------------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------------------------

AActor* ARMRoadSmoothHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	ARMBuildableSpline* NewCurve = nullptr;
	FTrim A, B;
	TArray<FRMSplinePoint> Curve;
	ARMBuildableRoad* RoadA = mRoadA;
	ARMBuildableRoad* RoadB = mRoadB;
	if (RoadA && RoadB && !RoadA->GetIsDismantled() && !RoadB->GetIsDismantled() && ComputeSmooth(A, B, Curve))
	{
		// The curve costs what road A costs for its length.
		TArray<FRMFrame> CurveFrames;
		RMGeometry::SampleSpline(Curve, 100.f, CurveFrames);
		const float CurveLength = CurveFrames.Num() > 0 ? CurveFrames.Last().Dist : 0.f;
		const int32 CurveCost = FMath::Max(1, FMath::RoundToInt(RoadA->GetCostMultiplier() * CurveLength / FMath::Max(RoadA->GetLength(), 1.f)));
		const TSubclassOf<ARMBuildableSpline> CurveClass = RoadA->GetClass();
		const TSubclassOf<UFGRecipe> CurveRecipe = RoadA->GetBuiltWithRecipe();
		SetGrabbedRoadsHidden(false);

		// Each road is cut like the Cut tool does (its tracks and vehicle paths split there too); the piece at the
		// joint goes, with what rides on it.
		auto CutBack = [](ARMBuildableRoad* Road, int32 JointEnd, float CutDist) -> ARMBuildableSpline*
		{
			ARMBuildableSpline* Second = nullptr;
			ARMBuildableSpline* First = Road->CutAt(CutDist, &Second);
			if (!First || !Second)
			{
				return nullptr;
			}
			RMSmoothJoint::RemovePiece(JointEnd == 1 ? Second : First);
			return JointEnd == 1 ? First : Second;
		};
		ARMBuildableSpline* KeptA = CutBack(RoadA, mEndA, A.CutDist);
		ARMBuildableSpline* KeptB = KeptA ? CutBack(RoadB, mEndB, B.CutDist) : nullptr;
		if (KeptA && KeptB)
		{
			// Built from the old corner outwards.
			NewCurve = KeptA->SpawnLike(CurveClass, CurveRecipe, Curve, CurveCost, ERMRevealFrom::NearestPoint, mJoint);
			UE_LOG(LogRoadMaker, Verbose, TEXT("Smooth Joint: %s and %s cut back %.0f cm, curve %s (%.0f cm)"), *RoadA->GetName(), *RoadB->GetName(),
				mSetback, *GetNameSafe(NewCurve), CurveLength);
		}
		else
		{
			UE_LOG(LogRoadMaker, Warning, TEXT("Smooth Joint: could not cut %s / %s back by %.0f cm"), *GetNameSafe(RoadA), *GetNameSafe(RoadB), mSetback);
		}
	}
	mRoadsHidden = false;
	mRoadA = nullptr;
	mRoadB = nullptr;
	mStep = 0;
	for (UProceduralMeshComponent* Mesh : { mPreviewMesh, mPreviewMeshB, mPreviewMeshC, mCircleMesh })
	{
		Mesh->ClearAllMeshSections();
	}
	return NewCurve;
}
