#include "Holograms/RMRoadHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMHologramLook.h"
#include "Buildables/RMBuildableRoad.h"
#include "Buildables/RMBuildableIntersection.h"
#include "RMRoadUtils.h"
#include "Equipment/FGBuildGunBuild.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "ProceduralMeshComponent.h"
#include "Buildables/FGBuildable.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Net/UnrealNetwork.h"
#include "Algo/Reverse.h"

const ARMBuildableRoad* ARMRoadHologram::GetRoadDefaults() const
{
	// mBuildClass is the buildable class this hologram was spawned for (set by the game).
	return mBuildClass ? mBuildClass->GetDefaultObject<ARMBuildableRoad>() : GetDefault<ARMBuildableRoad>();
}

float ARMRoadHologram::GetSurfaceOffset() const
{
	// The bottom of the slab rests on what you aim at; the spline runs along the top.
	return GetRoadDefaults()->GetRoadHeight();
}

int32 ARMRoadHologram::GetCostWidthFactor() const
{
	return GetRoadDefaults()->mWidthInFoundations;
}

void ARMRoadHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMRoadHologram, mBesideRoad);
}

ARMRoadHologram::ARMRoadHologram()
{
	mDirectionArrow = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("DirectionArrow"));
	mDirectionArrow->SetupAttachment(mPreviewMesh);
	mDirectionArrow->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mDirectionArrow->SetCastShadow(false);
}

void ARMRoadHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state);
	ApplyArrowMaterial();
}

