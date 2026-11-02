#include "Holograms/RMRoadHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableRoad.h"
#include "RMRoadUtils.h"
#include "ProceduralMeshComponent.h"
#include "Buildables/FGBuildable.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

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

bool ARMRoadHologram::TrySnapToRoadEnd(const FHitResult& hitResult)
{
	// Any free road end near the aim point, whether you aim at the road or the ground next to it.
	TArray<FRMRoadEndRef> Ends;
	RMRoadUtils::FindFreeEnds(GetWorld(), hitResult.Location, mSnapRadius, 1, nullptr, Ends);
	if (Ends.Num() == 0)
	{
		return TrySnapToFoundation(hitResult);
	}
	const FRMRoadEndRef& End = Ends[0];

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
		return;
	}
	FRMMesh Top, Body;
	RMGeometry::BuildRoad(Frames, Road->GetRoadWidth(), Road->GetRoadHeight(), 400.f, Top, Body);
	Top.ApplyToSection(mPreviewMesh, 0, false);
	Body.ApplyToSection(mPreviewMesh, 1, false);
}

void ARMRoadHologram::CheckValidPlacement()
{
	// No Super call on purpose: the vanilla floor check ("Surface is too uneven") is meant for
	// box-shaped buildings and rejects spline pieces. Placement rules are checked below instead.

	if (mStep == 0)
	{
		return; // only the finished curve is judged
	}

	const ARMBuildableRoad* Road = GetRoadDefaults();
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
	if (Stats.MinTurnRadius < Road->GetEffectiveMinTurnRadius())
	{
		AddConstructDisqualifier(URMCD_TurnTooTight::StaticClass());
	}
	if (Stats.MaxGradeDegrees > Road->mMaxGradeDegrees)
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
