#include "Holograms/RMIntersectionHologram.h"
#include "Holograms/RMDisqualifiers.h"
#include "Buildables/RMBuildableIntersection.h"
#include "Buildables/RMBuildableRoad.h"
#include "RMRoadUtils.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Holograms/RMHologramLook.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

ARMIntersectionHologram::ARMIntersectionHologram()
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

	mOverlayMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PickOverlay"));
	mOverlayMesh->SetupAttachment(mPreviewMesh);
	mOverlayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mOverlayMesh->SetCastShadow(false);
}

void ARMIntersectionHologram::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMIntersectionHologram, mLocalEnds);
	DOREPLIFETIME(ARMIntersectionHologram, mHeight);
	DOREPLIFETIME(ARMIntersectionHologram, mSideLines);
}

namespace
{
	bool SameEnd(const FRMRoadEndRef& A, const FRMRoadEndRef& B)
	{
		return A.Road == B.Road && A.Intersection == B.Intersection && A.EndIndex == B.EndIndex;
	}

	/**
	 * Ends lying side by side as one wider road (e.g. strips made by Split Road): facing the same way,
	 * on the same line across, at the same height, edge touching edge.
	 */
	bool IsectEndsSideBySide(const FRMRoadEndRef& A, const FRMRoadEndRef& B)
	{
		const FVector DirA = FVector(A.Outward.X, A.Outward.Y, 0.f).GetSafeNormal();
		const FVector DirB = FVector(B.Outward.X, B.Outward.Y, 0.f).GetSafeNormal();
		if (FVector::DotProduct(DirA, DirB) < 0.9994f) // within 2 degrees
		{
			return false;
		}
		const FVector Delta = B.Location - A.Location;
		const FVector Across = FVector::CrossProduct(FVector::UpVector, DirA);
		const float Tolerance = 50.f;
		return FMath::Abs(FVector::DotProduct(Delta, DirA)) < Tolerance
			&& FMath::Abs(Delta.Z) < Tolerance
			&& FMath::Abs(FMath::Abs(FVector::DotProduct(Delta, Across)) - (A.Width + B.Width) * 0.5f) < Tolerance;
	}

	/**
	 * How many of the intersection's road slots these ends use: ends side by side count as one road
	 * per 4 foundations of their total width (a road split in three still counts as one).
	 */
	/** For each end, the index of its side-by-side group's root end (the same for the whole group). */
	TArray<int32> IsectGroupEnds(const TArray<FRMRoadEndRef>& Ends)
	{
		TArray<int32> Group;
		for (int32 I = 0; I < Ends.Num(); ++I)
		{
			Group.Add(I);
		}
		auto Root = [&Group](int32 I)
		{
			while (Group[I] != I)
			{
				I = Group[I];
			}
			return I;
		};
		for (int32 I = 0; I < Ends.Num(); ++I)
		{
			for (int32 J = I + 1; J < Ends.Num(); ++J)
			{
				if (IsectEndsSideBySide(Ends[I], Ends[J]))
				{
					Group[Root(J)] = Root(I);
				}
			}
		}
		TArray<int32> Out;
		for (int32 I = 0; I < Ends.Num(); ++I)
		{
			Out.Add(Root(I));
		}
		return Out;
	}

	int32 IsectCountRoadSlots(const TArray<FRMRoadEndRef>& Ends)
	{
		const TArray<int32> Groups = IsectGroupEnds(Ends);
		TMap<int32, float> Widths;
		for (int32 I = 0; I < Ends.Num(); ++I)
		{
			Widths.FindOrAdd(Groups[I]) += Ends[I].Width;
		}
		int32 Slots = 0;
		const float MaxWidth = 4.f * RM::FoundationSize;
		for (const TPair<int32, float>& W : Widths)
		{
			Slots += FMath::Max(1, FMath::CeilToInt(W.Value / MaxWidth - 0.01f));
		}
		return Slots;
	}
}

void ARMIntersectionHologram::RefreshPicks()
{
	for (int32 I = mPicked.Num() - 1; I >= 0; --I)
	{
		TArray<FRMRoadEndRef> Near;
		RMRoadUtils::FindFreeEnds(GetWorld(), mPicked[I].Location, 200.f, 0, nullptr, Near);
		const FRMRoadEndRef* Same = Near.FindByPredicate([this, I](const FRMRoadEndRef& R) { return SameEnd(R, mPicked[I]); });
		if (Same)
		{
			mPicked[I] = *Same;
		}
		else
		{
			mPicked.RemoveAt(I);
		}
	}
}