void ARMRoadHologram::ApplyArrowMaterial()
{
	// Its own material: the game may give every mesh of a hologram the hologram material.
	// Kept loaded for good: a plain static pointer would dangle once the garbage collector frees it.
	static UMaterialInterface* Overlay = [] { UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay")); if (M) { M->AddToRoot(); } return M; }();
	for (int32 Section = 0; Overlay && Section < mDirectionArrow->GetNumSections(); ++Section)
	{
		mDirectionArrow->SetMaterial(Section, Overlay);
	}
	mDirectionArrow->SetRenderCustomDepth(false);
}

void ARMRoadHologram::RebuildDirectionArrow(const TArray<FRMFrame>& Frames)
{
	mDirectionArrow->ClearAllMeshSections();
	// Only on the stub before the first click: once the start is placed (or the road is placed beside
	// another one) the preview is just the road in its material.
	if (Frames.Num() < 2 || !IsFadingStub())
	{
		return;
	}
	// The intersection tool's arrow (1.5 times its size), lying flat on the road's top in the middle of the road
	// (hologram space), tip towards the road's end.
	const FRMFrame& F = Frames[Frames.Num() / 2];
	const FVector Fwd = F.Fwd.GetSafeNormal();
	const FVector Side = F.Right.GetSafeNormal();
	const FVector Base = F.Pos + F.Up * 5.f; // a hair above the road top
	const float K = 1.5f; // 1.5 times the intersection tool's arrow: 6 m long
	const FVector Tail = Base - Fwd * 200.f * K;
	const FVector HeadBase = Base + Fwd * 50.f * K;
	const FVector Tip = Base + Fwd * 200.f * K;
	TArray<FVector> V = {
		Tip, HeadBase + Side * 110.f * K, HeadBase - Side * 110.f * K,                                      // head
		HeadBase - Side * 35.f * K, HeadBase + Side * 35.f * K, Tail + Side * 35.f * K, Tail - Side * 35.f * K }; // shaft
	TArray<int32> T;
	// Wound so the faces look up (both windings: seen from above whatever the frame's handedness).
	T.Append({ 0, 1, 2, 0, 2, 1, 3, 4, 5, 3, 5, 6, 3, 5, 4, 3, 6, 5 });
	TArray<FLinearColor> C;
	C.Init(FLinearColor::White, V.Num());
	TArray<FVector> N;
	N.Init(F.Up, V.Num());
	mDirectionArrow->CreateMeshSection_LinearColor(0, V, T, N, TArray<FVector2D>(), C, TArray<FProcMeshTangent>(), false);

	// Outline of the start cap (the road's end face where it starts): light blue, 4 pixels wide on screen.
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC && PC->PlayerCameraManager)
	{
		int32 ViewW = 0, ViewH = 0;
		PC->GetViewportSize(ViewW, ViewH);
		const FTransform& ToWorld = GetActorTransform();
		const FVector Camera = ToWorld.InverseTransformPosition(PC->PlayerCameraManager->GetCameraLocation());
		const float WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);
		const ARMBuildableRoad* Road = GetRoadDefaults();
		const FRMFrame& S = Frames[0];
		const FVector Right = S.Right.GetSafeNormal();
		const FVector Up = S.Up.GetSafeNormal();
		const FVector Back = -S.Fwd.GetSafeNormal() * 2.f; // a hair outside the face
		const float Half = Road->GetRoadWidth() * 0.5f;
		const FVector TL = S.Pos - Right * Half + Back, TR = S.Pos + Right * Half + Back;
		const FVector BL = TL - Up * Road->GetRoadHeight(), BR = TR - Up * Road->GetRoadHeight();
		TArray<FVector> CV;
		TArray<int32> CT;
		TArray<FLinearColor> CC;
		const FLinearColor LightBlue(0.35f, 0.75f, 1.f);
		auto Line = [&](FVector A, FVector B)
		{
			const FVector Dir = (B - A).GetSafeNormal();
			const float HA = 2.f * WorldPerPixel * FMath::Max(FVector::Dist(A, Camera), 10.f); // half of 4 px
			const float HB = 2.f * WorldPerPixel * FMath::Max(FVector::Dist(B, Camera), 10.f);
			A -= Dir * HA;
			B += Dir * HB;
			const FVector SA = FVector::CrossProduct(Dir, Camera - A).GetSafeNormal();
			const FVector SB = FVector::CrossProduct(Dir, Camera - B).GetSafeNormal();
			const int32 I = CV.Num();
			CV.Append({ A - SA * HA, A + SA * HA, B + SB * HB, B - SB * HB });
			CC.Append({ LightBlue, LightBlue, LightBlue, LightBlue });
			CT.Append({ I, I + 1, I + 2, I, I + 2, I + 3 });
		};
		Line(TL, TR);
		Line(TR, BR);
		Line(BR, BL);
		Line(BL, TL);
		mDirectionArrow->CreateMeshSection_LinearColor(1, CV, CT, TArray<FVector>(), TArray<FVector2D>(), CC, TArray<FProcMeshTangent>(), false);
	}
	ApplyArrowMaterial();
}

bool ARMRoadHologram::TrySnapBesideRoad(const FHitResult& hitResult)
{
	// Only for a new road (before the first click): the whole road is placed in one click.
	if (mStep != 0)
	{
		return false;
	}
	if (ARMBuildableIntersection* Intersection = Cast<ARMBuildableIntersection>(hitResult.GetActor()))
	{
		mLook = Intersection->GetLook();
		return SnapBesideCurb(Intersection, hitResult.Location);
	}
	ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(hitResult.GetActor());
	if (!Road)
	{
		return false;
	}
	mLook = Road->GetLook();
	TArray<FRMFrame> Frames;
	Road->GetWorldFrames(Frames, 50.f);
	if (Frames.Num() < 2)
	{
		return false;
	}
	// Which side: where the crosshair is relative to the road's centre line.
	int32 Closest = 0;
	float Best = TNumericLimits<float>::Max();
	for (int32 I = 0; I < Frames.Num(); ++I)
	{
		const float D = FVector::DistSquared2D(Frames[I].Pos, hitResult.Location);
		if (D < Best)
		{
			Best = D;
			Closest = I;
		}
	}
	const float Side = FVector::DotProduct(hitResult.Location - Frames[Closest].Pos, Frames[Closest].Right) >= 0.f ? 1.f : -1.f;
	const float Offset = Side * (Road->GetRoadWidth() + GetRoadDefaults()->GetRoadWidth()) * 0.5f;

	// The parallel of the whole road, edge to edge, flush with its top.
	TArray<FRMSplinePoint> Points;
	RMGeometry::OffsetCurve(Frames, Offset, 400.f, Points);
	if (Points.Num() < 2)
	{
		return false;
	}
	return PlaceBeside(Points);
}

