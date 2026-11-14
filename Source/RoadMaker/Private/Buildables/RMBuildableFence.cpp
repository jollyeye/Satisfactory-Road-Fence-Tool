#include "Buildables/RMBuildableFence.h"
#include "RoadMakerModule.h"
#include "ProceduralMeshComponent.h"
#include "Net/UnrealNetwork.h"
#include "Buildables/RMBuildableRoad.h"
#include "FGFactoryColoringTypes.h"
#include "HAL/PlatformTime.h"
#include "FGRecipe.h"
#include "FGCharacterPlayer.h"
#include "FGInventoryComponent.h"
#include "FGGameState.h"
#include "FGCentralStorageSubsystem.h"
#include "FGDismantleInterface.h"
#include "Kismet/GameplayStatics.h"

ARMBuildableFence::ARMBuildableFence(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	mMeshStep = 50.f;
	mLODDistance = 6000.f; // 60 m: fences are thin, details vanish sooner
}

void ARMBuildableFence::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARMBuildableFence, mInwardSign);
	DOREPLIFETIME(ARMBuildableFence, mWallLook);
}

void ARMBuildableFence::RebuildMesh()
{
	// Long fences are drawn in sections (each switches detail on its own); every detail level gets a
	// ring exactly on each section boundary, so the sections meet without a gap.
	UpdateChunkLayout();
	TArray<FRMFrame> Frames;
	GetLocalFrames(Frames, mMeshStep);
	AddChunkBoundaries(Frames);
	mMesh->ClearAllMeshSections();
	PrepareChunks();
	ClearFarMesh();
	if (Frames.Num() < 2)
	{
		return;
	}

	FRMMesh Main, Panel;
	const ERMFenceType Type = GetEffectiveType();
	UMaterialInterface* MainMaterial = GetEffectiveMainMaterial();
	BuildFenceMesh(Type, Frames, mUVScale, mInwardSign, Main, Panel, GetWallLayout());

	ApplyNearSection(Main, 0, true, MainMaterial);
	ApplyNearSection(Panel, 1, true, mPanelMaterial);

	// Distance levels, like the game's walls (detailed, simpler, simplest): fence walls are big and
	// seen from further away, so they switch later than the road fences.
	mLODDistance = IsWall() ? 8000.f : 6000.f;   // 80 m / 60 m
	mLOD2Distance = IsWall() ? 20000.f : 15000.f; // 200 m / 150 m

	// Level 1: the same fence sampled every 4 m instead of every 50 cm.
	TArray<FRMFrame> FarFrames;
	GetLocalFrames(FarFrames, 400.f);
	AddChunkBoundaries(FarFrames);
	if (FarFrames.Num() >= 2)
	{
		FRMMesh FarMain, FarPanel;
		BuildFenceMesh(Type, FarFrames, mUVScale, mInwardSign, FarMain, FarPanel, GetWallLayout());
		ApplyFarSection(FarMain, 0, MainMaterial);
		ApplyFarSection(FarPanel, 1, mPanelMaterial);
	}

	// Level 2: the simplest shape, sampled every 8 m.
	TArray<FRMFrame> Far2Frames;
	GetLocalFrames(Far2Frames, 800.f);
	AddChunkBoundaries(Far2Frames);
	if (Far2Frames.Num() >= 2)
	{
		FRMMesh Far2Main, Far2Panel;
		BuildFenceFar2Mesh(Type, Far2Frames, mInwardSign, Far2Main, Far2Panel, GetWallLayout());
		ApplyFar2Section(Far2Main, 0, MainMaterial);
		ApplyFar2Section(Far2Panel, 1, mPanelMaterial);
	}
	ApplyPaintColors(mCustomizationData);
}

void ARMBuildableFence::GetWorldSplinePoints(TArray<FRMSplinePoint>& Out) const
{
	const FTransform T = GetActorTransform();
	Out.Reset();
	for (const FRMSplinePoint& P : mSplinePoints)
	{
		FRMSplinePoint W = P;
		W.Location = T.TransformPosition(P.Location);
		W.Tangent = T.TransformVector(P.Tangent);
		Out.Add(W);
	}
}

