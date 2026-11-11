#include "Buildables/RMBuildableGantry.h"
#include "ProceduralMeshComponent.h"
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

void ARMBuildableGantry::RebuildMesh()
{
	mMesh->ClearAllMeshSections();
	ClearFarMesh();
	FRMMesh Steel;
	BuildGantryMesh(mHalfSpan, mBeamBottom, Steel);
	Steel.ApplyToSection(mMesh, 0, true);
	ApplyMaterial(0, mMainMaterial);
}

int32 ARMBuildableGantry::CostUnits(float HalfSpan, float BeamBottom)
{
	const float Length = 2.f * (BeamBottom + BeamDepth) + 2.f * HalfSpan + 2.f * ColumnInset;
	return FMath::Max(1, FMath::CeilToInt(Length / 800.f));
}

namespace
{
	/** A square-section bar between two points. */
	void AddStrut(FRMMesh& Out, const FVector& A, const FVector& B, float HalfThickness)
	{
		const FVector D = B - A;
		const float Len = D.Size();
		if (Len < 1.f)
		{
			return;
		}
		const FVector AxisX = D / Len;
		const FVector Ref = FMath::Abs(AxisX.Z) > 0.95f ? FVector::ForwardVector : FVector::UpVector;
		const FVector AxisY = FVector::CrossProduct(Ref, AxisX).GetSafeNormal();
		const FVector AxisZ = FVector::CrossProduct(AxisX, AxisY);
		Out.AddBox((A + B) * 0.5f, AxisX, AxisY, AxisZ, FVector(Len * 0.5f, HalfThickness, HalfThickness), 100.f);
	}

	void AddAxisBox(FRMMesh& Out, const FVector& Center, const FVector& Half)
	{
		Out.AddBox(Center, FVector::ForwardVector, FVector::RightVector, FVector::UpVector, Half, 100.f);
	}
}

void ARMBuildableGantry::BuildGantryMesh(float HalfSpan, float BeamBottom, FRMMesh& Out)
{
	const float Top = BeamBottom + BeamDepth;
	const float Lx = BeamHalfThickness;

	// Columns: four chords, diagonal bracing on the front and back planes, a foot plate.
	for (float Side : { -1.f, 1.f })
	{
		const float Y = Side * HalfSpan;
		for (float X : { -Lx, Lx })
		{
			for (float Dy : { -18.f, 18.f })
			{
				AddStrut(Out, FVector(X, Y + Dy, 0.f), FVector(X, Y + Dy, Top), 6.f);
			}
			// Zigzag between the two chords every 1.5 m.
			const int32 Cells = FMath::Max(1, FMath::RoundToInt(Top / 150.f));
			for (int32 I = 0; I < Cells; ++I)
			{
				const float Z0 = Top * I / Cells;
				const float Z1 = Top * (I + 1) / Cells;
				const bool bFlip = (I % 2) == 0;
				AddStrut(Out, FVector(X, Y + (bFlip ? -18.f : 18.f), Z0), FVector(X, Y + (bFlip ? 18.f : -18.f), Z1), 3.5f);
			}
		}
		AddAxisBox(Out, FVector(0.f, Y, 7.f), FVector(55.f, 45.f, 7.f));
	}

	// Beam: front and back plates, top and bottom plates, edge chords; a little longer than the span.
	const float Y0 = -HalfSpan - 40.f;
	const float Y1 = HalfSpan + 40.f;
	const float YC = (Y0 + Y1) * 0.5f;
	const float HalfLen = (Y1 - Y0) * 0.5f;
	for (float X : { -1.f, 1.f })
	{
		AddAxisBox(Out, FVector(X * (Lx - 4.f), YC, BeamBottom + BeamDepth * 0.5f), FVector(4.f, HalfLen, BeamDepth * 0.5f));
		for (float Z : { BeamBottom + 6.f, Top - 6.f })
		{
			AddStrut(Out, FVector(X * Lx, Y0, Z), FVector(X * Lx, Y1, Z), 6.f);
		}
	}
	AddAxisBox(Out, FVector(0.f, YC, BeamBottom + 3.f), FVector(Lx - 8.f, HalfLen, 3.f));
	AddAxisBox(Out, FVector(0.f, YC, Top - 3.f), FVector(Lx - 8.f, HalfLen, 3.f));
}

bool ARMBuildableGantry::GetSignFace(const FVector& Hit, const FVector& HitNormal, FVector& OutLocation, FVector& OutNormal) const
{
	const FTransform& T = GetActorTransform();
	const FVector Local = T.InverseTransformPosition(Hit);
	if (FMath::Abs(Local.Y) > mHalfSpan + 40.f || Local.Z < mBeamBottom - 60.f || Local.Z > mBeamBottom + BeamDepth + 60.f)
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
