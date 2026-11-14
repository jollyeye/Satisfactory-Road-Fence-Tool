// Investigation helper: saves game textures as PNG files next to the game's logs (Saved/Logs/RoadMaker_<name>.png),
// to see the real pictures (the modding project only has small placeholders). Run once per game start for the
// textures in GDumpList (only if the file is not there yet), or any texture with the console: RM.DumpTexture <path>.
#include "Debug/RMTextureDump.h"
#include "RoadMakerModule.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "TextureResource.h"
#include "ImageUtils.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* GDumpList[] = {
		TEXT("/Game/FactoryGame/Buildable/Factory/ConveyorPole/Texture/TX_ConveyorPole_BC.TX_ConveyorPole_BC"),
		TEXT("/Game/FactoryGame/Buildable/Factory/ConveyorPole/Texture/TX_ConveyorPole_Refl.TX_ConveyorPole_Refl"),
		TEXT("/Game/FactoryGame/Buildable/Factory/SignDigital/Texture/TX_DigitalSign_BC.TX_DigitalSign_BC"),
		TEXT("/Game/FactoryGame/Buildable/Factory/SignDigital/Texture/TX_DigitalSign_Refl.TX_DigitalSign_Refl"),
	};

	FString DumpPath(const UTexture2D* T)
	{
		return FPaths::Combine(FPaths::ProjectLogDir(), FString::Printf(TEXT("RoadMaker_%s.png"), *T->GetName()));
	}

	bool Dump(UWorld* World, UTexture2D* T)
	{
		if (!World || !T)
		{
			return false;
		}
		T->SetForceMipLevelsToBeResident(30.f);
		T->WaitForStreaming();
		const int32 W = FMath::Clamp(T->GetSizeX(), 1, 2048);
		const int32 H = FMath::Clamp(T->GetSizeY(), 1, 2048);
		UTextureRenderTarget2D* RT = UKismetRenderingLibrary::CreateRenderTarget2D(World, W, H, T->SRGB ? RTF_RGBA8_SRGB : RTF_RGBA8, FLinearColor::Black);
		if (!RT)
		{
			return false;
		}
		UCanvas* Canvas = nullptr;
		FVector2D Size;
		FDrawToRenderTargetContext Ctx;
		UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, RT, Canvas, Size, Ctx);
		if (Canvas)
		{
			Canvas->K2_DrawTexture(T, FVector2D::ZeroVector, Size, FVector2D::ZeroVector, FVector2D::UnitVector, FLinearColor::White, BLEND_Opaque);
		}
		UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Ctx);
		TArray<FColor> Pixels;
		FTextureRenderTargetResource* Res = RT->GameThread_GetRenderTargetResource();
		if (!Res || !Res->ReadPixels(Pixels) || Pixels.Num() != W * H)
		{
			return false;
		}
		for (FColor& C : Pixels)
		{
			C.A = 255;
		}
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(W, H, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
		const FString Path = DumpPath(T);
		const bool bOk = FFileHelper::SaveArrayToFile(Png, *Path);
		UE_LOG(LogRoadMaker, Display, TEXT("Texture dump: %s (%dx%d, srgb %d) -> %s %s"), *T->GetPathName(), W, H, T->SRGB ? 1 : 0, *Path,
			bOk ? TEXT("saved") : TEXT("FAILED"));
		return bOk;
	}

	FAutoConsoleCommandWithWorldAndArgs GRMDumpTexture(
		TEXT("RM.DumpTexture"),
		TEXT("Road Maker: save a texture (object path) as a PNG next to the logs."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			for (const FString& A : Args)
			{
				Dump(World, LoadObject<UTexture2D>(nullptr, *A));
			}
		}));
}

void RMTextureDump::DumpListOnce(UWorld* World)
{
	static bool bDone = false;
	if (bDone || !World || World->WorldType != EWorldType::Game || !World->GetFirstPlayerController())
	{
		return;
	}
	bDone = true;
	for (const TCHAR* P : GDumpList)
	{
		UTexture2D* T = LoadObject<UTexture2D>(nullptr, P);
		if (!T)
		{
			UE_LOG(LogRoadMaker, Display, TEXT("Texture dump: %s not found"), P);
			continue;
		}
		if (!IFileManager::Get().FileExists(*DumpPath(T)))
		{
			Dump(World, T);
		}
	}
}
