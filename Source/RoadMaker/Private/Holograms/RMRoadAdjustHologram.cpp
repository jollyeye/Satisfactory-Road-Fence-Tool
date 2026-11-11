#include "Holograms/RMRoadAdjustHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Holograms/RMHologramLook.h"
#include "Buildables/RMBuildableRoad.h"
#include "RMRoadUtils.h"
#include "FGRecipe.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	/** Rise per horizontal distance of a direction. */
	float AdjustSlopeOf(const FVector& Dir)
	{
		const float Flat = FVector(Dir.X, Dir.Y, 0.f).Size();
		return Flat > KINDA_SMALL_NUMBER ? Dir.Z / Flat : 0.f;
	}

	void SetAdjustTangentSlope(FVector& Tangent, float Slope)
	{
		Tangent.Z = FVector(Tangent.X, Tangent.Y, 0.f).Size() * Slope;
	}
}

ARMRoadAdjustHologram::ARMRoadAdjustHologram()
{
	mPreviewMeshB = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PreviewMeshB"));
	mCircleMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("EndCircles"));
	for (UProceduralMeshComponent* Mesh : { mPreviewMeshB, mCircleMesh })
	{
		if (RootComponent)
		{
			Mesh->SetupAttachment(RootComponent);
		}
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCastShadow(false);
	}
}

void ARMRoadAdjustHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMRoadAdjustHologram, mRoadA);
	DOREPLIFETIME(ARMRoadAdjustHologram, mRoadB);
	DOREPLIFETIME(ARMRoadAdjustHologram, mEndA);
	DOREPLIFETIME(ARMRoadAdjustHologram, mEndB);
	DOREPLIFETIME(ARMRoadAdjustHologram, mJoint);
	DOREPLIFETIME(ARMRoadAdjustHologram, mJointDir);
}

float ARMRoadAdjustHologram::GetSurfaceOffset() const
{
	return mRoadA ? mRoadA->GetRoadHeight() : 0.f;
}

FVector ARMRoadAdjustHologram::GetPlacedPointAxis() const
{
	FVector Dir = mJointDir;
	Dir.Z = 0.f;
	return Dir.Normalize() ? Dir : FVector::ForwardVector;
}

// ---------------------------------------------------------------------------------------------
// Step 0: the circles
// ---------------------------------------------------------------------------------------------

void ARMRoadAdjustHologram::SetGrabbedRoadsHidden(bool bHidden)
{
	mRoadsHidden = bHidden;
	for (ARMBuildableRoad* Road : { mRoadA, mRoadB })
	{
		if (IsValid(Road) && !Road->GetIsDismantled())
		{
			Road->SetActorHiddenInGame(bHidden);
		}
	}
}

void ARMRoadAdjustHologram::Destroyed()
{
	// Cancelled (or switched tool): the roads come back.
	if (mRoadsHidden)
	{
		SetGrabbedRoadsHidden(false);
	}
	Super::Destroyed();
}

