#include "Buildables/RMBuildableGantry.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
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
	/** The game's H-beam: a 1 m long piece (about 1 m x 0.8 m in section) that the game stretches along its local Z. */
	UStaticMesh* LoadHBeamMesh()
	{
		static TWeakObjectPtr<UStaticMesh> Cached;
		if (!Cached.IsValid())
		{
			Cached = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/FactoryGame/Buildable/Building/Pillars/Mesh/SM_Beam_04.SM_Beam_04"));
		}
		return Cached.Get();
	}

	/** The game's own rotation of its beam mesh (a beam runs along the actor's X). */
	const FRotator HBeamMeshRotation(0.f, 90.f, -90.f);
	const float HBeamMeshLength = 100.f;

	void PlacePart(UStaticMeshComponent* Part, const FVector& Center, const FRotator& Outer, float Length)
	{
		Part->SetRelativeLocation(Center);
		Part->SetRelativeRotation(FRotator(Outer.Quaternion() * HBeamMeshRotation.Quaternion()));
		Part->SetRelativeScale3D(FVector(1.f, 1.f, Length / HBeamMeshLength));
	}
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

	UStaticMesh* BeamMesh = LoadHBeamMesh();
	if (mParts.Num() == 0 && BeamMesh)
	{
		for (int32 I = 0; I < 3; ++I)
		{
			UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(this);
			Part->SetupAttachment(GetRootComponent());
			Part->SetStaticMesh(BeamMesh);
			Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Part->RegisterComponent();
			mParts.Add(Part);
		}
	}
	if (mParts.Num() == 3)
	{
		// The game's beam mesh starts at its origin and runs along its length (not centred), so each part is
		// placed by its start point. Columns (length along the gantry's Z: pitch 90) from the road up; the
		// cross beam (along Y: yaw 90) from the left column's outer face, resting on top of the columns so
		// the flanges do not intersect.
		PlacePart(mParts[0], FVector(0.f, -mHalfSpan, 0.f), FRotator(90.f, 0.f, 0.f), mBeamBottom);
		PlacePart(mParts[1], FVector(0.f, mHalfSpan, 0.f), FRotator(90.f, 0.f, 0.f), mBeamBottom);
		PlacePart(mParts[2], FVector(0.f, -(mHalfSpan + BeamOverhang), mBeamBottom + BeamDepth * 0.5f), FRotator(0.f, 90.f, 0.f), 2.f * (mHalfSpan + BeamOverhang));
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
	}
	AddAxisBox(FVector(0.f, 0.f, BeamBottom + BeamDepth * 0.5f), FVector(BeamHalfThickness, HalfSpan + BeamOverhang, BeamDepth * 0.5f));
}

bool ARMBuildableGantry::GetSignFace(const FVector& Hit, const FVector& HitNormal, FVector& OutLocation, FVector& OutNormal) const
{
	const FTransform& T = GetActorTransform();
	const FVector Local = T.InverseTransformPosition(Hit);
	if (FMath::Abs(Local.Y) > mHalfSpan + BeamOverhang || Local.Z < mBeamBottom - 60.f || Local.Z > mBeamBottom + BeamDepth + 60.f)
	{
		return false;
	}
	const FVector LocalNormal = T.InverseTransformVectorNoScale(HitNormal);
	float Side = FMath::Abs(LocalNormal.X) > 0.3f ? LocalNormal.X : Local.X;
	Side = Side >= 0.f ? 1.f : -1.f;
	// Bottom edge, middle, top edge of the beam.
	const float Level = FMath::Clamp(FMath::RoundToFloat((Local.Z - mBeamBottom) / (BeamDepth * 0.5f)), 0.f, 2.f);
	const FVector OnFace(Side * (BeamHalfThickness + 1.f), FMath::Clamp(Local.Y, -mHalfSpan, mHalfSpan), mBeamBottom + Level * BeamDepth * 0.5f);
	OutLocation = T.TransformPosition(OnFace);
	OutNormal = T.TransformVectorNoScale(FVector(Side, 0.f, 0.f));
	return true;
}