void ARMIntersectionHologram::SetHologramLocationAndRotation(const FHitResult& hitResult)
{
	RefreshPicks();
	TArray<FRMRoadEndRef> Near;
	RMRoadUtils::FindFreeEnds(GetWorld(), hitResult.Location, mPickRadius, 1, nullptr, Near);
	mHasHover = Near.Num() > 0;
	if (mHasHover)
	{
		mHover = Near[0];
	}
	// With 2+ ends picked, aiming nearer the middle of the picked ends than the nearest end means
	// "build": on a small intersection every end is within the pick radius of the middle, so a click
	// in the middle used to toggle an end instead of confirming.
	if (mHasHover && mPicked.Num() >= 2)
	{
		FVector Middle = FVector::ZeroVector;
		for (const FRMRoadEndRef& R : mPicked)
		{
			Middle += R.Location / mPicked.Num();
		}
		if (FVector::Dist2D(hitResult.Location, Middle) < FVector::Dist2D(hitResult.Location, mHover.Location))
		{
			mHasHover = false;
		}
	}
	ApplyEnds(mPicked, hitResult.Location);
	RebuildOverlay();
}

bool ARMIntersectionHologram::DoMultiStepPlacement(bool isInputFromARelease)
{
	if (mHasHover)
	{
		// One toggle per click, even if the game reports the click twice (press and release).
		const double Now = FPlatformTime::Seconds();
		if (Now - mLastPickTime > 0.25)
		{
			mLastPickTime = Now;
			const int32 Index = mPicked.IndexOfByPredicate([this](const FRMRoadEndRef& R) { return SameEnd(R, mHover); });
			if (Index != INDEX_NONE)
			{
				mPicked.RemoveAt(Index);
			}
			else
			{
				// Ends side by side (a split road) use one road slot per 4 foundations of width.
				TArray<FRMRoadEndRef> WithHover = mPicked;
				WithHover.Add(mHover);
				if (IsectCountRoadSlots(WithHover) <= mMaxRoads
					&& !mPicked.ContainsByPredicate([this](const FRMRoadEndRef& R) { return FVector::Dist(R.Location, mHover.Location) > mPickMaxSpan; }))
				{
					mPicked.Add(mHover);
				}
			}
			mPickKey = 0; // redraw
		}
		return false;
	}
	return mPicked.Num() >= 2; // clicked away from the ends: build
}

void ARMIntersectionHologram::ApplyEnds(const TArray<FRMRoadEndRef>& Found, const FVector& FallbackLocation)
{
	uint32 Key = GetTypeHash(mPicked.Num()) + 1;
	for (const FRMRoadEndRef& R : mPicked)
	{
		Key = HashCombine(Key, HashCombine(GetTypeHash(R.Road), HashCombine(GetTypeHash(R.Intersection), GetTypeHash(R.EndIndex))));
	}
	const bool bPicksChanged = Key != mPickKey;
	mPickKey = Key;

	if (Found.Num() == 0)
	{
		SetActorLocationAndRotation(FallbackLocation, FRotator::ZeroRotator);
		mLocalEnds.Reset();
		RebuildPreview();
		return;
	}

	// The slab is as thick as the thickest connected road.
	FVector Origin = FVector::ZeroVector;
	float Height = 0.f;
	bool bSideLines = false;
	for (const FRMRoadEndRef& R : Found)
	{
		Origin += R.Location;
		Height = FMath::Max(Height, R.Height);
		bSideLines |= (R.Road && R.Road->GetMarkings().bEdgeLines) || (R.Intersection && R.Intersection->HasSideLines());
	}
	mSideLines = bSideLines;
	Origin /= Found.Num();
	SetActorLocationAndRotation(Origin, FRotator::ZeroRotator);

	TArray<FRMRoadEnd> Local;
	for (const FRMRoadEndRef& R : Found)
	{
		FRMRoadEnd E;
		E.Center = R.Location - Origin;
		E.Inward = FVector(R.Outward.X, R.Outward.Y, 0.f).GetSafeNormal();
		E.Width = R.Width;
		Local.Add(E);
	}

	// Only rebuild the preview when the set of ends actually changed.
	bool bChanged = bPicksChanged || Local.Num() != mLocalEnds.Num() || !FMath::IsNearlyEqual(Height, mHeight);
	for (int32 I = 0; !bChanged && I < Local.Num(); ++I)
	{
		bChanged = !Local[I].Center.Equals(mLocalEnds[I].Center, 1.f);
	}
	if (bChanged)
	{
		mLocalEnds = Local;
		mHeight = Height;
		RebuildPreview();
	}
}