void ARMRoadAdjustHologram::RefreshEnds()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	if (Now - mEndsTime < 0.5)
	{
		return;
	}
	mEndsTime = Now;
	APlayerController* PC = World->GetFirstPlayerController();
	const FVector Camera = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : GetActorLocation();
	mCircleBase = Camera;

	struct FRaw
	{
		ARMBuildableRoad* Road;
		int32 EndIndex;
		FVector Location;
	};
	TArray<FRaw> Raw;
	for (TActorIterator<ARMBuildableRoad> It(World); It; ++It)
	{
		ARMBuildableRoad* Road = *It;
		if (Road->GetIsDismantled() || Road->IsActorBeingDestroyed() || Road->IsHidden()
			|| FVector::Dist(Road->GetActorLocation(), Camera) > mShowRadius + Road->GetLength())
		{
			continue;
		}
		for (int32 E = 0; E < 2; ++E)
		{
			FVector Loc, Out;
			Road->GetEndWorld(E, Loc, Out);
			if (FVector::Dist(Loc, Camera) <= mShowRadius)
			{
				Raw.Add({ Road, E, Loc });
			}
		}
	}

	mEnds.Reset();
	for (int32 I = 0; I < Raw.Num(); ++I)
	{
		// Roads meeting this end at its middle (end to end).
		int32 Partners = 0;
		int32 Other = INDEX_NONE;
		for (int32 J = 0; J < Raw.Num(); ++J)
		{
			if (J != I && Raw[J].Road != Raw[I].Road && FVector::Dist(Raw[J].Location, Raw[I].Location) < RMRoadUtils::JoinTolerance)
			{
				++Partners;
				Other = J;
			}
		}
		if (Partners >= 2)
		{
			continue; // three or more roads at one point: not moved
		}
		if (Partners == 0 && RMRoadUtils::IsEndOccupied(World, Raw[I].Road, Raw[I].EndIndex))
		{
			continue; // joined to an intersection: stays
		}
		FEnd End;
		End.Road = Raw[I].Road;
		End.EndIndex = Raw[I].EndIndex;
		End.Location = Raw[I].Location;
		if (Other != INDEX_NONE)
		{
			End.Partner = Raw[Other].Road;
			End.PartnerEnd = Raw[Other].EndIndex;
		}
		mEnds.Add(End);
	}
	mDrawnAimed = -2; // redraw
}

int32 ARMRoadAdjustHologram::FindAimedEnd(const FVector& Aim) const
{
	int32 Best = INDEX_NONE;
	float BestDist = mPickRadius;
	for (int32 I = 0; I < mEnds.Num(); ++I)
	{
		const float D = FVector::Dist(mEnds[I].Location, Aim);
		if (D <= BestDist)
		{
			BestDist = D;
			Best = I;
		}
	}
	return Best;
}

void ARMRoadAdjustHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	if (mStep == 0)
	{
		RefreshEnds();
		mAimed = FindAimedEnd(hitResult.Location);
		SetActorLocationAndRotation(mCircleBase, FRotator::ZeroRotator);
		if (mAimed != mDrawnAimed)
		{
			RebuildCircles();
		}
		return;
	}
	if (TrySnapToRoadEnd(hitResult))
	{
		return;
	}
	mEndSnapped = false;
	mEnd = hitResult.Location + FVector(0.f, 0.f, GetSurfaceOffset());
	UpdateCurve();
}

bool ARMRoadAdjustHologram::TrySnapToRoadEnd(const FHitResult& hitResult)
{
	if (mStep == 0 || !mRoadA)
	{
		return false;
	}
	// One road: a free road end (or intersection mouth) near the aim. Two roads moved together do not
	// snap to road ends (a third road would meet them at one point).
	if (!mRoadB)
	{
		TArray<FRMRoadEndRef> Ends;
		RMRoadUtils::FindFreeEnds(GetWorld(), hitResult.Location, mSnapRadius, 4, nullptr, Ends);
		for (const FRMRoadEndRef& End : Ends)
		{
			if (End.Road == mRoadA)
			{
				continue;
			}
			mEnd = End.Location;
			mJointDir = End.Outward * -1.f; // road A arrives travelling into that end
			mEndSnapped = true;
			UpdateCurve();
			return true;
		}
	}
	return TrySnapToFoundation(hitResult);
}