void ARMBuildableFence::ApplyCustomizationData_Implementation(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Implementation(customizationData);
	ApplyPaintColors(customizationData);
}

void ARMBuildableFence::ApplyCustomizationData_Native(const FFactoryCustomizationData& customizationData)
{
	Super::ApplyCustomizationData_Native(customizationData);
	ApplyPaintColors(customizationData);
}

void ARMBuildableFence::ApplyPaintColors(const FFactoryCustomizationData& Data)
{
	if (!mMesh || !mAllowColoring)
	{
		return;
	}
	ForEachMeshComponent([this, &Data](UProceduralMeshComponent* M)
	{
		ApplyPaintToMesh(M, Data);
	});
}

void ARMBuildableFence::ApplyPaintToMesh(UMeshComponent* M, const FFactoryCustomizationData& Data) const
{
	if (!M)
	{
		return;
	}
	const FResolvedFactoryCustomizationData Resolved(Data, mColorSlot);
	const FLinearColor P = Resolved.PrimaryColor;
	const FLinearColor S = Resolved.SecondaryColor;
	// The game's own way of passing paint (colours, finish, pattern) to its materials: used by
	// the game's wall material on the fence wall. Road Maker's materials read 24 / 28 below.
	Resolved.ApplyToMeshComponent(M);
	M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::PrimaryColorIndex, FVector4(P.R, P.G, P.B, 1.0));
	M->SetCustomPrimitiveDataVector4(ARMBuildableRoad::SecondaryColorIndex, FVector4(S.R, S.G, S.B, 1.0));
}

namespace
{
	/** Shape of a fence wall being swapped for another material, kept a moment for the new wall. */
	struct FPendingWallSwap
	{
		FTransform Transform;
		TArray<FRMSplinePoint> Points;
		float InwardSign = 1.f;
		int32 CostMultiplier = 1;
		double Time = 0.0;
	};
	TArray<FPendingWallSwap> GPendingWallSwaps;
}

uint8 ARMBuildableFence::LookOfType(ERMFenceType Type)
{
	switch (Type)
	{
	case ERMFenceType::ConcreteWall: return 1;
	case ERMFenceType::FicsitWall: return 2;
	case ERMFenceType::SteelWall: return 3;
	default: return 0;
	}
}

ERMFenceType ARMBuildableFence::GetEffectiveType() const
{
	if (!HasLooks())
	{
		return mFenceType;
	}
	switch (mPreviewWallLook ? mPreviewWallLook : mWallLook)
	{
	case 1: return ERMFenceType::ConcreteWall;
	case 2: return ERMFenceType::FicsitWall;
	case 3: return ERMFenceType::SteelWall;
	default: return mFenceType;
	}
}

UMaterialInterface* ARMBuildableFence::GetEffectiveMainMaterial() const
{
	const uint8 Look = HasLooks() ? (mPreviewWallLook ? mPreviewWallLook : mWallLook) : 0;
	if (Look > 0 && mWallLookMaterials.IsValidIndex(Look - 1) && mWallLookMaterials[Look - 1])
	{
		return mWallLookMaterials[Look - 1];
	}
	return mMainMaterial;
}

namespace
{
	/** Items the player can spend: their inventory plus the Dimensional Depot (central storage). */
	int32 AvailableItems(const UObject* Context, UFGInventoryComponent* Inventory, TSubclassOf<UFGItemDescriptor> Item)
	{
		int32 Count = Inventory ? Inventory->GetNumItems(Item) : 0;
		if (AFGCentralStorageSubsystem* Central = AFGCentralStorageSubsystem::Get(const_cast<UObject*>(Context)))
		{
			Count += Central->GetNumItemsFromCentralStorage(Item);
		}
		return Count;
	}