bool ARMRoadHologram::SnapBesideCurb(ARMBuildableIntersection* Intersection, const FVector& AimPoint)
{
	TArray<TArray<FVector>> Curbs;
	Intersection->GetWorldCurbs(Curbs);
	int32 BestCurb = INDEX_NONE;
	float Best = TNumericLimits<float>::Max();
	for (int32 C = 0; C < Curbs.Num(); ++C)
	{
		for (const FVector& P : Curbs[C])
		{
			const float D = FVector::DistSquared2D(P, AimPoint);
			if (D < Best)
			{
				Best = D;
				BestCurb = C;
			}
		}
	}
	if (BestCurb == INDEX_NONE || Curbs[BestCurb].Num() < 2)
	{
		return false;
	}
	// Curbs run counter-clockwise, so the junction centre is on their +Right side: the new road goes
	// on the other side, its centre half its width away, flush with the slab top.
	const TArray<FVector>& Curb = Curbs[BestCurb];
	const float Half = GetRoadDefaults()->GetRoadWidth() * 0.5f;
	TArray<FVector> Centre;
	for (int32 I = 0; I < Curb.Num(); ++I)
	{
		const FVector Prev = Curb[FMath::Max(I - 1, 0)];
		const FVector Next = Curb[FMath::Min(I + 1, Curb.Num() - 1)];
		const FVector Fwd = FVector(Next.X - Prev.X, Next.Y - Prev.Y, 0.f).GetSafeNormal();
		const FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd).GetSafeNormal();
		Centre.Add(Curb[I] - Right * Half);
	}
	TArray<FRMSplinePoint> Points;
	RMGeometry::SplineThroughPoints(Centre, Points);
	return PlaceBeside(Points);
}

bool ARMRoadHologram::PlaceBeside(const TArray<FRMSplinePoint>& Points)
{
	if (Points.Num() < 2)
	{
		return false;
	}
	mBesideRoad = true;
	mStart = Points[0].Location;
	mStartDir = Points[0].Tangent.GetSafeNormal();
	mStartSnapped = false;
	mEnd = Points.Last().Location;
	mEndDir = Points.Last().Tangent.GetSafeNormal();
	mEndSnapped = false;
	SetWorldPoints(Points);
	return true;
}

bool ARMRoadHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	// A road beside a road is complete already: one click builds it.
	if (mStep == 0 && mBesideRoad)
	{
		return true;
	}
	return Super::DoMultiStepPlacement(isInputFromARelease);
}

bool ARMRoadHologram::TrySnapToRoadEnd(const FHitResult& hitResult)
{
	// Snap key held, aiming at a road: a parallel road beside it.
	if (mSnapToGuideLines && TrySnapBesideRoad(hitResult))
	{
		return true;
	}
	mBesideRoad = false;

	// Any free place at a road end near the aim point, whether you aim at the road or the ground next to
	// it: the end's centre, or, on an end wider than this road, a place across it in 8 m steps (joining
	// back the pieces of a split road beside a piece already attached).
	TArray<FRMRoadEndRef> Ends;
	RMRoadUtils::FindEndSlots(GetWorld(), hitResult.Location, mSnapRadius, GetRoadDefaults()->GetRoadWidth(), Ends);
	if (Ends.Num() == 0)
	{
		return TrySnapToFoundation(hitResult);
	}
	const FRMRoadEndRef& End = Ends[0];
	if (End.Road && (mStep == 0 || !mHasCopiedMarkings))
	{
		mMarkings = End.Road->GetMarkings();
		mLook = End.Road->GetLook();
		mHasCopiedMarkings = true;
	}

	if (mStep == 0)
	{
		mStart = End.Location;
		mStartDir = End.Outward;
		mStartSnapped = true;
	}
	else
	{
		// Do not let the second click snap back onto the end we started from.
		if (mStartSnapped && FVector::Dist(End.Location, mStart) < RMRoadUtils::JoinTolerance)
		{
			return false;
		}
		mEnd = End.Location;
		mEndDir = -End.Outward; // we arrive travelling into that road
		mEndSnapped = true;
	}
	UpdateCurve();
	return true;
}