bool ARMRoadAdjustHologram::TrySnapToFoundation(const FHitResult& hitResult)
{
	FTransform Piece;
	FBox Box;
	if (!RMRoadUtils::GetFoundationPiece(hitResult, Piece, Box))
	{
		return false;
	}
	const FVector Local = Piece.InverseTransformPosition(hitResult.Location);
	const FVector Scale = Piece.GetScale3D().GetAbs();
	constexpr float Grid = 400.f;

	if (mRoadB)
	{
		// Two roads: the joint on the 4 m grid of the top face, resting on it.
		const float StepX = Grid / FMath::Max(Scale.X, KINDA_SMALL_NUMBER);
		const float StepY = Grid / FMath::Max(Scale.Y, KINDA_SMALL_NUMBER);
		const float X = FMath::Clamp(Box.Min.X + FMath::RoundToFloat((Local.X - Box.Min.X) / StepX) * StepX, Box.Min.X, Box.Max.X);
		const float Y = FMath::Clamp(Box.Min.Y + FMath::RoundToFloat((Local.Y - Box.Min.Y) / StepY) * StepY, Box.Min.Y, Box.Max.Y);
		mEnd = Piece.TransformPosition(FVector(X, Y, Box.Max.Z)) + FVector(0.f, 0.f, GetSurfaceOffset());
		mEndSnapped = false;
		UpdateCurve();
		return true;
	}

	// One road: nearest edge of the top face.
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
	const float AlongScale = bAlongY ? Scale.Y : Scale.X;
	const float Step = Grid / FMath::Max(AlongScale, KINDA_SMALL_NUMBER);
	const float Snapped = FMath::Clamp(FMath::RoundToFloat(Along / Step) * Step, -Half, Half);

	FVector LocalPoint, LocalNormal;
	switch (Edge)
	{
	case 0:  LocalPoint = FVector(Box.Max.X, Center.Y + Snapped, Box.Max.Z); LocalNormal = FVector(1, 0, 0); break;
	case 1:  LocalPoint = FVector(Box.Min.X, Center.Y + Snapped, Box.Max.Z); LocalNormal = FVector(-1, 0, 0); break;
	case 2:  LocalPoint = FVector(Center.X + Snapped, Box.Max.Y, Box.Max.Z); LocalNormal = FVector(0, 1, 0); break;
	default: LocalPoint = FVector(Center.X + Snapped, Box.Min.Y, Box.Max.Z); LocalNormal = FVector(0, -1, 0); break;
	}
	FVector Outward = Piece.TransformVectorNoScale(LocalNormal);
	Outward.Z = 0.f;
	if (!Outward.Normalize())
	{
		return false;
	}
	mEnd = Piece.TransformPosition(LocalPoint);
	mJointDir = -Outward; // the road arrives travelling into the foundation
	mEndSnapped = true;
	UpdateCurve();
	return true;
}