	/** Takes items from the inventory first, the rest from the Dimensional Depot. */
	void SpendItems(const UObject* Context, UFGInventoryComponent* Inventory, TSubclassOf<UFGItemDescriptor> Item, int32 Num)
	{
		const int32 FromInventory = Inventory ? FMath::Min(Num, Inventory->GetNumItems(Item)) : 0;
		if (FromInventory > 0)
		{
			Inventory->Remove(Item, FromInventory);
		}
		if (Num - FromInventory > 0)
		{
			if (AFGCentralStorageSubsystem* Central = AFGCentralStorageSubsystem::Get(const_cast<UObject*>(Context)))
			{
				Central->TryRemoveItemsFromCentralStorage(Item, Num - FromInventory);
			}
		}
	}

	/** Ingredients of a recipe, times a count, merged per item. */
	void AddRecipeCost(const UObject* Context, TSubclassOf<UFGRecipe> Recipe, int32 Times, TMap<TSubclassOf<UFGItemDescriptor>, int32>& Out)
	{
		if (!Recipe)
		{
			return;
		}
		for (const FItemAmount& A : UFGRecipe::GetIngredients(Context, Recipe))
		{
			Out.FindOrAdd(A.ItemClass) += A.Amount * Times;
		}
	}
}

void ARMBuildableFence::GetDismantleRefund_Implementation(TArray<FInventoryStack>& out_refund, bool noBuildCostEnabled) const
{
	const uint8 Look = GetShownLook();
	if (!HasLooks() || mWallLook == 0 || !mWallLookRecipes.IsValidIndex(Look - 1) || !mWallLookRecipes[Look - 1])
	{
		Super::GetDismantleRefund_Implementation(out_refund, noBuildCostEnabled);
		return;
	}
	// A switched wall gives back what its current look costs.
	if (noBuildCostEnabled)
	{
		return;
	}
	TMap<TSubclassOf<UFGItemDescriptor>, int32> Cost;
	AddRecipeCost(this, mWallLookRecipes[Look - 1], mCostMultiplier, Cost);
	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& C : Cost)
	{
		out_refund.Add(FInventoryStack(C.Value, C.Key));
	}
}

bool ARMBuildableFence::CanAffordLook(uint8 Look, AFGCharacterPlayer* Payer) const
{
	const AFGGameState* GameState = GetWorld() ? GetWorld()->GetGameState<AFGGameState>() : nullptr;
	UFGInventoryComponent* Inventory = Payer ? Payer->GetInventory() : nullptr;
	if (!Inventory || (GameState && GameState->GetCheatNoCost()) || Look < 1 || Look > 3
		|| !mWallLookRecipes.IsValidIndex(Look - 1) || !mWallLookRecipes.IsValidIndex(GetShownLook() - 1))
	{
		return true;
	}
	TMap<TSubclassOf<UFGItemDescriptor>, int32> Net;
	AddRecipeCost(this, mWallLookRecipes[Look - 1], mCostMultiplier, Net);
	TMap<TSubclassOf<UFGItemDescriptor>, int32> Refund;
	AddRecipeCost(this, mWallLookRecipes[GetShownLook() - 1], mCostMultiplier, Refund);
	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& R : Refund)
	{
		Net.FindOrAdd(R.Key) -= R.Value;
	}
	for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& N : Net)
	{
		if (N.Value > 0 && AvailableItems(this, Inventory, N.Key) < N.Value)
		{
			return false;
		}
	}
	return true;
}