namespace
{
	/** Camera-facing strips and shapes for the pick overlay, in actor space. */
	struct FOverlayBuilder
	{
		FVector Base;      // actor location (overlay vertices are relative to it)
		FVector Camera;
		float WorldPerPixel = 0.f; // at 1 cm distance
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FLinearColor> Colors;

		float PixelsAt(const FVector& P, float Pixels) const
		{
			return Pixels * WorldPerPixel * FMath::Max(FVector::Dist(P, Camera), 10.f);
		}

		void Quad(const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FLinearColor& Color)
		{
			const int32 I = Vertices.Num();
			for (const FVector& P : { A, B, C, D })
			{
				Vertices.Add(P - Base);
				Colors.Add(Color);
			}
			Triangles.Append({ I, I + 1, I + 2, I, I + 2, I + 3 });
		}

		/** A line Pixels wide on screen, facing the camera. */
		void Line(FVector A, FVector B, float Pixels, const FLinearColor& Color)
		{
			const FVector Dir = (B - A).GetSafeNormal();
			const float HA = PixelsAt(A, Pixels) * 0.5f;
			const float HB = PixelsAt(B, Pixels) * 0.5f;
			A -= Dir * HA; // overlap at the corners
			B += Dir * HB;
			FVector SA = FVector::CrossProduct(Dir, Camera - A).GetSafeNormal();
			FVector SB = FVector::CrossProduct(Dir, Camera - B).GetSafeNormal();
			Quad(A - SA * HA, A + SA * HA, B + SB * HB, B - SB * HB, Color);
		}

		/** Outline of a road end's cap (its end face). */
		void Cap(const FRMRoadEndRef& R, const FLinearColor& Color)
		{
			const FVector Out = FVector(R.Outward.X, R.Outward.Y, 0.f).GetSafeNormal();
			const FVector Right = FVector::CrossProduct(FVector::UpVector, Out);
			const FVector C = R.Location + Out * 2.f;
			const FVector Down(0.f, 0.f, -FMath::Max(R.Height, 10.f));
			const FVector TL = C - Right * R.Width * 0.5f;
			const FVector TR = C + Right * R.Width * 0.5f;
			Line(TL, TR, 4.f, Color);
			Line(TR, TR + Down, 4.f, Color);
			Line(TR + Down, TL + Down, 4.f, Color);
			Line(TL + Down, TL, 4.f, Color);
		}

		/** A flat 4 m arrow pointing down at the end, turned towards the camera. */
		void Arrow(const FRMRoadEndRef& R, const FLinearColor& Color)
		{
			const FVector Tip = R.Location + FVector(0.f, 0.f, 20.f);
			FVector ToCam = Camera - Tip;
			ToCam.Z = 0.f;
			const FVector Side = FVector::CrossProduct(FVector::UpVector, ToCam.GetSafeNormal()).GetSafeNormal();
			const FVector Up = FVector::UpVector;
			const FVector HeadTop = Tip + Up * 150.f;
			// Head: a triangle (as a quad with two corners at the tip).
			Quad(Tip, Tip, HeadTop + Side * 110.f, HeadTop - Side * 110.f, Color);
			// Shaft.
			Quad(HeadTop - Side * 35.f, HeadTop + Side * 35.f, Tip + Up * 400.f + Side * 35.f, Tip + Up * 400.f - Side * 35.f, Color);
		}
	};
}