bool ARMRoadAdjustHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	if (mStep == 0)
	{
		if (!mEnds.IsValidIndex(mAimed) || !mEnds[mAimed].Road.IsValid())
		{
			return false; // nothing picked yet
		}
		const FEnd& End = mEnds[mAimed];
		mRoadA = End.Road.Get();
		mEndA = End.EndIndex;
		mRoadB = End.Partner.Get();
		mEndB = End.PartnerEnd;
		mStep = 1;
		mScrollAtStepStart = GetScrollRotateValue();
		mEnd = End.Location;
		mEndSnapped = false;
		SetGrabbedRoadsHidden(true);
		UpdateCurve();
		ResetNudge();
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// Step 1: the new curves
// ---------------------------------------------------------------------------------------------

void ARMRoadAdjustHologram::UpdateCurve()
{
	if (mStep == 0 || !mRoadA)
	{
		return;
	}
	mJoint = mEnd;
	if (!mEndSnapped)
	{
		// The joint points along the chord (from the far end of road A to the far end of road B, or to
		// the joint when only one road moves), turned by the mouse wheel.
		FVector FarA, OutA;
		mRoadA->GetEndWorld(1 - mEndA, FarA, OutA);
		FVector Target = mEnd;
		if (mRoadB)
		{
			FVector FarB, OutB;
			mRoadB->GetEndWorld(1 - mEndB, FarB, OutB);
			Target = FarB;
		}
		FVector Dir = FVector(Target.X - FarA.X, Target.Y - FarA.Y, 0.f).GetSafeNormal();
		if (Dir.IsNearlyZero())
		{
			FVector Loc, Out;
			mRoadA->GetEndWorld(mEndA, Loc, Out);
			Dir = FVector(Out.X, Out.Y, 0.f).GetSafeNormal();
		}
		mJointDir = Dir.RotateAngleAxis(GetStepScrollYaw(), FVector::UpVector);
	}
	SetActorLocationAndRotation(mJoint, FRotator::ZeroRotator);
	RebuildRoadPreviews();
	RebuildCircles();
}

bool ARMRoadAdjustHologram::ComputeCurves(TArray<FRMSplinePoint>& OutA, TArray<FRMSplinePoint>& OutB) const
{
	OutA.Reset();
	OutB.Reset();
	if (!mRoadA)
	{
		return false;
	}
	auto Curve = [this](const ARMBuildableRoad* Road, int32 GrabbedEnd, const FVector& OutwardAtJoint, TArray<FRMSplinePoint>& Out)
	{
		// The far end keeps its place and direction; the grabbed end goes to the joint. The road keeps
		// its own direction of travel (start to end).
		FVector FarLoc, FarOutward;
		Road->GetEndWorld(1 - GrabbedEnd, FarLoc, FarOutward);
		// Smooth: the height eases out of the far end with the slope it had and into the joint with the
		// slope of what it snaps to (level when free), like the Smooth build mode.
		FVector StartDir, EndDir;
		if (GrabbedEnd == 1)
		{
			StartDir = -FarOutward;
			EndDir = OutwardAtJoint;
			RMGeometry::MakeQuadratic(FarLoc, StartDir, mJoint, EndDir, Out);
		}
		else
		{
			StartDir = -OutwardAtJoint;
			EndDir = FarOutward;
			RMGeometry::MakeQuadratic(mJoint, StartDir, FarLoc, EndDir, Out);
		}
		if (Out.Num() >= 2)
		{
			SetAdjustTangentSlope(Out[0].Tangent, AdjustSlopeOf(StartDir));
			SetAdjustTangentSlope(Out.Last().Tangent, AdjustSlopeOf(EndDir));
		}
	};
	Curve(mRoadA, mEndA, mJointDir, OutA);
	if (mRoadB)
	{
		Curve(mRoadB, mEndB, -mJointDir, OutB);
	}
	return OutA.Num() >= 2 && (!mRoadB || OutB.Num() >= 2);
}

void ARMRoadAdjustHologram::RebuildRoadPreviews()
{
	mPreviewMesh->ClearAllMeshSections();
	mPreviewMeshB->ClearAllMeshSections();
	TArray<FRMSplinePoint> A, B;
	if (!ComputeCurves(A, B))
	{
		return;
	}
	auto Build = [this](const ARMBuildableRoad* Road, const TArray<FRMSplinePoint>& Points, UProceduralMeshComponent* Mesh)
	{
		TArray<FRMFrame> Frames;
		RMGeometry::SampleSpline(Points, 100.f, Frames);
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
	Build(mRoadA, A, mPreviewMesh);
	if (mRoadB)
	{
		Build(mRoadB, B, mPreviewMeshB);
	}
	UpdateHologramLook();
	ApplyLookToSecondRoad();
}

void ARMRoadAdjustHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state);
	ApplyLookToSecondRoad();
}

void ARMRoadAdjustHologram::ApplyLookToSecondRoad()
{
	UMaterialInterface* Material = (mPlacementMaterialState == EHologramMaterialState::HMS_ERROR && mInvalidPlacementMaterial)
		? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
	RMHologramLook::Apply(mPreviewMeshB, Material, GetStencilForHologramMaterialState(mPlacementMaterialState));
}