bool ARMBuildableFence::SetWallLook(uint8 Look, AFGCharacterPlayer* Payer)
{
	if (!HasLooks() || Look < 1 || Look > 3 || Look == GetShownLook() || GetIsDismantled() || IsActorBeingDestroyed())
	{
		return false;
	}
	// Pay the difference: the new look's cost, minus what the old look gives back (nothing to pay
	// with the game's "no build cost" cheat / creative option).
	const AFGGameState* GameState = GetWorld() ? GetWorld()->GetGameState<AFGGameState>() : nullptr;
	const bool bFree = GameState && GameState->GetCheatNoCost();
	UFGInventoryComponent* Inventory = Payer && !bFree ? Payer->GetInventory() : nullptr;
	if (Inventory && mWallLookRecipes.IsValidIndex(Look - 1) && mWallLookRecipes.IsValidIndex(GetShownLook() - 1))
	{
		TMap<TSubclassOf<UFGItemDescriptor>, int32> Net;
		AddRecipeCost(this, mWallLookRecipes[Look - 1], mCostMultiplier, Net);
		TMap<TSubclassOf<UFGItemDescriptor>, int32> Refund;
		AddRecipeCost(this, mWallLookRecipes[GetShownLook() - 1], mCostMultiplier, Refund);
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& R : Refund)
		{
			Net.FindOrAdd(R.Key) -= R.Value;
		}
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& N : Net)
		{
			if (N.Value > 0 && AvailableItems(this, Inventory, N.Key) < N.Value)
			{
				return false; // cannot pay
			}
		}
		for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& N : Net)
		{
			if (N.Value > 0)
			{
				SpendItems(this, Inventory, N.Key, N.Value);
			}
			else if (N.Value < 0)
			{
				Inventory->AddStack(FInventoryStack(-N.Value, N.Key), true);
			}
		}
	}
	// Swap to the fence wall of that material, like the game's material swap: a new building of the
	// right type (name, recipe, refund) with this wall's course, side and paint; this one goes away.
	UClass* NewClass = mWallLookClasses.IsValidIndex(Look - 1) ? mWallLookClasses[Look - 1].Get() : nullptr;
	UWorld* World = GetWorld();
	if (NewClass && NewClass != GetClass() && World)
	{
		const FTransform T = GetActorTransform();
		ARMBuildableFence* New = World->SpawnActorDeferred<ARMBuildableFence>(NewClass, T, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (New)
		{
			if (mWallLookRecipes.IsValidIndex(Look - 1))
			{
				New->SetBuiltWithRecipe(mWallLookRecipes[Look - 1]);
			}
			New->SetInwardSign(mInwardSign);
			New->SetCostMultiplier(mCostMultiplier);
			New->SetSplinePoints(mSplinePoints);
			New->SetCustomizationData_Native(mCustomizationData);
			UGameplayStatics::FinishSpawningActor(New, T);
			// Out of the way at once (the dismantle takes a moment): aiming must find the new wall.
			SetActorEnableCollision(false);
			SetActorHiddenInGame(true);
			IFGDismantleInterface::Execute_Dismantle(this);
			return true;
		}
	}
	// No class for that look (older setup): switch the look on this wall.
	mWallLook = Look == LookOfType(mFenceType) ? 0 : Look;
	RebuildMesh();
	ForceNetUpdate();
	return true;
}

void ARMBuildableFence::SetPreviewWallLook(uint8 Look)
{
	if (!HasLooks() || Look == mPreviewWallLook)
	{
		return;
	}
	mPreviewWallLook = Look;
	RebuildMesh();
}

FRMWallTexLayout ARMBuildableFence::GetWallLayout() const
{
	// The layout belongs to the game material of the look shown (a wall can switch looks).
	FRMWallTexLayout L;
	if (GetEffectiveType() == ERMFenceType::FicsitWall)
	{
		// FICSIT look: the game's 1 m FICSIT wall material (MI_WallSetOrange_8x1). Its texture is a
		// 4 m x 1 m tile (256 texels per metre) that repeats sideways; on the game's 4 m wall the
		// grooves fall at 5.5 %, 43.5 %, 55.5 % and 93.5 % of 8 m, i.e. 0.11 / 0.87 of a 4 m tile.
		// Its top and bottom edges break the grooves, so it is not repeated upwards: the 4 m height
		// uses the plain middle band of the tile (v 0.3 to 0.7), where the grooves run straight.
		L.UCentre = 0.f;
		L.UPerMetre = 0.25f;
		L.VTop = 0.3f;
		L.VPerMetre = 0.1f;
		L.bMirror = false;
		return L;
	}
	L.UCentre = mWallUCentre;
	L.UPerMetre = mWallUPerMetre;
	L.VTop = mWallVTop;
	L.VPerMetre = mWallVPerMetre;
	L.bMirror = mWallMirror;
	if (mFenceType != ERMFenceType::ConcreteWall && GetEffectiveType() == ERMFenceType::ConcreteWall)
	{
		L = FRMWallTexLayout(); // concrete look on another variant: the concrete layout (the defaults)
	}
	return L;
}