void ARMIntersectionHologram::RebuildOverlay()
{
	mOverlayMesh->ClearAllMeshSections();
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager || (!mHasHover && mPicked.Num() == 0))
	{
		return;
	}
	int32 ViewW = 0, ViewH = 0;
	PC->GetViewportSize(ViewW, ViewH);
	FOverlayBuilder B;
	B.Base = GetActorLocation();
	B.Camera = PC->PlayerCameraManager->GetCameraLocation();
	B.WorldPerPixel = 2.f * FMath::Tan(FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle()) * 0.5f) / FMath::Max(ViewW, 1);

	const FLinearColor White(1.f, 1.f, 1.f);
	const FLinearColor Orange(1.f, 0.35f, 0.f);
	// Picked ends side by side (a split road, or roads that line up) show as one: one outline across
	// all of them and one arrow in its middle.
	const TArray<int32> Groups = IsectGroupEnds(mPicked);
	for (int32 G = 0; G < mPicked.Num(); ++G)
	{
		if (Groups[G] != G)
		{
			continue; // only once per group (at its root)
		}
		FRMRoadEndRef Merged = mPicked[G];
		const FVector Out = FVector(Merged.Outward.X, Merged.Outward.Y, 0.f).GetSafeNormal();
		const FVector Across = FVector::CrossProduct(FVector::UpVector, Out);
		float MinAcross = TNumericLimits<float>::Max(), MaxAcross = -TNumericLimits<float>::Max();
		int32 Members = 0;
		bool bOnlyHover = true;
		for (int32 I = 0; I < mPicked.Num(); ++I)
		{
			if (Groups[I] != G)
			{
				continue;
			}
			const float Mid = FVector::DotProduct(mPicked[I].Location - mPicked[G].Location, Across);
			MinAcross = FMath::Min(MinAcross, Mid - mPicked[I].Width * 0.5f);
			MaxAcross = FMath::Max(MaxAcross, Mid + mPicked[I].Width * 0.5f);
			Merged.Height = FMath::Max(Merged.Height, mPicked[I].Height);
			bOnlyHover &= mHasHover && SameEnd(mPicked[I], mHover);
			++Members;
		}
		Merged.Location = mPicked[G].Location + Across * ((MinAcross + MaxAcross) * 0.5f);
		Merged.Width = MaxAcross - MinAcross;
		if (!(Members == 1 && bOnlyHover))
		{
			B.Cap(Merged, White);
		}
		B.Arrow(Merged, White);
	}
	if (mHasHover)
	{
		B.Cap(mHover, Orange);
	}

	// Kept loaded for good: a plain static pointer would dangle once the garbage collector frees it.
	static UMaterialInterface* Fallback = [] { UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay")); if (M) { M->AddToRoot(); } return M; }();
	UMaterialInterface* Material = mOverlayMaterial ? mOverlayMaterial : Fallback;
	mOverlayMesh->CreateMeshSection_LinearColor(0, B.Vertices, B.Triangles, TArray<FVector>(), TArray<FVector2D>(), B.Colors, TArray<FProcMeshTangent>(), false);
	if (Material)
	{
		mOverlayMesh->SetMaterial(0, Material);
	}
	mOverlayMesh->SetRenderCustomDepth(false);
}

void ARMIntersectionHologram::RebuildPreview()
{
	mPreviewMesh->ClearAllMeshSections();
	mGeometryValid = false;
	if (mLocalEnds.Num() >= 2)
	{
		FRMMesh Top, Body;
		TArray<FVector> Outline;
		TArray<bool> IsMouth;
		mGeometryValid = RMGeometry::BuildIntersection(mLocalEnds, mHeight, 400.f, 10, Top, Body, &Outline, &IsMouth);
		if (mGeometryValid)
		{
			Top.ApplyToSection(mPreviewMesh, 0, false);
			Body.ApplyToSection(mPreviewMesh, 1, false);
			// Side lines, as the intersection will have them.
			const ARMBuildableIntersection* CDO = mBuildClass ? Cast<ARMBuildableIntersection>(mBuildClass->GetDefaultObject()) : nullptr;
			if (mSideLines && CDO)
			{
				TArray<TArray<FVector>> Curbs;
				RMGeometry::GetIntersectionCurbs(Outline, IsMouth, Curbs);
				FRMMesh Lines;
				RMGeometry::BuildCurbLines(Curbs, CDO->mEdgeLineInset, Top, Lines);
				Lines.ApplyToSection(mPreviewMesh, 2, false);
			}
		}
	}
	UpdateHologramLook();
}

void ARMIntersectionHologram::SetMaterialState(EHologramMaterialState state)
{
	Super::SetMaterialState(state);
	UpdateHologramLook();
}

