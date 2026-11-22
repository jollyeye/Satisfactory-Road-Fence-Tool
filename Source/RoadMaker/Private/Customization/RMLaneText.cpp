#include "Customization/RMLaneText.h"
#include "RoadMakerModule.h"
#include "RMGeometry.h"
#include "Buildables/RMBuildableMarking.h"
#include "Buildables/RMBuildableRoad.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

namespace
{
	constexpr int32 GlyphTextureSize = 2048;
	constexpr int32 GlyphCell = 128;
	constexpr int32 GlyphCellsPerRow = GlyphTextureSize / GlyphCell;
	constexpr int32 GlyphCells = GlyphCellsPerRow * GlyphCellsPerRow;

	TStrongObjectPtr<UTextureRenderTarget2D> GGlyphTexture;
	TMap<TCHAR, int32> GGlyphCells;
	TWeakObjectPtr<UFont> GGlyphFont;

	TAutoConsoleVariable<FString> CVarLaneTextFont(TEXT("RM.LaneTextFont"), TEXT(""),
		TEXT("Lane Text: name (part) of the font to draw the characters with; empty = chosen automatically (logged)."));

	/**
	 * The font the characters are drawn with: one the game has loaded for its UI (it has Chinese / Japanese), a runtime
	 * (composite) font so missing characters fall back. Every candidate is logged once.
	 */
	UFont* PickLaneTextFont()
	{
		const FString Wanted = CVarLaneTextFont.GetValueOnGameThread();
		if (GGlyphFont.IsValid() && (Wanted.IsEmpty() || GGlyphFont->GetName().Contains(Wanted)))
		{
			return GGlyphFont.Get();
		}
		TArray<UFont*> Runtime;
		TArray<FString> Names;
		for (TObjectIterator<UFont> It; It; ++It)
		{
			UFont* F = *It;
			if (!F || F->IsTemplate())
			{
				continue;
			}
			const bool bRuntime = F->FontCacheType == EFontCacheType::Runtime;
			Names.Add(FString::Printf(TEXT("%s%s"), *F->GetPathName(), bRuntime ? TEXT(" (runtime)") : TEXT("")));
			if (bRuntime)
			{
				Runtime.Add(F);
			}
		}
		static bool bListed = false;
		if (!bListed)
		{
			bListed = true;
			UE_LOG(LogRoadMaker, Display, TEXT("Lane Text: fonts loaded: %s"), *FString::Join(Names, TEXT(", ")));
		}
		UFont* Best = nullptr;
		auto Pick = [&](TFunctionRef<bool(const FString&)> Test)
		{
			for (UFont* F : Runtime)
			{
				if (!Best && Test(F->GetName()))
				{
					Best = F;
				}
			}
		};
		if (!Wanted.IsEmpty())
		{
			Pick([&](const FString& N) { return N.Contains(Wanted); });
		}
		Pick([](const FString& N) { return N.Contains(TEXT("Noto")) && N.Contains(TEXT("Bold")); });
		Pick([](const FString& N) { return N.Contains(TEXT("Bold")); });
		Pick([](const FString&) { return true; });
		if (!Best && GEngine)
		{
			Best = GEngine->GetLargeFont();
		}
		if (Best != GGlyphFont.Get())
		{
			UE_LOG(LogRoadMaker, Display, TEXT("Lane Text: draws with font %s"), *GetPathNameSafe(Best));
			GGlyphFont = Best;
			GGlyphCells.Reset(); // another font: draw again
		}
		return Best;
	}
}

UTextureRenderTarget2D* RMLaneText::GetGlyphTexture(UWorld* World)
{
	if (!GGlyphTexture.IsValid() && World)
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), TEXT("RM_LaneTextGlyphs"));
		RT->RenderTargetFormat = RTF_RGBA8;
		// Opaque black: the icon paint cuts by red x alpha, and the canvas writes the characters' colour, not alpha
		// (with a transparent clear nothing showed, 2026-11-22): so alpha stays 1 and the white characters decide.
		RT->ClearColor = FLinearColor(0.f, 0.f, 0.f, 1.f);
		RT->bAutoGenerateMips = false;
		RT->InitAutoFormat(GlyphTextureSize, GlyphTextureSize);
		RT->UpdateResourceImmediate(true);
		GGlyphTexture.Reset(RT);
		GGlyphCells.Reset();
	}
	return GGlyphTexture.Get();
}