void ARMBuildableFence::CopyShapeTo(ARMBuildableFence* Other) const
{
	if (!Other || Other == this)
	{
		return;
	}
	Other->SetActorTransform(GetActorTransform());
	Other->SetInwardSign(mInwardSign);
	Other->SetCostMultiplier(mCostMultiplier);
	Other->SetSplinePoints(mSplinePoints);
	Other->RebuildMesh();
	Other->ForceNetUpdate();
}

void ARMBuildableFence::PreUpgrade_Implementation()
{
	Super::PreUpgrade_Implementation();
	if (!IsWall())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	GPendingWallSwaps.RemoveAll([Now](const FPendingWallSwap& S) { return Now - S.Time > 5.0; });
	FPendingWallSwap Swap;
	Swap.Transform = GetActorTransform();
	Swap.Points = mSplinePoints;
	Swap.InwardSign = mInwardSign;
	Swap.CostMultiplier = mCostMultiplier;
	Swap.Time = Now;
	GPendingWallSwaps.Add(Swap);
}

void ARMBuildableFence::Upgrade_Implementation(AActor* newActor)
{
	Super::Upgrade_Implementation(newActor);
	if (ARMBuildableFence* Other = Cast<ARMBuildableFence>(newActor))
	{
		CopyShapeTo(Other);
	}
}

void ARMBuildableFence::BeginPlay()
{
	// A fence wall built by a material swap may arrive without a shape: take the swapped wall's.
	if (HasAuthority() && IsWall() && mSplinePoints.Num() < 2)
	{
		const double Now = FPlatformTime::Seconds();
		const int32 Index = GPendingWallSwaps.IndexOfByPredicate([this, Now](const FPendingWallSwap& S)
		{
			return Now - S.Time < 5.0 && FVector::Dist(S.Transform.GetLocation(), GetActorLocation()) < 100.f;
		});
		if (Index != INDEX_NONE)
		{
			const FPendingWallSwap Swap = GPendingWallSwaps[Index];
			GPendingWallSwaps.RemoveAt(Index);
			SetActorTransform(Swap.Transform);
			mInwardSign = Swap.InwardSign;
			mCostMultiplier = Swap.CostMultiplier;
			SetSplinePoints(Swap.Points);
		}
	}
	Super::BeginPlay();
}