void ARMIntersectionHologram::UpdateHologramLook()
{
	const bool bInvalid = mPlacementMaterialState == EHologramMaterialState::HMS_ERROR;
	const ARMBuildableIntersection* CDO = mBuildClass ? Cast<ARMBuildableIntersection>(mBuildClass->GetDefaultObject()) : nullptr;
	if (CDO && mPreviewMesh->GetNumSections() > 0)
	{
		// The real surface: the look and paint of the first road (or intersection) joined, as the
		// intersection will be built. The game's tint and outline (stencil) show valid / warning / invalid.
		uint8 Look = 0;
		FLinearColor Primary(1.f, 1.f, 1.f), Secondary(1.f, 1.f, 1.f);
		CDO->GetPaintColours(Primary, Secondary);
		for (const FRMRoadEndRef& End : mPicked)
		{
			const ARMBuildableProcedural* Joined = End.Road ? static_cast<const ARMBuildableProcedural*>(End.Road) : End.Intersection;
			if (Joined)
			{
				Look = Joined->GetLook();
				Joined->GetPaintColours(Primary, Secondary);
				break;
			}
		}
		UMaterialInterface* Top = CDO->GetLookMaterialFor(Look, true, CDO->mTopMaterial);
		UMaterialInterface* Body = CDO->GetLookMaterialFor(Look, false, CDO->mBodyMaterial);
		for (int32 I = 0; I < mPreviewMesh->GetNumSections(); ++I)
		{
			mPreviewMesh->SetMaterial(I, I == 1 ? Body : Top);
		}
		mPreviewMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(Primary.R, Primary.G, Primary.B, 1.0));
		mPreviewMesh->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(Secondary.R, Secondary.G, Secondary.B, 1.0));
		RMHologramLook::Apply(mPreviewMesh, nullptr, GetStencilForHologramMaterialState(mPlacementMaterialState));
	}
	else
	{
		// Can't be built: the game's red hologram.
		UMaterialInterface* Material = mPreviewMaterial;
		if (!Material)
		{
			Material = (bInvalid && mInvalidPlacementMaterial) ? mInvalidPlacementMaterial.Get() : mValidPlacementMaterial.Get();
		}
		RMHologramLook::Apply(mPreviewMesh, Material, GetStencilForHologramMaterialState(mPlacementMaterialState));
	}
	// The pick overlay keeps its own material (the game may give every mesh the hologram material).
	// Kept loaded for good: a plain static pointer would dangle once the garbage collector frees it.
	static UMaterialInterface* OverlayFallback = [] { UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay")); if (M) { M->AddToRoot(); } return M; }();
	if (UMaterialInterface* Overlay = mOverlayMaterial ? mOverlayMaterial : OverlayFallback)
	{
		if (mOverlayMesh->GetNumSections() > 0)
		{
			mOverlayMesh->SetMaterial(0, Overlay);
		}
		mOverlayMesh->SetRenderCustomDepth(false);
	}
}


void ARMIntersectionHologram::CheckValidPlacement()
{
	// No Super call on purpose: the vanilla floor check ("Surface is too uneven") is meant for
	// box-shaped buildings and rejects spline pieces. Placement rules are checked below instead.
	if (mHasHover)
	{
		return; // this click picks (or drops) the aimed road end
	}
	if (mLocalEnds.Num() < 2)
	{
		AddConstructDisqualifier(URMCD_PickRoadEnds::StaticClass());
	}
	else if (!mGeometryValid)
	{
		AddConstructDisqualifier(URMCD_BadIntersection::StaticClass());
	}
}

void ARMIntersectionHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
	Super::ConfigureActor(inBuildable);
	if (ARMBuildableIntersection* X = Cast<ARMBuildableIntersection>(inBuildable))
	{
		X->SetSideLines(mSideLines);
		// The look of the first road (or intersection) joined.
		for (const FRMRoadEndRef& End : mPicked)
		{
			if (End.Road)
			{
				X->SetLook(End.Road->GetLook());
				break;
			}
			if (End.Intersection)
			{
				X->SetLook(End.Intersection->GetLook());
				break;
			}
		}
		X->SetEnds(mLocalEnds, mHeight);
		X->SetCostMultiplier(GetBaseCostMultiplier());
	}
}

AActor* ARMIntersectionHologram::Construct(TArray<AActor*>& out_children, FNetConstructionID constructionID)
{
	AActor* Built = Super::Construct(out_children, constructionID);
	// Built: start over with no picked ends (outlines and arrows go away). On a client the picked
	// ends drop out by themselves once the new intersection arrives and takes them.
	if (Built)
	{
		mPicked.Reset();
		mHasHover = false;
		mPickKey = 0;
		mOverlayMesh->ClearAllMeshSections();
	}
	return Built;
}
