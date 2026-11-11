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

	TSet<FString> GDiagSeenClasses;
	TMap<FString, FString> GDiagLast;

	void DiagSet(const FString& Key, const FString& Value)
	{
		FString* Old = GDiagLast.Find(Key);
		if (!Old || *Old != Value)
		{
			UE_LOG(LogRoadMaker, Display, TEXT("[Diag] %s = %s"), *Key, *Value);
			GDiagLast.Add(Key, Value);
		}
	}

	/** Parameters of a Blueprint function: name:type, return marked. */
	void DiagSignature(UClass* Class, const TCHAR* FnName)
	{
		UFunction* Fn = Class ? Class->FindFunctionByName(FnName) : nullptr;
		if (!Fn)
		{
			UE_LOG(LogRoadMaker, Display, TEXT("[Diag] sig %s.%s: not found"), *GetNameSafe(Class), FnName);
			return;
		}
		FString S;
		for (TFieldIterator<FProperty> It(Fn); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			S += FString::Printf(TEXT("%s%s:%s "), It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm) ? TEXT("out ") : TEXT(""), *It->GetName(), *It->GetCPPType());
		}
		UE_LOG(LogRoadMaker, Display, TEXT("[Diag] sig %s.%s(%s) ParmsSize=%d"), *Class->GetName(), FnName, *S, (int32)Fn->ParmsSize);
	}

	/** A property's value as text (objects by name, arrays element by element). */
	FString DiagValue(UObject* Obj, const TCHAR* PropName)
	{
		FProperty* P = Obj ? Obj->GetClass()->FindPropertyByName(PropName) : nullptr;
		if (!P)
		{
			return TEXT("<none>");
		}
		FString Out;
		if (FArrayProperty* A = CastField<FArrayProperty>(P))
		{
			FScriptArrayHelper H(A, A->ContainerPtrToValuePtr<void>(Obj));
			Out = FString::Printf(TEXT("[%d] inner=%s: "), H.Num(), *A->Inner->GetCPPType());
			for (int32 I = 0; I < H.Num() && I < 40; ++I)
			{
				FString E;
				A->Inner->ExportTextItem_Direct(E, H.GetRawPtr(I), nullptr, nullptr, PPF_None);
				Out += E.Right(120) + TEXT(" | ");
			}
			return Out;
		}
		P->ExportTextItem_InContainer(Out, Obj, nullptr, nullptr, PPF_None);
		return Out.Left(400);
	}

	bool GDiagSigsDone = false;

	void DiagBuildMenuState(UUserWidget* Menu)
	{
		UObject* Window = nullptr;
		UObject* Sliding = nullptr;
		if (FObjectProperty* P = CastField<FObjectProperty>(Menu->GetClass()->FindPropertyByName(TEXT("Widget_Window_DarkMode"))))
		{
			Window = P->GetObjectPropertyValue_InContainer(Menu);
		}
		if (FObjectProperty* P = CastField<FObjectProperty>(Menu->GetClass()->FindPropertyByName(TEXT("Widget_SlidingTabs"))))
		{
			Sliding = P->GetObjectPropertyValue_InContainer(Menu);
		}
		if (!GDiagSigsDone)
		{
			GDiagSigsDone = true;
			for (const TCHAR* F : { TEXT("SetupCategoryButtons"), TEXT("SetupSubCategoryButtons"), TEXT("SetSelectedCategory"), TEXT("FocusBuildCategoryByClass"),
				TEXT("OnCategoryClicked"), TEXT("InitBuildMenu"), TEXT("SetBuildMenuMode"), TEXT("UpdateBuildMenuMode"), TEXT("OnMenuStateChanged"),
				TEXT("BndEvt__Widget_Window_DarkMode_K2Node_ComponentBoundEvent_7_OnTabButtonClicked__DelegateSignature"),
				TEXT("BndEvt__Widget_BuildMenu_Widget_SlidingTabs_K2Node_ComponentBoundEvent_17_OnActiveTabChanged__DelegateSignature"),
				TEXT("On Customiser Categories Setup"), TEXT("InitBuildGunState") })
			{
				DiagSignature(Menu->GetClass(), F);
			}
			if (Window)
			{
				for (const TCHAR* F : { TEXT("InitTabs"), TEXT("SetTabVisibility"), TEXT("GetActiveTabIndex"), TEXT("SetTitleForTab"), TEXT("LockTabSwitching") })
				{
					DiagSignature(Window->GetClass(), F);
				}
				UE_LOG(LogRoadMaker, Display, TEXT("[Diag] window mTabs = %s"), *DiagValue(Window, TEXT("mTabs")));
			}
			if (Sliding)
			{
				for (const TCHAR* F : { TEXT("SetActiveIndex"), TEXT("SetupIndexOffset"), TEXT("SetActiveButton"), TEXT("SetInitialTab"), TEXT("HideNonActiveIndexes") })
				{
					DiagSignature(Sliding->GetClass(), F);
				}
			}
			if (UEnum* Modes = FindFirstObject<UEnum>(TEXT("EBuildMenu_Modes"), EFindFirstObjectOptions::None))
			{
				FString S;
				for (int32 I = 0; I < Modes->NumEnums(); ++I)
				{
					S += FString::Printf(TEXT("%d=%s "), (int32)Modes->GetValueByIndex(I), *Modes->GetDisplayNameTextByIndex(I).ToString());
				}
				UE_LOG(LogRoadMaker, Display, TEXT("[Diag] EBuildMenu_Modes: %s"), *S);
			}
		}
		DiagSet(TEXT("menu mBuildMenuMode"), DiagValue(Menu, TEXT("mBuildMenuMode")));
		DiagSet(TEXT("menu mSelectedBuildCategory"), DiagValue(Menu, TEXT("mSelectedBuildCategory")));
		DiagSet(TEXT("menu mAllBuildCategories"), DiagValue(Menu, TEXT("mAllBuildCategories")));
		DiagSet(TEXT("menu mSubCategoryToWidget"), DiagValue(Menu, TEXT("mSubCategoryToWidget")).Left(300));
		if (Sliding)
		{
			DiagSet(TEXT("sliding mActiveIndex"), DiagValue(Sliding, TEXT("mActiveIndex")));
			DiagSet(TEXT("sliding mIndexOffset"), DiagValue(Sliding, TEXT("mIndexOffset")));
			DiagSet(TEXT("sliding mButtons"), DiagValue(Sliding, TEXT("mButtons")));
		}
		if (Window)
		{
			DiagSet(TEXT("window mTabs"), DiagValue(Window, TEXT("mTabs")));
		}
	}

	bool DiagTick(float)
	{
		for (TObjectIterator<UUserWidget> It; It; ++It)
		{
			UUserWidget* W = *It;
			if (!W || W->IsTemplate() || !W->GetCachedWidget().IsValid() || !W->IsVisible())
			{
				continue;
			}
			const FString ClassName = W->GetClass()->GetName();
			if (ClassName == TEXT("Widget_BuildMenu_C"))
			{
				DiagBuildMenuState(W);
			}
			// Every widget class seen on screen, once (to find the build menu's own name).
			if (!GDiagSeenClasses.Contains(ClassName))
			{
				GDiagSeenClasses.Add(ClassName);
				UUserWidget* Outer = W->GetTypedOuter<UUserWidget>();
				UE_LOG(LogRoadMaker, Display, TEXT("[Diag] shown: %s (in %s)"), *ClassName, Outer ? *Outer->GetClass()->GetName() : TEXT("-"));
			}
			// Dump the outermost build menu widget (not one nested in another build menu widget).
			const bool bBuildMenu = ClassName.Contains(TEXT("BuildMenu")) || ClassName.Contains(TEXT("Build_Menu"));
			UUserWidget* Parent = W->GetTypedOuter<UUserWidget>();
			const bool bNested = Parent && (Parent->GetClass()->GetName().Contains(TEXT("BuildMenu")) || Parent->GetClass()->GetName().Contains(TEXT("Build_Menu")));
			if (!bBuildMenu || bNested || GDiagDumped.Contains(ClassName))
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
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&DiagTick), 0.1f);
	UE_LOG(LogRoadMaker, Display, TEXT("[Diag] development logger on (build menu dump)"));
}
#else
void RMDiag::InstallHooks() {}
#endif