void ARMBuildableFence::BuildFenceFar2Mesh(ERMFenceType Type, TArray<FRMFrame> Frames, float InwardSign, FRMMesh& Main, FRMMesh& Panel,
	const FRMWallTexLayout& WallLayout)
{
	if (InwardSign < 0.f)
	{
		RMGeometry::MirrorFrames(Frames);
	}
	for (FRMFrame& F : Frames)
	{
		F.Up = FVector::UpVector;
		F.Right = FVector(F.Right.X, F.Right.Y, 0.f).GetSafeNormal();
	}
	auto Lifted = [&Frames](float Z)
	{
		TArray<FRMFrame> Out = Frames;
		for (FRMFrame& F : Out)
		{
			F.Pos.Z += Z;
		}
		return Out;
	};
	switch (Type)
	{
	case ERMFenceType::GuardRail:
	{
		// The beam as a plain bar (posts dropped).
		const TArray<FVector2D> Bar = { FVector2D(-4.f, -21.f), FVector2D(5.f, -21.f), FVector2D(5.f, 21.f), FVector2D(-4.f, 21.f) };
		const TArray<FRMFrame> Rail = Lifted(61.f);
		RMGeometry::ExtrudeProfile(Rail, Bar, true, FVector2D(0.5f, 0.f), 400.f, Main);
		break;
	}
	case ERMFenceType::JerseyBarrier:
	{
		// A plain wedge; UV1.y = 1 tells the material "no block joints here".
		const TArray<FVector2D> Wedge = { FVector2D(-30.f, 0.f), FVector2D(30.f, 0.f), FVector2D(7.5f, 80.f), FVector2D(-7.5f, 80.f) };
		RMGeometry::ExtrudeProfile(Frames, Wedge, true, FVector2D(0.f, 30.f), 400.f, Main);
		RMGeometry::AddProfileCap(Frames[0], Wedge, false, 400.f, Main);
		RMGeometry::AddProfileCap(Frames.Last(), Wedge, true, 400.f, Main);
		Main.MarkUV1.Init(FVector2D(0.f, 1.f), Main.Vertices.Num());
		Main.MarkUV2.Init(FVector2D(2.f, 0.f), Main.Vertices.Num());
		break;
	}
	case ERMFenceType::GridFence:
	case ERMFenceType::GridFenceTall:
	{
		// The mesh sheet only (UV1.x = metres along, as the grid material wants), plus a thin top bar.
		const float Height = Type == ERMFenceType::GridFenceTall ? 400.f : 200.f;
		RMGeometry::ExtrudeProfile(Frames, { FVector2D(-3.f, Height), FVector2D(-3.f, 8.f) }, false, FVector2D(7.f, Height * 0.5f), 200.f, Panel);
		Panel.MarkUV1.SetNumZeroed(Panel.Vertices.Num());
		for (int32 I = 0; I < Panel.Vertices.Num(); ++I)
		{
			Panel.MarkUV1[I] = FVector2D(Panel.UVs[I].Y * 2.f, 0.f);
		}
		const TArray<FVector2D> Bar = { FVector2D(-6.f, Height - 6.f), FVector2D(0.f, Height - 6.f), FVector2D(0.f, Height + 2.f), FVector2D(-6.f, Height + 2.f) };
		RMGeometry::ExtrudeProfile(Frames, Bar, true, FVector2D(-3.f, Height - 2.f), 400.f, Main);
		break;
	}
	case ERMFenceType::SteelWall:
	{
		// A plain slab on the ribbed part of the steel wall texture.
		FRMWallTexLayout Steel;
		Steel.UCentre = 0.3f;
		Steel.UPerMetre = 0.f;
		Steel.VTop = 0.2f;
		Steel.VPerMetre = 0.175f;
		Steel.bMirror = false;
		RMGeometry::BuildFenceWall(Frames, 400.f, Main, GetWallHeight(), 50.f, Steel);
		break;
	}
	case ERMFenceType::ConcreteWall:
	case ERMFenceType::FicsitWall:
		RMGeometry::BuildFenceWall(Frames, 400.f, Main, GetWallHeight(), 50.f, WallLayout);
		break;
	}
}

void ARMBuildableFence::BuildFenceMesh(ERMFenceType Type, TArray<FRMFrame> Frames, float UVScale, float InwardSign, FRMMesh& Main, FRMMesh& Panel,
	const FRMWallTexLayout& WallLayout)
{
	if (InwardSign < 0.f)
	{
		RMGeometry::MirrorFrames(Frames);
	}
	switch (Type)
	{
	case ERMFenceType::GuardRail:
		RMGeometry::BuildGuardRail(Frames, UVScale, Main, Panel);
		break;
	case ERMFenceType::JerseyBarrier:
		RMGeometry::BuildJerseyBarrier(Frames, UVScale, Main);
		break;
	case ERMFenceType::GridFence:
		RMGeometry::BuildGridFence(Frames, UVScale, Main, Panel);
		break;
	case ERMFenceType::GridFenceTall:
		RMGeometry::BuildGridFence(Frames, UVScale, Main, Panel, 2.f);
		break;
	case ERMFenceType::ConcreteWall:
		RMGeometry::BuildFenceWall(Frames, UVScale, Main, GetWallHeight(), 50.f, WallLayout);
		break;
	case ERMFenceType::SteelWall:
		RMGeometry::BuildSteelFenceWall(Frames, Main, GetWallHeight());
		break;
	case ERMFenceType::FicsitWall:
		RMGeometry::BuildFenceWall(Frames, UVScale, Main, GetWallHeight(), 50.f, WallLayout); // shape to follow
		break;
	}
}