void ARMRoadHologram::RebuildPreview()
{
	const ARMBuildableRoad* Road = GetRoadDefaults();
	TArray<FRMFrame> Frames;
	GetPreviewFrames(Frames, 100.f);
	mPreviewMesh->ClearAllMeshSections();
	if (Frames.Num() < 2)
	{
		mDirectionArrow->ClearAllMeshSections();
		return;
	}
	FRMMesh Top, Body;
	RMGeometry::BuildRoad(Frames, Road->GetRoadWidth(), Road->GetRoadHeight(), 400.f, mMarkings, Road->mEdgeLineInset, Top, Body);
	if (IsFadingStub())
	{
		// Vertex alpha: 1 at the start point, 0 at the far end of the stub (the hologram materials
		// dither it away). The stub runs along the hologram's X (its rotation is the start direction).
		const float Length = FMath::Max(GetStubLength(), 1.f);
		auto Fade = [Length](const FRMMesh& M)
		{
			TArray<FLinearColor> C;
			C.Reserve(M.Vertices.Num());
			for (const FVector& V : M.Vertices)
			{
				C.Add(FLinearColor(1.f, 1.f, 1.f, 1.f - FMath::Clamp(V.X / Length, 0.f, 1.f)));
			}
			return C;
		};
		const TArray<FLinearColor> TopFade = Fade(Top);
		const TArray<FLinearColor> BodyFade = Fade(Body);
		Top.ApplyToSection(mPreviewMesh, 0, false, &TopFade);
		Body.ApplyToSection(mPreviewMesh, 1, false, &BodyFade);
	}
	else
	{
		Top.ApplyToSection(mPreviewMesh, 0, false);
		Body.ApplyToSection(mPreviewMesh, 1, false);
	}
	RebuildDirectionArrow(Frames);
}

void ARMRoadHologram::UpdateHologramLook()
{
	const ARMBuildableRoad* Road = GetRoadDefaults();
	if (mPlacementMaterialState == EHologramMaterialState::HMS_ERROR || !Road || mPreviewMesh->GetNumSections() == 0)
	{
		Super::UpdateHologramLook(); // the game's red hologram
		return;
	}
	// The real surface in its look (copied from the road it snaps to), in the hologram version of the
	// material (fades with the vertex alpha); the game's outline still shows valid / warning.
	FLinearColor Primary, Secondary;
	Road->GetPaintColours(Primary, Secondary);
	mPreviewMesh->SetMaterial(0, RMHologramLook::HoloVersion(Road->GetLookMaterialFor(mLook, true, Road->mTopMaterial)));
	if (mPreviewMesh->GetNumSections() > 1)
	{
		mPreviewMesh->SetMaterial(1, RMHologramLook::HoloVersion(Road->GetLookMaterialFor(mLook, false, Road->mBodyMaterial)));
	}
	// All lines in the dark side line colour: a white middle line would run into the white direction arrow.
	mPreviewMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(Secondary.R, Secondary.G, Secondary.B, 1.0));
	mPreviewMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(Secondary.R, Secondary.G, Secondary.B, 1.0));
	RMHologramLook::Apply(mPreviewMesh, nullptr, GetStencilForHologramMaterialState(mPlacementMaterialState));
}

