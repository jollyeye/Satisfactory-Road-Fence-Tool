// Development logger (temporary): dumps the build menu window's widget tree to FactoryGame.log
// ("[Diag]" lines) the first time each build menu widget is seen, to find where its top tabs
// (Build / Blueprints) live. Remove before release (also its InstallHooks call in RoadMakerModule.cpp).
#include "RoadMakerModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Engine/Engine.h"
#include "Containers/Ticker.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace RMDiag
{
	void InstallHooks();
}

#if !WITH_EDITOR
namespace
{
	TSet<FString> GDiagDumped;

	FString DiagWidgetLine(UWidget* W)
	{
		FString S = FString::Printf(TEXT("%s (%s) vis=%d"), *W->GetName(), *W->GetClass()->GetName(), (int32)W->GetVisibility());
		if (UTextBlock* T = Cast<UTextBlock>(W))
		{
			S += TEXT(" text='") + T->GetText().ToString() + TEXT("'");
		}
		if (UImage* I = Cast<UImage>(W))
		{
			S += TEXT(" res=") + GetNameSafe(I->GetBrush().GetResourceObject());
		}
		if (W->Slot)
		{
			S += TEXT(" slot=") + W->Slot->GetClass()->GetName();
		}
		return S;
	}

	void DiagClassInfo(UClass* Class, const FString& Indent)
	{
		// Blueprint-declared functions and properties (where tab logic lives), not the engine's.
		FString Fns;
		for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			Fns += It->GetName() + TEXT(" ");
		}
		FString Props;
		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			Props += It->GetName() + TEXT(":") + It->GetCPPType() + TEXT(" ");
		}
		UE_LOG(LogRoadMaker, Display, TEXT("[Diag] %s  class %s parent %s"), *Indent, *Class->GetName(), *GetNameSafe(Class->GetSuperClass()));
		UE_LOG(LogRoadMaker, Display, TEXT("[Diag] %s  functions: %s"), *Indent, *Fns.Left(3000));
		UE_LOG(LogRoadMaker, Display, TEXT("[Diag] %s  properties: %s"), *Indent, *Props.Left(3000));
	}

	void DiagDump(UWidget* W, int32 Depth, TSet<UClass*>& SeenClasses)
	{
		if (!W || Depth > 16)
		{
			return;
		}
		const FString Indent = FString::ChrN(Depth * 2, TEXT(' '));
		UE_LOG(LogRoadMaker, Display, TEXT("[Diag] %s%s"), *Indent, *DiagWidgetLine(W));
		if (UUserWidget* U = Cast<UUserWidget>(W))
		{
			if (!SeenClasses.Contains(U->GetClass()))
			{
				SeenClasses.Add(U->GetClass());
				DiagClassInfo(U->GetClass(), Indent);
			}
			if (U->WidgetTree && U->WidgetTree->RootWidget)
			{
				DiagDump(U->WidgetTree->RootWidget, Depth + 1, SeenClasses);
			}
			return;
		}
		if (UPanelWidget* P = Cast<UPanelWidget>(W))
		{
			for (int32 I = 0; I < P->GetChildrenCount(); ++I)
			{
				DiagDump(P->GetChildAt(I), Depth + 1, SeenClasses);
			}
		}
	}

	bool DiagTick(float)
	{
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* W = *It;
			if (!W || W->IsTemplate() || !W->IsInViewport())
			{
				continue;
			}
			const FString ClassName = W->GetClass()->GetName();
			if (!ClassName.Contains(TEXT("BuildMenu")) || GDiagDumped.Contains(ClassName))
			{
				continue;
			}
			GDiagDumped.Add(ClassName);
			UE_LOG(LogRoadMaker, Display, TEXT("[Diag] ===== build menu widget %s (%s) ====="), *W->GetName(), *ClassName);
			TSet<UClass*> Seen;
			DiagDump(W, 0, Seen);
		}
		return true;
	}
}

void RMDiag::InstallHooks()
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&DiagTick), 0.5f);
	UE_LOG(LogRoadMaker, Display, TEXT("[Diag] development logger on (build menu dump)"));
}
#else
void RMDiag::InstallHooks() {}
#endif