void ARMRoadAdjustHologram::CheckValidPlacement()
{
	if (mStep == 0 || !mRoadA)
	{
		if (!mEnds.IsValidIndex(mAimed))
		{
			AddConstructDisqualifier(URMCD_AimAtRoadEnd::StaticClass());
		}
		return;
	}
	TArray<FRMSplinePoint> A, B;
	if (!ComputeCurves(A, B))
	{
		AddConstructDisqualifier(URMCD_TooShort::StaticClass());
		return;
	}
	bool bShort = false, bLong = false, bTight = false, bSteep = false;
	auto Check = [&](const ARMBuildableRoad* Road, const TArray<FRMSplinePoint>& Points)
	{
		TArray<FRMFrame> Frames;
		RMGeometry::SampleSpline(Points, 50.f, Frames);
		const FRMSplineStats Stats = RMGeometry::Analyze(Frames);
		bShort |= Stats.Length < Road->mMinLength;
		bLong |= Stats.Length > Road->mMaxLength;
		bTight |= Stats.MinTurnRadius < Road->GetEffectiveMinTurnRadius() * 0.95f;
		bSteep |= Stats.MaxGradeDegrees > Road->mMaxGradeDegrees;
	};
	Check(mRoadA, A);
	if (mRoadB)
	{
		Check(mRoadB, B);
	}
	if (bShort)
	{
		AddConstructDisqualifier(URMCD_TooShort::StaticClass());
	}
	if (bLong)
	{
		AddConstructDisqualifier(URMCD_TooLong::StaticClass());
	}
	if (bTight)
	{
		AddConstructDisqualifier(URMCD_TurnTooTight::StaticClass());
	}
	if (bSteep)
	{
		AddConstructDisqualifier(URMCD_TooSteep::StaticClass());
	}
}

AActor* ARMRoadAdjustHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	ARMBuildableSpline* First = nullptr;
	TArray<FRMSplinePoint> A, B;
	if (mRoadA && !mRoadA->GetIsDismantled() && !(mRoadB && mRoadB->GetIsDismantled()) && ComputeCurves(A, B))
	{
		auto Replace = [](ARMBuildableRoad* Road, const TArray<FRMSplinePoint>& Points) -> ARMBuildableSpline*
		{
			TArray<FRMFrame> Frames;
			RMGeometry::SampleSpline(Points, 100.f, Frames);
			const float NewLength = Frames.Num() > 0 ? Frames.Last().Dist : 0.f;
			const int32 Cost = FMath::Max(1, FMath::RoundToInt(Road->GetCostMultiplier() * NewLength / FMath::Max(Road->GetLength(), 1.f)));
			return Road->SpawnLike(Road->GetClass(), Road->GetBuiltWithRecipe(), Points, Cost);
		};
		ARMBuildableSpline* NewA = Replace(mRoadA, A);
		ARMBuildableSpline* NewB = mRoadB ? Replace(mRoadB, B) : nullptr;
		if (NewA && (!mRoadB || NewB))
		{
			mRoadA->DismantleReplaced();
			if (mRoadB)
			{
				mRoadB->DismantleReplaced();
			}
			First = NewA;
			if (NewB)
			{
				out_children.Add(NewB);
			}
		}
		else
		{
			if (NewA)
			{
				NewA->Destroy();
			}
			if (NewB)
			{
				NewB->Destroy();
			}
			SetGrabbedRoadsHidden(false);
		}
	}
	mRoadsHidden = false;
	mRoadA = nullptr;
	mRoadB = nullptr;
	mPreviewMesh->ClearAllMeshSections();
	mPreviewMeshB->ClearAllMeshSections();
	mCircleMesh->ClearAllMeshSections();
	return First;
}

// ---------------------------------------------------------------------------------------------
// Circles
// ---------------------------------------------------------------------------------------------