bool RMLaneText::EnsureGlyphs(UWorld* World, const FString& Text)
{
	UTextureRenderTarget2D* RT = GetGlyphTexture(World);
	UFont* Font = PickLaneTextFont();
	if (!RT || !Font || !World)
	{
		return false;
	}
	TArray<TCHAR> New;
	for (TCHAR C : Text)
	{
		if (!GGlyphCells.Contains(C) && !New.Contains(C))
		{
			New.Add(C);
		}
	}
	if (New.Num() == 0)
	{
		return true;
	}
	if (GGlyphCells.Num() == 0)
	{
		UKismetRenderingLibrary::ClearRenderTarget2D(World, RT, FLinearColor(0.f, 0.f, 0.f, 1.f));
	}
	UCanvas* Canvas = nullptr;
	FVector2D Size;
	FDrawToRenderTargetContext Context;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, RT, Canvas, Size, Context);
	if (!Canvas)
	{
		return false;
	}
	for (TCHAR C : New)
	{
		const int32 Index = GGlyphCells.Num();
		if (Index >= GlyphCells)
		{
			UE_LOG(LogRoadMaker, Warning, TEXT("Lane Text: the glyph texture is full (%d characters)"), GlyphCells);
			break;
		}
		GGlyphCells.Add(C, Index);
		if (FChar::IsWhitespace(C))
		{
			continue; // an empty cell
		}
		const FString Str(1, &C);
		const FVector2D Measured = Canvas->K2_TextSize(Font, Str, FVector2D(1.f, 1.f));
		// As large as the cell allows (a margin round it), so every character is as tall as it can be.
		const float Fit = GlyphCell * 0.86f;
		const float Scale = FMath::Min(Fit / FMath::Max(Measured.Y, 1.f), Fit / FMath::Max(Measured.X, 1.f));
		const FVector2D Centre((Index % GlyphCellsPerRow + 0.5f) * GlyphCell, (Index / GlyphCellsPerRow + 0.5f) * GlyphCell);
		Canvas->K2_DrawText(Font, Str, Centre, FVector2D(Scale, Scale), FLinearColor::White, 0.f, FLinearColor::Transparent,
			FVector2D::ZeroVector, true, true, false, FLinearColor::Transparent);
	}
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Context);
	// Check: the brightest red and its alpha over the first new character's cell (a few pixels read back).
	if (const int32* First = GGlyphCells.Find(New[0]))
	{
		float MaxR = 0.f, AtA = 0.f;
		const int32 X0 = (*First % GlyphCellsPerRow) * GlyphCell, Y0 = (*First / GlyphCellsPerRow) * GlyphCell;
		for (int32 Y = 16; Y < GlyphCell; Y += 16)
		{
			for (int32 X = 16; X < GlyphCell; X += 16)
			{
				const FColor P = UKismetRenderingLibrary::ReadRenderTargetPixel(World, RT, X0 + X, Y0 + Y);
				if (P.R / 255.f > MaxR)
				{
					MaxR = P.R / 255.f;
					AtA = P.A / 255.f;
				}
			}
		}
		UE_LOG(LogRoadMaker, Display, TEXT("Lane Text: drew %d characters (%d in the texture); '%c': brightest red %.2f, alpha %.2f"),
			New.Num(), GGlyphCells.Num(), New[0], MaxR, AtA);
	}
	return true;
}

FBox2D RMLaneText::CellUV(TCHAR Char)
{
	const int32* Found = GGlyphCells.Find(Char);
	const int32 Index = Found ? *Found : 0;
	const float Cell = 1.f / GlyphCellsPerRow;
	const FVector2D Min((Index % GlyphCellsPerRow) * Cell, (Index / GlyphCellsPerRow) * Cell);
	return FBox2D(Min, Min + FVector2D(Cell, Cell));
}

