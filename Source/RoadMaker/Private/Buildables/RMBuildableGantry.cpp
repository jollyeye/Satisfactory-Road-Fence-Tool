#include "Buildables/RMBuildableGantry.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Math/RotationMatrix.h"
#include "Net/UnrealNetwork.h"

ARMBuildableGantry::ARMBuildableGantry(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	mLODDistance = 0.f; // simple enough to draw in full
}

void ARMBuildableGantry::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableGantry, mHalfSpan);
	DOREPLIFETIME(ARMBuildableGantry, mBeamBottom);
}

void ARMBuildableGantry::SetShape(float HalfSpan, float BeamBottom)
{
	mHalfSpan = HalfSpan;
	mBeamBottom = BeamBottom;
	if (HasActorBegunPlay())
	{
		RebuildMesh();
	}
}

namespace
{
	UStaticMesh* LoadPillarMesh(const TCHAR* Name)
	{
		return LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Game/FactoryGame/Buildable/Building/Pillars/Mesh/%s.%s"), Name, Name));
	}

	/** The game's own rotation of its beam meshes (a beam runs along the actor's X, from its origin). */
	const FRotator HBeamMeshRotation(0.f, 90.f, -90.f);
	const float HBeamMeshLength = 100.f;

	/** A stretched game beam starting at Start, running along Direction for Length. */
	void PlaceBeam(UStaticMeshComponent* Part, const FVector& Start, const FQuat& Outer, float Length)
	{
		Part->SetRelativeLocation(Start);
		Part->SetRelativeRotation(FRotator(Outer * HBeamMeshRotation.Quaternion()));
		Part->SetRelativeScale3D(FVector(1.f, 1.f, Length / HBeamMeshLength));
	}

	/** A connection cube (centred), stood up on its long side. */
	void PlaceBlock(UStaticMeshComponent* Part, const FVector& Center)
	{
		Part->SetRelativeLocation(Center);
		Part->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));
		Part->SetRelativeScale3D(FVector::OneVector);
	}

	enum EPart { ColumnA, ColumnB, BeamEndA, BeamMid, BeamEndB, BaseA, BaseB, BeamMidB, NumParts };

	/** Grows 0 -> 1 over [From, To] of T. */
	float GantryPhase(float T, float From, float To)
	{
		return FMath::Clamp((T - From) / FMath::Max(To - From, KINDA_SMALL_NUMBER), 0.f, 1.f);
	}

	constexpr float GantryRiseShare = 0.7f; // columns: the first 70% of the animation, the beam the rest
}

void ARMBuildableGantry::RebuildMesh()
{
	// Collision only: invisible boxes around the parts.
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	FRMMesh Boxes;
	BuildGantryMesh(mHalfSpan, mBeamBottom, Boxes);
	Boxes.ApplyToSection(mMesh, 0, true);
	ApplyMaterial(0, nullptr);
	mMesh->SetRenderInMainPass(false);
	mMesh->SetRenderInDepthPass(false);
	mMesh->SetCastShadow(false);

	if (mParts.Num() == 0)
	{
		CreateParts(this, GetRootComponent(), mParts);
	}
	if (!mAnimating)
	{
		PlaceParts(mParts, mHalfSpan, mBeamBottom, 1.f);
	}
}

void ARMBuildableGantry::CreateParts(AActor* Owner, USceneComponent* Parent, TArray<TObjectPtr<UStaticMeshComponent>>& OutParts)
{
	OutParts.Reset();
	// H-beam for the columns and the middle of the cross beam, shelf beams for its two ends, connection
	// cubes as feet; the second middle piece only shows during the build animation.
	UStaticMesh* HBeam = LoadPillarMesh(TEXT("SM_Beam_04"));
	UStaticMesh* Shelf = LoadPillarMesh(TEXT("SM_Beam_05"));
	UStaticMesh* Block = LoadPillarMesh(TEXT("SM_ConnectionCube_Wide_01"));
	UStaticMesh* Meshes[NumParts] = { HBeam, HBeam, Shelf, HBeam, Shelf, Block, Block, HBeam };
	for (int32 I = 0; I < NumParts; ++I)
	{
		if (!Meshes[I])
		{
			return;
		}
	}
	for (int32 I = 0; I < NumParts; ++I)
	{
		UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(Owner);
		Part->SetupAttachment(Parent);
		Part->SetStaticMesh(Meshes[I]);
		Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Part->SetCanEverAffectNavigation(false);
		Part->RegisterComponent();
		OutParts.Add(Part);
	}
}