void ARMRoadHologram::CheckValidPlacement()
{
	// No Super call on purpose: the vanilla floor check ("Surface is too uneven") is meant for
	// box-shaped buildings and rejects spline pieces. Placement rules are checked below instead.

	const ARMBuildableRoad* Road = GetRoadDefaults();
	if (mBesideRoad)
	{
		// A parallel of a built road (judged before the click, it is built in one): no length, turn
		// or grade rules, only what would break the mesh (the inner edge folding over itself on a
		// bend tighter than half the width).
		TArray<FRMFrame> SideFrames;
		GetPreviewFrames(SideFrames, 50.f);
		if (RMGeometry::Analyze(SideFrames).MinTurnRadius < Road->GetRoadWidth() * 0.5f * 1.02f)
		{
			AddConstructDisqualifier(URMCD_TurnTooTight::StaticClass());
		}
		return;
	}

	if (mStep == 0)
	{
		return; // only the finished curve is judged
	}

	TArray<FRMFrame> Frames;
	GetPreviewFrames(Frames, 50.f);
	const FRMSplineStats Stats = RMGeometry::Analyze(Frames);

	if (Stats.Length < Road->mMinLength)
	{
		AddConstructDisqualifier(URMCD_TooShort::StaticClass());
	}
	if (Stats.Length > Road->mMaxLength)
	{
		AddConstructDisqualifier(URMCD_TooLong::StaticClass());
	}
	if (Stats.MinTurnRadius < Road->GetEffectiveMinTurnRadius() * 0.95f)
	{
		AddConstructDisqualifier(URMCD_TurnTooTight::StaticClass());
	}
	// Smooth mode allows steeper middles (its S-curve is steeper there than an even slope).
	const float MaxGrade = IsSmoothMode() ? FMath::Max(Road->mMaxGradeDegrees, 45.f) : Road->mMaxGradeDegrees;
	if (Stats.MaxGradeDegrees > MaxGrade)
	{
		AddConstructDisqualifier(URMCD_TooSteep::StaticClass());
	}
}

bool ARMRoadHologram::TrySnapToFoundation(const FHitResult& hitResult)
{
	AActor* Actor = hitResult.GetActor();
	UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(hitResult.GetComponent());
	if (!Actor || !Mesh || !Mesh->GetStaticMesh() || Cast<ARMBuildableProcedural>(Actor))
	{
		return false;
	}

	// Only buildings: classic buildable actors, or lightweight buildables (foundations since 1.0 are
	// rendered as instances owned by an abstract instance manager).
	const bool bBuilding = Actor->IsA<AFGBuildable>()
		|| Actor->GetClass()->GetName().Contains(TEXT("AbstractInstance"))
		|| Actor->GetClass()->GetName().Contains(TEXT("Lightweight"));
	if (!bBuilding)
	{
		return false;
	}

	// Transform of the exact piece that was hit: the instance for instanced meshes.
	FTransform PieceTransform = Mesh->GetComponentTransform();
	if (UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Mesh))
	{
		if (hitResult.Item == INDEX_NONE || !Instanced->GetInstanceTransform(hitResult.Item, PieceTransform, true))
		{
			return false;
		}
	}

	// Foundation-shaped: at least half a foundation wide each way, 0.5 to 4.5 m thick.
	const FBox Box = Mesh->GetStaticMesh()->GetBoundingBox();
	const FVector Size = Box.GetSize() * PieceTransform.GetScale3D().GetAbs();
	if (Size.X < 390.f || Size.Y < 390.f || Size.Z < 50.f || Size.Z > 450.f)
	{
		return false;
	}

	// Nearest edge of the top face, in the piece's local space.
	const FVector Local = PieceTransform.InverseTransformPosition(hitResult.Location);
	const FVector Center = Box.GetCenter();
	const float Dist[4] = { Box.Max.X - Local.X, Local.X - Box.Min.X, Box.Max.Y - Local.Y, Local.Y - Box.Min.Y };
	int32 Edge = 0;
	for (int32 I = 1; I < 4; ++I)
	{
		if (Dist[I] < Dist[Edge])
		{
			Edge = I;
		}
	}

	const bool bAlongY = Edge < 2; // X edges run along Y
	const float Half = bAlongY ? (Box.Max.Y - Box.Min.Y) * 0.5f : (Box.Max.X - Box.Min.X) * 0.5f;
	const float Along = bAlongY ? Local.Y - Center.Y : Local.X - Center.X;
	const float Scale = bAlongY ? FMath::Abs(PieceTransform.GetScale3D().Y) : FMath::Abs(PieceTransform.GetScale3D().X);
	const float Step = mFoundationEdgeStep / FMath::Max(Scale, KINDA_SMALL_NUMBER);
	const float Snapped = FMath::Clamp(FMath::RoundToFloat(Along / Step) * Step, -Half, Half);

	FVector LocalPoint, LocalNormal;
	switch (Edge)
	{
	case 0:  LocalPoint = FVector(Box.Max.X, Center.Y + Snapped, Box.Max.Z); LocalNormal = FVector(1, 0, 0); break;
	case 1:  LocalPoint = FVector(Box.Min.X, Center.Y + Snapped, Box.Max.Z); LocalNormal = FVector(-1, 0, 0); break;
	case 2:  LocalPoint = FVector(Center.X + Snapped, Box.Max.Y, Box.Max.Z); LocalNormal = FVector(0, 1, 0); break;
	default: LocalPoint = FVector(Center.X + Snapped, Box.Min.Y, Box.Max.Z); LocalNormal = FVector(0, -1, 0); break;
	}

	const FVector Point = PieceTransform.TransformPosition(LocalPoint);
	FVector Outward = PieceTransform.TransformVectorNoScale(LocalNormal);
	Outward.Z = 0.f;
	if (!Outward.Normalize())
	{
		return false;
	}

	if (mStep == 0)
	{
		mStart = Point;
		mStartDir = Outward;
		mStartSnapped = true;
	}
	else
	{
		if (FVector::Dist(Point, mStart) < 100.f)
		{
			return false;
		}
		mEnd = Point;
		mEndDir = -Outward; // arrive travelling into the foundation
		mEndSnapped = true;
	}
	UpdateCurve();
	return true;
}

void ARMRoadHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableRoad* Road = Cast<ARMBuildableRoad>(inBuildable))
	{
		Road->SetMarkings(mMarkings);
		Road->SetLook(mLook);
	}
}

void ARMRoadHologram::OnHologramLockStateChanged()
{
	Super::OnHologramLockStateChanged();
	mBesideBase = GetActorLocation();
	mBesideNudge = FVector::ZeroVector;
}

void ARMRoadHologram::SetHologramNudgeLocation()
{
	if (!mBesideRoad)
	{
		Super::SetHologramNudgeLocation();
		return;
	}
	if (IsHologramLocked())
	{
		SetActorLocation(mBesideBase + mBesideNudge); // the whole road: its points are relative to the hologram
	}
}

ENudgeFailReason ARMRoadHologram::NudgeHologram(const FVector& NudgeInput, const FHitResult& HitResult)
{
	if (!mBesideRoad || !IsHologramLocked())
	{
		return Super::NudgeHologram(NudgeInput, HitResult);
	}
	const FVector Step(FMath::Sign(NudgeInput.X), FMath::Sign(NudgeInput.Y), FMath::Sign(NudgeInput.Z));
	if (Step.IsNearlyZero())
	{
		return ENudgeFailReason::NFR_Disallowed;
	}
	// Forward / right are the world axes nearest to where the camera looks; Z is up / down.
	float CameraYaw = 0.f;
	if (const UWorld* World = GetWorld())
	{
		if (const APlayerController* PC = World->GetFirstPlayerController())
		{
			if (PC->PlayerCameraManager)
			{
				CameraYaw = PC->PlayerCameraManager->GetCameraRotation().Yaw;
			}
		}
	}
	const float AxisYaw = FMath::RoundToFloat(CameraYaw / 90.f) * 90.f;
	const FVector Forward = FRotator(0.f, AxisYaw, 0.f).Vector();
	const FVector Right = FRotator(0.f, AxisYaw + 90.f, 0.f).Vector();
	mBesideNudge += (Forward * Step.X + Right * Step.Y + FVector::UpVector * Step.Z) * GetNudgeDistance();
	SetHologramNudgeLocation();
	return ENudgeFailReason::NFR_Success;
}
