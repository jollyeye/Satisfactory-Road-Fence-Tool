// Shared by the click tools (tunnel, gantry, 1.5.0): camera-facing overlay lines and a polyline helper.
#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectGlobals.h"

namespace RMOverlayLines
{
	constexpr float OverlayPixels = 4.f;

	/** Closest point of a polyline to P seen from above: segment, fraction, point (3D, on the line). */
	inline bool Nearest2D(const TArray<FVector>& L, const FVector& P, int32& OutSeg, float& OutT, FVector& OutPoint)
	{
		float Best = TNumericLimits<float>::Max();
		for (int32 I = 0; I + 1 < L.Num(); ++I)
		{
			const FVector2D AB(L[I + 1] - L[I]), AP(P - L[I]);
			const float Len2 = AB.SizeSquared();
			const float T = Len2 > 1e-4f ? FMath::Clamp(FVector2D::DotProduct(AP, AB) / Len2, 0.f, 1.f) : 0.f;
			const FVector C = FMath::Lerp(L[I], L[I + 1], T);
			const float D = FVector::DistSquared2D(C, P);
			if (D < Best)
			{
				Best = D;
				OutSeg = I;
				OutT = T;
				OutPoint = C;
			}
		}
		return Best < TNumericLimits<float>::Max();
	}

	/** Camera-facing lines, in actor space (as the Gap Fill tool draws them). */
	struct FOverlay
	{
		FVector Base;
		FVector Camera;
		float WorldPerPixel = 0.f;
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FLinearColor> Colors;

		void Line(FVector A, FVector B, const FLinearColor& Color)
		{
			A.Z += 8.f;
			B.Z += 8.f;
			const FVector Dir = (B - A).GetSafeNormal();
			if (Dir.IsNearlyZero())
			{
				return;
			}
			const float HA = OverlayPixels * WorldPerPixel * FMath::Max(FVector::Dist(A, Camera), 10.f) * 0.5f;
			const float HB = OverlayPixels * WorldPerPixel * FMath::Max(FVector::Dist(B, Camera), 10.f) * 0.5f;
			A -= Dir * HA;
			B += Dir * HB;
			const FVector SA = FVector::CrossProduct(Dir, Camera - A).GetSafeNormal();
			const FVector SB = FVector::CrossProduct(Dir, Camera - B).GetSafeNormal();
			const int32 I = Vertices.Num();
			for (const FVector& P : { A - SA * HA, A + SA * HA, B + SB * HB, B - SB * HB })
			{
				Vertices.Add(P - Base);
				Colors.Add(Color);
			}
			Triangles.Append({ I, I + 1, I + 2, I, I + 2, I + 3 });
		}

		void Polyline(const TArray<FVector>& L, const FLinearColor& Color)
		{
			for (int32 I = 0; I + 1 < L.Num(); ++I)
			{
				Line(L[I], L[I + 1], Color);
			}
		}
	};

	inline UMaterialInterface* OverlayMaterial()
	{
		static UMaterialInterface* M = [] { UMaterialInterface* L = LoadObject<UMaterialInterface>(nullptr, TEXT("/RoadMaker/Materials/M_RM_Overlay.M_RM_Overlay")); if (L) { L->AddToRoot(); } return L; }();
		return M;
	}

}