void ARMBuildableGantry::PlaceParts(const TArray<TObjectPtr<UStaticMeshComponent>>& Parts, float HalfSpan, float BeamBottom, float Built)
{
	if (Parts.Num() != NumParts)
	{
		return;
	}
	// The game's beam mesh starts at its origin and runs along its length (not centred), so beams are
	// placed by their start point. Columns (along the gantry's Z: pitch 90) stand on an upright
	// connection cube; the cross beam (along Y: yaw 90) rests on top of the columns so the flanges do
	// not intersect. It is three pieces: a 1 m shelf beam at each end (the shelf beam repeats its
	// pattern when stretched) and a stretched H-beam between them.
	const FQuat Up = FRotator(90.f, 0.f, 0.f).Quaternion();
	const FQuat Across = FRotator(0.f, 90.f, 0.f).Quaternion();
	const FQuat Back = FRotator(0.f, -90.f, 0.f).Quaternion();
	// The rising front (cm above the road), then the share of the beam grown from each column.
	const float Rise = GantryPhase(Built, 0.f, GantryRiseShare) * BeamBottom;
	const float Span = GantryPhase(Built, GantryRiseShare, 1.f);
	for (int32 Side = 0; Side < 2; ++Side)
	{
		const float Y = (Side == 0 ? -1.f : 1.f) * HalfSpan;
		// The foot grows up from the road (stood on its long side: its X is up).
		const float Foot = FMath::Clamp(Rise / FootHeight, 0.f, 1.f);
		UStaticMeshComponent* Base = Parts[BaseA + Side];
		Base->SetVisibility(Foot > 0.01f);
		PlaceBlock(Base, FVector(0.f, Y, FootHeight * Foot * 0.5f));
		Base->SetRelativeScale3D(FVector(FMath::Max(Foot, 0.01f), 1.f, 1.f));
		// The column grows from the foot up to the front.
		const float ColumnFull = BeamBottom - FootHeight + FootOverlap;
		const float Column = FMath::Clamp(Rise - (FootHeight - FootOverlap), 0.f, ColumnFull);
		Parts[ColumnA + Side]->SetVisibility(Column > 1.f);
		PlaceBeam(Parts[ColumnA + Side], FVector(0.f, Y, FootHeight - FootOverlap), Up, FMath::Max(Column, 1.f));
	}
	const float BeamZ = BeamBottom + BeamDepth * 0.5f;
	const float Start = -(HalfSpan + BeamOverhang);
	const float Total = 2.f * (HalfSpan + BeamOverhang);
	const float Middle = Total - 2.f * EndPiece;
	// The end pieces sit on the columns; the middle grows from both of them and meets in the centre.
	Parts[BeamEndA]->SetVisibility(Span > 0.f);
	Parts[BeamEndB]->SetVisibility(Span > 0.f);
	PlaceBeam(Parts[BeamEndA], FVector(0.f, Start, BeamZ), Across, EndPiece);
	PlaceBeam(Parts[BeamEndB], FVector(0.f, Start + Total - EndPiece, BeamZ), Across, EndPiece);
	if (Built >= 1.f)
	{
		Parts[BeamMid]->SetVisibility(true);
		Parts[BeamMidB]->SetVisibility(false);
		PlaceBeam(Parts[BeamMid], FVector(0.f, Start + EndPiece, BeamZ), Across, Middle);
	}
	else
	{
		const float Half = FMath::Max(Middle * 0.5f * Span, 1.f);
		Parts[BeamMid]->SetVisibility(Span > 0.01f);
		Parts[BeamMidB]->SetVisibility(Span > 0.01f);
		PlaceBeam(Parts[BeamMid], FVector(0.f, Start + EndPiece, BeamZ), Across, Half);
		PlaceBeam(Parts[BeamMidB], FVector(0.f, -Start - EndPiece, BeamZ), Back, Half);
	}
}