namespace
{
	/**
	 * Test (step 1): "RM.LaneText <text>" paints the text on the lane aimed at as a temporary preview (not a building,
	 * not saved); "RM.LaneText cyber <text>" with the cyberpunk glow; "RM.LaneText" alone clears it.
	 */
	TWeakObjectPtr<AActor> GLaneTextPreview;

	FAutoConsoleCommandWithWorldAndArgs GRMLaneText(
		TEXT("RM.LaneText"),
		TEXT("Test: paint text along the lane aimed at (preview only). RM.LaneText [cyber] <text>; no text clears."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (GLaneTextPreview.IsValid())
			{
				GLaneTextPreview->Destroy();
			}
			TArray<FString> Words = Args;
			const bool bCyber = Words.Num() > 0 && Words[0].Equals(TEXT("cyber"), ESearchCase::IgnoreCase);
			if (bCyber)
			{
				Words.RemoveAt(0);
			}
			const FString Text = FString::Join(Words, TEXT(" "));
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			if (Text.IsEmpty() || !PC)
			{
				return;
			}
			FVector Eye;
			FRotator View;
			PC->GetPlayerViewPoint(Eye, View);
			FHitResult Hit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(RMLaneText), true, PC->GetPawn());
			if (!World->LineTraceSingleByChannel(Hit, Eye, Eye + View.Vector() * 20000.f, ECC_Visibility, Params))
			{
				UE_LOG(LogRoadMaker, Display, TEXT("Lane Text: aim at a road"));
				return;
			}
			ARMBuildableRoad* Road = ARMBuildableMarking::FindRoadAt(World, Hit.Location);
			FTransform T;
			float LaneWidth = 200.f;
			FRMMarkingDrape Probe;
			if (!Road || !ARMBuildableMarking::PlanRange(Road, Hit.Location, View.Vector(), 0.f, 0.f, T, LaneWidth, Probe))
			{
				UE_LOG(LogRoadMaker, Display, TEXT("Lane Text: aim at a road"));
				return;
			}
			const float Length = ARMBuildableMarking::TextLength(Text.Len(), LaneWidth);
			FRMMarkingDrape Drape;
			ARMBuildableMarking::PlanRange(Road, Hit.Location, View.Vector(), -25.f, Length + 25.f, T, LaneWidth, Drape);
			if (!RMLaneText::EnsureGlyphs(World, Text))
			{
				UE_LOG(LogRoadMaker, Warning, TEXT("Lane Text: could not draw the characters"));
				return;
			}
			FRMMesh Mesh;
			ARMBuildableMarking::BuildTextMesh(Text.Len(), [&Text](int32 I) { return RMLaneText::CellUV(Text[I]); }, LaneWidth, Mesh, &Drape);

			AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), T);
			UProceduralMeshComponent* Comp = NewObject<UProceduralMeshComponent>(Actor, TEXT("LaneText"));
			Actor->SetRootComponent(Comp);
			Comp->RegisterComponent();
			Comp->SetWorldTransform(T);
			Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Comp->CreateMeshSection(0, Mesh.Vertices, Mesh.Triangles, Mesh.Normals, Mesh.UVs, TArray<FColor>(), Mesh.Tangents, false);
			UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, bCyber
				? TEXT("/RoadMaker/Materials/M_RoadMarkingIconCyber.M_RoadMarkingIconCyber") : TEXT("/RoadMaker/Materials/M_RoadMarkingIcon.M_RoadMarkingIcon"));
			if (Base)
			{
				UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Base, Comp);
				MID->SetTextureParameterValue(TEXT("Icon"), RMLaneText::GetGlyphTexture(World));
				Comp->SetMaterial(0, MID);
			}
			Comp->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(0.8, 0.8, 0.76, 1.0));
			Comp->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(0.0, 0.85, 1.0, 1.0));
			GLaneTextPreview = Actor;
			UE_LOG(LogRoadMaker, Display, TEXT("Lane Text: \"%s\" on %s, lane %.0f cm, %.0f cm long, material %s"), *Text, *Road->GetName(),
				LaneWidth, Length, *GetNameSafe(Base));
		}));
}
