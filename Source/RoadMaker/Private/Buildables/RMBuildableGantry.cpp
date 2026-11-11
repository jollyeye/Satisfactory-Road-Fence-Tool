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

	enum EPart { ColumnA, ColumnB, BeamEndA, BeamMid, BeamEndB, BaseA, BaseB, NumParts };
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
		// H-beam for the columns and the middle of the cross beam, shelf beams for its two ends, connection cubes as feet.
		UStaticMesh* HBeam = LoadPillarMesh(TEXT("SM_Beam_04"));
		UStaticMesh* Shelf = LoadPillarMesh(TEXT("SM_Beam_05"));
		UStaticMesh* Block = LoadPillarMesh(TEXT("SM_ConnectionCube_Wide_01"));
		UStaticMesh* Meshes[NumParts] = { HBeam, HBeam, Shelf, HBeam, Shelf, Block, Block };
		for (int32 I = 0; I < NumParts; ++I)
		{
			if (!Meshes[I])
			{
				continue;
			}
			UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
			Part->SetupAttachment(GetRootComponent());
			Part->SetStaticMesh(Meshes[I]);
			Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Part->RegisterComponent();
			mParts.Add(Part);
		}
	}
	if (mParts.Num() == NumParts)
	{
		// The game's beam mesh starts at its origin and runs along its length (not centred), so beams are
		// placed by their start point. Columns (along the gantry's Z: pitch 90) stand on an upright
		// connection cube; the cross beam (along Y: yaw 90) rests on top of the columns so the flanges do
		// not intersect. It is three pieces: a 1 m shelf beam at each end (the shelf beam repeats its
		// pattern when stretched) and a stretched H-beam between them.
		const FQuat Up = FRotator(90.f, 0.f, 0.f).Quaternion();
		const FQuat Across = FRotator(0.f, 90.f, 0.f).Quaternion();
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const float Y = (Side == 0 ? -1.f : 1.f) * mHalfSpan;
			PlaceBlock(mParts[BaseA + Side], FVector(0.f, Y, FootHeight * 0.5f));
			PlaceBeam(mParts[ColumnA + Side], FVector(0.f, Y, FootHeight), Up, mBeamBottom - FootHeight);
		}
		const float BeamZ = mBeamBottom + BeamDepth * 0.5f;
		const float Start = -(mHalfSpan + BeamOverhang);
		const float Total = 2.f * (mHalfSpan + BeamOverhang);
		PlaceBeam(mParts[BeamEndA], FVector(0.f, Start, BeamZ), Across, EndPiece);
		PlaceBeam(mParts[BeamMid], FVector(0.f, Start + EndPiece, BeamZ), Across, Total - 2.f * EndPiece);
		PlaceBeam(mParts[BeamEndB], FVector(0.f, Start + Total - EndPiece, BeamZ), Across, EndPiece);
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

bool ARMBuildableGantry::GetSignFace(const FVector& Hit, const FVector& HitNormal, FVector& OutLocation, FVector& OutNormal) const
{
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
		}
		else
		{
			const float Side = (FMath::Abs(LocalNormal.Y) > 0.3f ? LocalNormal.Y : RelY) >= 0.f ? 1.f : -1.f;
			OnFace = FVector(0.f, ColumnY + Side * (ColumnHalfAcross + 1.f), Z);
			FaceNormal = FVector(0.f, Side, 0.f);
		}
	}
	OutLocation = T.TransformPosition(OnFace);
	OutNormal = T.TransformVectorNoScale(FaceNormal);
	return true;
}