void ARMRoadAdjustHologram::RebuildCircles()
{
	mCircleMesh->ClearAllMeshSections();
	mDrawnAimed = mAimed;
	const FVector Base = GetActorLocation();

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FLinearColor> Colors;
	auto Disc = [&](const FVector& Centre, float Radius, const FLinearColor& Color)
	{
		const int32 First = Vertices.Num();
		Vertices.Add(Centre - Base);
		Colors.Add(Color);
		constexpr int32 Segments = 40;
		for (int32 I = 0; I < Segments; ++I)
		{
			const float A = 2.f * PI * I / Segments;
			Vertices.Add(Centre + FVector(FMath::Cos(A), FMath::Sin(A), 0.f) * Radius - Base);
			Colors.Add(Color);
		}
		for (int32 I = 0; I < Segments; ++I)
		{
			const int32 A = First + 1 + I;
			const int32 B = First + 1 + (I + 1) % Segments;
			Triangles.Append({ First, A, B, First, B, A }); // both sides
		}
	};

	if (mStep == 0)
	{
		for (int32 I = 0; I < mEnds.Num(); ++I)
		{
			const bool bAimed = I == mAimed;
			const FVector C = mEnds[I].Location + FVector(0.f, 0.f, 12.f);
			const float R = bAimed ? 300.f : 220.f;
			Disc(C, R + 35.f, FLinearColor(0.f, 0.f, 0.f));
			Disc(C + FVector(0.f, 0.f, 1.f), R, bAimed ? FLinearColor(1.f, 0.8f, 0.1f) : FLinearColor(1.f, 0.35f, 0.f));
		}
	}
	else
	{
		Disc(mJoint + FVector(0.f, 0.f, 12.f), 335.f, FLinearColor(0.f, 0.f, 0.f));
		Disc(mJoint + FVector(0.f, 0.f, 13.f), 300.f, FLinearColor(1.f, 0.35f, 0.f));

		// The grabbed roads are hidden (their collision stays, nobody falls): a wireframe marks them.
		APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		if (PC && PC->PlayerCameraManager)
		{
			int32 ViewW = 0, ViewH = 0;
			PC->GetViewportSize(ViewW, ViewH);
			const FVector Camera = PC->PlayerCameraManager->GetCameraLocation();
			const float WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);
			const FLinearColor WireColor(0.55f, 0.85f, 1.f);
			auto Line = [&](FVector A, FVector B)
			{
				const float Pixels = 3.f;
				const FVector Dir = (B - A).GetSafeNormal();
				const float HA = Pixels * WorldPerPixel * FMath::Max(FVector::Dist(A, Camera), 10.f) * 0.5f;
				const float HB = Pixels * WorldPerPixel * FMath::Max(FVector::Dist(B, Camera), 10.f) * 0.5f;
				const FVector SA = FVector::CrossProduct(Dir, Camera - A).GetSafeNormal();
				const FVector SB = FVector::CrossProduct(Dir, Camera - B).GetSafeNormal();
				const int32 I = Vertices.Num();
				for (const FVector& P : { A - SA * HA, A + SA * HA, B + SB * HB, B - SB * HB })
				{
					Vertices.Add(P - Base);
					Colors.Add(WireColor);
				}
				Triangles.Append({ I, I + 1, I + 2, I, I + 2, I + 3 });
			};
			for (const ARMBuildableRoad* Road : { mRoadA, mRoadB })
			{
				if (!IsValid(Road))
				{
					continue;
				}
				TArray<FRMFrame> Frames;
				Road->GetWorldFrames(Frames, 200.f);
				const float Half = Road->GetRoadWidth() * 0.5f;
				auto Edge = [&](const FRMFrame& F, float Side) { return F.Pos + F.Right * (Half * Side) + F.Up * 4.f; };
				float NextRib = 0.f;
				for (int32 I = 0; I < Frames.Num(); ++I)
				{
					if (I > 0)
					{
						Line(Edge(Frames[I - 1], -1.f), Edge(Frames[I], -1.f));
						Line(Edge(Frames[I - 1], 1.f), Edge(Frames[I], 1.f));
					}
					// A rib across every 4 m (and at both ends).
					if (Frames[I].Dist >= NextRib || I == Frames.Num() - 1)
					{
						Line(Edge(Frames[I], -1.f), Edge(Frames[I], 1.f));
						NextRib = Frames[I].Dist + 400.f;
					}
				}
			}
		}
	}
	if (Vertices.Num() == 0)
	{
		return;
	}
	static UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay"));
	mCircleMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, TArray<FVector>(), TArray<FVector2D>(), Colors, TArray<FProcMeshTangent>(), false);
	if (Material)
	{
		mCircleMesh->SetMaterial(0, Material);
	}
	mCircleMesh->SetRenderCustomDepth(false);
}