void ARMBuildableGantry::BuildGlowMesh(float HalfSpan, float BeamBottom, float Built, FRMMesh& Out)
{
	auto AddAxisBox = [&Out](const FVector& Center, const FVector& Half)
	{
		Out.AddBox(Center, FVector::ForwardVector, FVector::RightVector, FVector::UpVector, Half, 100.f);
	};
	const float Band = 6.f;
	if (Built < GantryRiseShare)
	{
		// A glowing ring around each foot / column at the front.
		const float Rise = GantryPhase(Built, 0.f, GantryRiseShare) * BeamBottom;
		const bool bFoot = Rise < FootHeight;
		const float HX = (bFoot ? FootHalfAlong : ColumnHalfAlong) + 4.f;
		const float HY = (bFoot ? FootHalfAcross : ColumnHalfAcross) + 4.f;
		for (float Side : { -1.f, 1.f })
		{
			AddAxisBox(FVector(0.f, Side * HalfSpan, FMath::Max(Rise - Band, Band)), FVector(HX, HY, Band));
		}
	}
	else if (Built < 1.f)
	{
		// A glowing slice across the beam at each growing tip.
		const float Span = GantryPhase(Built, GantryRiseShare, 1.f);
		const float Middle = 2.f * (HalfSpan + BeamOverhang) - 2.f * EndPiece;
		const float Tip = HalfSpan + BeamOverhang - EndPiece - Middle * 0.5f * Span;
		for (float Side : { -1.f, 1.f })
		{
			AddAxisBox(FVector(0.f, Side * FMath::Max(Tip - Band, 0.f), BeamBottom + BeamDepth * 0.5f),
				FVector(BeamHalfThickness + 4.f, Band, BeamDepth * 0.5f + 4.f));
		}
	}
}

void ARMBuildableGantry::StartRiseAnimation()
{
	UWorld* World = GetWorld();
	if (!World || mParts.Num() != NumParts)
	{
		return;
	}
	mAnimating = true;
	mBuildStart = World->GetTimeSeconds();
	mGlowMesh = NewObject<UProceduralMeshComponent>(this, TEXT("GantryGlow"), RF_Transient);
	mGlowMesh->SetupAttachment(GetRootComponent());
	mGlowMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mGlowMesh->SetCanEverAffectNavigation(false);
	mGlowMesh->SetCastShadow(false);
	mGlowMesh->RegisterComponent();
	World->GetTimerManager().SetTimer(mBuildTimer, this, &ARMBuildableGantry::StepBuildAnimation, 0.02f, true);
	StepBuildAnimation();
}