void ARMBuildableFence::GetCutSection(float& OutHalfWidth, float& OutBottom, float& OutTop) const
{
	// Rough cross-section of each fence type (the spline runs along its foot).
	OutBottom = -5.f;
	switch (GetEffectiveType())
	{
	case ERMFenceType::GuardRail:
		OutHalfWidth = 20.f;
		OutTop = 90.f;
		break;
	case ERMFenceType::JerseyBarrier:
		OutHalfWidth = 32.f;
		OutTop = 85.f;
		break;
	case ERMFenceType::GridFence:
		OutHalfWidth = 12.f;
		OutTop = 205.f;
		break;
	default: // fence walls (4 m)
		OutHalfWidth = 28.f;
		OutTop = GetWallHeight() + 5.f;
		break;
	}
}

void ARMBuildableFence::CopyCutSettingsTo(ARMBuildableSpline* New) const
{
	if (ARMBuildableFence* Fence = Cast<ARMBuildableFence>(New))
	{
		Fence->mInwardSign = mInwardSign;
		Fence->mWallLook = mWallLook;
	}
}

ARMBuildableFence::EJoinCheck ARMBuildableFence::CheckJoin(const ARMBuildableFence* Other, int32 MyEnd, int32 OtherEnd, TArray<FRMFrame>* OutFrames) const
{
	if (!Other || Other == this)
	{
		return EJoinCheck::NotJoined;
	}
	const ERMJoinCourse Course = JoinCourse(Other, MyEnd, OtherEnd, MaxJoinLength, OutFrames);
	if (Course == ERMJoinCourse::NotJoined)
	{
		return EJoinCheck::NotJoined;
	}
	if (GetEffectiveType() != Other->GetEffectiveType() || GetShownLook() != Other->GetShownLook())
	{
		return EJoinCheck::Type;
	}
	// The road side, seen along the joined course: a fence met head-on (end 0 to end 0, or 1 to 1)
	// runs the other way, so its side flips.
	const float OtherSide = (MyEnd == OtherEnd ? -1.f : 1.f) * Other->GetInwardSign();
	if (OtherSide != GetInwardSign())
	{
		return EJoinCheck::Side;
	}
	switch (Course)
	{
	case ERMJoinCourse::Ok: return EJoinCheck::Ok;
	case ERMJoinCourse::Kink: return EJoinCheck::Kink;
	case ERMJoinCourse::Curve: return EJoinCheck::Curve;
	case ERMJoinCourse::TooLong: return EJoinCheck::TooLong;
	default: return EJoinCheck::NotJoined;
	}
}

ARMBuildableFence* ARMBuildableFence::JoinWith(ARMBuildableFence* Other, int32 MyEnd, int32 OtherEnd)
{
	if (!HasAuthority() || !Other || GetIsDismantled() || Other->GetIsDismantled() || IsActorBeingDestroyed() || Other->IsActorBeingDestroyed())
	{
		return nullptr;
	}
	TArray<FRMFrame> Path;
	if (CheckJoin(Other, MyEnd, OtherEnd, &Path) != EJoinCheck::Ok || Path.Num() < 2)
	{
		return nullptr;
	}
	// Refitted through both courses (keys every 2 m, exact ends and end directions), heights as they
	// are. The course runs the way this fence runs, so its side carries over as it is.
	TArray<FRMSplinePoint> Points;
	RMGeometry::OffsetCurve(Path, 0.f, 200.f, Points);
	FVector JointLoc = GetActorLocation();
	{
		TArray<FRMFrame> Mine;
		GetWorldFrames(Mine, 100.f);
		if (Mine.Num() >= 2)
		{
			JointLoc = MyEnd == 0 ? Mine[0].Pos : Mine.Last().Pos;
		}
	}
	ARMBuildableFence* Joined = Cast<ARMBuildableFence>(SpawnLike(GetClass(), GetBuiltWithRecipe(), Points,
		FMath::Max(1, GetCostMultiplier() + Other->GetCostMultiplier()), ERMRevealFrom::FromPoint, JointLoc));
	if (!Joined)
	{
		return nullptr;
	}
	for (ARMBuildableFence* Old : { this, Other })
	{
		Old->SetActorEnableCollision(false);
		Old->SetActorHiddenInGame(true);
		IFGDismantleInterface::Execute_Dismantle(Old);
	}
	return Joined;
}