void ARMBuildableGantry::StepBuildAnimation()
{
	UWorld* World = GetWorld();
	const float T = World ? FMath::Clamp(static_cast<float>(World->GetTimeSeconds() - mBuildStart) / GetBuildAnimDuration(), 0.f, 1.f) : 1.f;
	if (T >= 1.f || !World)
	{
		mAnimating = false;
		if (World)
		{
			World->GetTimerManager().ClearTimer(mBuildTimer);
		}
		if (mGlowMesh)
		{
			mGlowMesh->DestroyComponent();
			mGlowMesh = nullptr;
		}
		PlaceParts(mParts, mHalfSpan, mBeamBottom, 1.f);
		return;
	}
	const float Eased = T * T * (3.f - 2.f * T);
	PlaceParts(mParts, mHalfSpan, mBeamBottom, Eased);
	if (mGlowMesh)
	{
		// Kept loaded for good: a plain static pointer would dangle once the garbage collector frees it.
		static UMaterialInterface* GlowMaterial = [] { UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_BuildGlow.M_RM_BuildGlow")); if (M) { M->AddToRoot(); } return M; }();
		FRMMesh Glow;
		BuildGlowMesh(mHalfSpan, mBeamBottom, Eased, Glow);
		TArray<FLinearColor> Colors;
		Colors.Init(FLinearColor(1.f, 0.55f, 0.12f), Glow.Vertices.Num());
		mGlowMesh->CreateMeshSection_LinearColor(0, Glow.Vertices, Glow.Triangles, Glow.Normals, Glow.UVs, Colors, TArray<FProcMeshTangent>(), false);
		if (GlowMaterial)
		{
			mGlowMesh->SetMaterial(0, GlowMaterial);
		}
	}
}

int32 ARMBuildableGantry::CostUnits(float HalfSpan, float BeamBottom)
{
	const float Length = 2.f * BeamBottom + 2.f * (HalfSpan + BeamOverhang);
	return FMath::Max(1, FMath::CeilToInt(Length / 800.f));
}

void ARMBuildableGantry::BuildGantryMesh(float HalfSpan, float BeamBottom, FRMMesh& Out)
{
	auto AddAxisBox = [&Out](const FVector& Center, const FVector& Half)
	{
		Out.AddBox(Center, FVector::ForwardVector, FVector::RightVector, FVector::UpVector, Half, 100.f);
	};
	for (float Side : { -1.f, 1.f })
	{
		AddAxisBox(FVector(0.f, Side * HalfSpan, BeamBottom * 0.5f), FVector(ColumnHalfAlong, ColumnHalfAcross, BeamBottom * 0.5f));
		AddAxisBox(FVector(0.f, Side * HalfSpan, FootHeight * 0.5f), FVector(FootHalfAlong, FootHalfAcross, FootHeight * 0.5f));
	}
	AddAxisBox(FVector(0.f, 0.f, BeamBottom + BeamDepth * 0.5f), FVector(BeamHalfThickness, HalfSpan + BeamOverhang, BeamDepth * 0.5f));
}

bool ARMBuildableGantry::GetSignFace(const FVector& Hit, const FVector& HitNormal, FVector& OutLocation, FVector& OutNormal, float* OutDepth) const
{
	float Depth = BeamHalfThickness + 1.f;
	const FTransform& T = GetActorTransform();
	const FVector Local = T.InverseTransformPosition(Hit);
	const FVector LocalNormal = T.InverseTransformVectorNoScale(HitNormal);
	FVector OnFace;
	FVector FaceNormal;

	if (Local.Z >= mBeamBottom - 20.f)
	{
		// The cross beam: flat on its front or back face, a point every 2 m along it (counted from the
		// middle), at the bottom edge, the middle or the top edge.
		if (FMath::Abs(Local.Y) > mHalfSpan + BeamOverhang || Local.Z > mBeamBottom + BeamDepth + 60.f)
		{
			return false;
		}
		const float Side = (FMath::Abs(LocalNormal.X) > 0.3f ? LocalNormal.X : Local.X) >= 0.f ? 1.f : -1.f;
		const float Along = FMath::Clamp(FMath::RoundToFloat(Local.Y / SignSpacing) * SignSpacing, -mHalfSpan, mHalfSpan);
		const float Level = FMath::Clamp(FMath::RoundToFloat((Local.Z - mBeamBottom) / (BeamDepth * 0.5f)), 0.f, 2.f);
		OnFace = FVector(Side * (BeamHalfThickness + 1.f), Along, mBeamBottom + Level * BeamDepth * 0.5f);
		FaceNormal = FVector(Side, 0.f, 0.f);
		Depth = BeamHalfThickness + 1.f;
	}
	else
	{
		// A column: on any of its four faces, a point every 2 m up the column (the lowest above the foot).
		const float ColumnY = Local.Y >= 0.f ? mHalfSpan : -mHalfSpan;
		if (FMath::Abs(Local.Y - ColumnY) > ColumnHalfAcross + 60.f || FMath::Abs(Local.X) > ColumnHalfAlong + 60.f)
		{
			return false;
		}
		const float RelX = Local.X;
		const float RelY = Local.Y - ColumnY;
		bool bAlongRoadFace; // the faces looking along the road (+-X) or across it (+-Y)
		if (FMath::Abs(LocalNormal.X) > 0.3f || FMath::Abs(LocalNormal.Y) > 0.3f)
		{
			bAlongRoadFace = FMath::Abs(LocalNormal.X) >= FMath::Abs(LocalNormal.Y);
		}
		else
		{
			bAlongRoadFace = FMath::Abs(RelX) / ColumnHalfAlong >= FMath::Abs(RelY) / ColumnHalfAcross;
		}
		const float Z = FMath::Clamp(FMath::RoundToFloat(Local.Z / SignSpacing) * SignSpacing, 2.f * SignSpacing, FMath::Max(2.f * SignSpacing, mBeamBottom - SignSpacing * 0.5f));
		if (bAlongRoadFace)
		{
			const float Side = (FMath::Abs(LocalNormal.X) > 0.3f ? LocalNormal.X : RelX) >= 0.f ? 1.f : -1.f;
			OnFace = FVector(Side * (ColumnHalfAlong + 1.f), ColumnY, Z);
			FaceNormal = FVector(Side, 0.f, 0.f);
			Depth = ColumnHalfAlong + 1.f;
		}
		else
		{
			const float Side = (FMath::Abs(LocalNormal.Y) > 0.3f ? LocalNormal.Y : RelY) >= 0.f ? 1.f : -1.f;
			OnFace = FVector(0.f, ColumnY + Side * (ColumnHalfAcross + 1.f), Z);
			FaceNormal = FVector(0.f, Side, 0.f);
			Depth = ColumnHalfAcross + 1.f;
		}
	}
	OutLocation = T.TransformPosition(OnFace);
	OutNormal = T.TransformVectorNoScale(FaceNormal);
	if (OutDepth)
	{
		*OutDepth = Depth;
	}
	return true;
}
