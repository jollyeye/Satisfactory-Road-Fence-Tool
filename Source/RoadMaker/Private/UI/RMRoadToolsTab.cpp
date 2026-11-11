#include "UI/RMRoadToolsTab.h"
#include "RoadMakerModule.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Containers/Ticker.h"
#include "Engine/Texture2D.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	URMRoadToolsTab* GRoadToolsTab = nullptr;

	const TCHAR* RoadToolsCategoryPath = TEXT("/RoadMaker/Buildables/BC_RoadTools.BC_RoadTools_C");

	UObject* GetObjectProp(UObject* Obj, const TCHAR* Name)
	{
		FObjectPropertyBase* P = Obj ? CastField<FObjectPropertyBase>(Obj->GetClass()->FindPropertyByName(Name)) : nullptr;
		return P ? P->GetObjectPropertyValue_InContainer(Obj) : nullptr;
	}

	/** One argument for CallBP, matched to the function's parameters in order. */
	struct FRMArg
	{
		enum EKind { Bool, Int, Object, Text } Kind;
		bool B = false;
		int64 I = 0;
		UObject* O = nullptr;
		FText T;
		static FRMArg MakeBool(bool V) { FRMArg A; A.Kind = Bool; A.B = V; return A; }
		static FRMArg MakeInt(int64 V) { FRMArg A; A.Kind = Int; A.I = V; return A; }
		static FRMArg MakeObject(UObject* V) { FRMArg A; A.Kind = Object; A.O = V; return A; }
		static FRMArg MakeText(const FText& V) { FRMArg A; A.Kind = Text; A.T = V; return A; }
	};

	/** Calls a Blueprint function by name (input parameters in order; outputs left at default). */
	bool CallBP(UObject* Obj, const TCHAR* Name, const TArray<FRMArg>& Args = {})
	{
		UFunction* Fn = Obj ? Obj->FindFunction(FName(Name)) : nullptr;
		if (!Fn)
		{
			UE_LOG(LogRoadMaker, Warning, TEXT("Road Tools tab: %s has no function %s"), *GetNameSafe(Obj), Name);
			return false;
		}
		TArray<uint8> Buffer;
		Buffer.SetNumZeroed(FMath::Max<int32>(Fn->ParmsSize, 1));
		uint8* Parms = Buffer.GetData();
		int32 ArgIndex = 0;
		for (TFieldIterator<FProperty> It(Fn); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FProperty* P = *It;
			P->InitializeValue_InContainer(Parms);
			if (P->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm) || !Args.IsValidIndex(ArgIndex))
			{
				continue;
			}
			const FRMArg& A = Args[ArgIndex++];
			void* V = P->ContainerPtrToValuePtr<void>(Parms);
			if (FBoolProperty* BP = CastField<FBoolProperty>(P))
			{
				BP->SetPropertyValue(V, A.B);
			}
			else if (FNumericProperty* NP = CastField<FNumericProperty>(P))
			{
				NP->SetIntPropertyValue(V, A.I);
			}
			else if (FEnumProperty* EP = CastField<FEnumProperty>(P))
			{
				EP->GetUnderlyingProperty()->SetIntPropertyValue(V, A.I);
			}
			else if (FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(P))
			{
				OP->SetObjectPropertyValue(V, A.O);
			}
			else if (FTextProperty* TP = CastField<FTextProperty>(P))
			{
				TP->SetPropertyValue(V, A.T);
			}
		}
		Obj->ProcessEvent(Fn, Parms);
		for (TFieldIterator<FProperty> It(Fn); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Parms);
		}
		return true;
	}

	FArrayProperty* CategoriesProp(UUserWidget* Menu)
	{
		return CastField<FArrayProperty>(Menu->GetClass()->FindPropertyByName(TEXT("mAllBuildCategories")));
	}

	void ReadCategories(UUserWidget* Menu, TArray<UClass*>& Out)
	{
		Out.Reset();
		FArrayProperty* A = CategoriesProp(Menu);
		FObjectPropertyBase* Inner = A ? CastField<FObjectPropertyBase>(A->Inner) : nullptr;
		if (!Inner)
		{
			return;
		}
		FScriptArrayHelper H(A, A->ContainerPtrToValuePtr<void>(Menu));
		for (int32 I = 0; I < H.Num(); ++I)
		{
			Out.Add(Cast<UClass>(Inner->GetObjectPropertyValue(H.GetRawPtr(I))));
		}
	}

	void WriteCategories(UUserWidget* Menu, const TArray<UClass*>& In)
	{
		FArrayProperty* A = CategoriesProp(Menu);
		FObjectPropertyBase* Inner = A ? CastField<FObjectPropertyBase>(A->Inner) : nullptr;
		if (!Inner)
		{
			return;
		}
		FScriptArrayHelper H(A, A->ContainerPtrToValuePtr<void>(Menu));
		H.Resize(In.Num());
		for (int32 I = 0; I < In.Num(); ++I)
		{
			Inner->SetObjectPropertyValue(H.GetRawPtr(I), In[I]);
		}
	}

	int64 GetMenuMode(UUserWidget* Menu)
	{
		FProperty* P = Menu->GetClass()->FindPropertyByName(TEXT("mBuildMenuMode"));
		if (FByteProperty* B = CastField<FByteProperty>(P))
		{
			return B->GetPropertyValue_InContainer(Menu);
		}
		if (FEnumProperty* E = CastField<FEnumProperty>(P))
		{
			return E->GetUnderlyingProperty()->GetSignedIntPropertyValue(E->ContainerPtrToValuePtr<void>(Menu));
		}
		return 0;
	}

	bool IsShown(UUserWidget* W)
	{
		return W && W->GetCachedWidget().IsValid() && W->IsVisible();
	}

	UClass* RoadToolsCategory()
	{
		static TWeakObjectPtr<UClass> Cached;
		if (!Cached.IsValid())
		{
			Cached = LoadClass<UObject>(nullptr, RoadToolsCategoryPath);
		}
		return Cached.Get();
	}
}

void URMRoadToolsTab::Install()
{
#if !WITH_EDITOR
	if (GRoadToolsTab)
	{
		return;
	}
	GRoadToolsTab = NewObject<URMRoadToolsTab>();
	GRoadToolsTab->AddToRoot();
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(GRoadToolsTab, &URMRoadToolsTab::Tick), 0.f);
#endif
}

void URMRoadToolsTab::FindMenu()
{
	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		if (!It->IsTemplate() && It->GetClass()->GetName() == TEXT("Widget_BuildMenu_C") && IsShown(*It))
		{
			mMenu = *It;
			return;
		}
	}
}

bool URMRoadToolsTab::Tick(float DeltaTime)
{
	UUserWidget* Menu = mMenu.Get();
	if (!IsShown(Menu))
	{
		mFindTimer -= DeltaTime;
		if (mFindTimer <= 0.f)
		{
			mFindTimer = 0.25f;
			FindMenu();
			Menu = mMenu.Get();
		}
	}
	const bool bShown = IsShown(Menu);
	if (bShown && !mWasShown)
	{
		OnMenuShown(Menu);
	}
	mWasShown = bShown;
	if (!bShown || !RoadToolsCategory())
	{
		return true;
	}
	EnsureOurButton(Menu);
	// Another mode (e.g. switched with the tab keys), or a game tab button made active again (its
	// click), ends Road Tools.
	if (mRoadMode && (GetMenuMode(Menu) != 0 || AnyGameTabActive()))
	{
		LeaveRoadMode(true);
	}
	UpdateCategoryButtons(Menu);
	return true;
}

void URMRoadToolsTab::OnMenuShown(UUserWidget* Menu)
{
	mRoadMode = false;
	mFullCategories.Reset();
	// The window's tab clicks: leaving Road Tools.
	UObject* Window = GetObjectProp(Menu, TEXT("Widget_Window_DarkMode"));
	if (Window && mBoundWindow.Get() != Window)
	{
		if (FMulticastDelegateProperty* P = CastField<FMulticastDelegateProperty>(Window->GetClass()->FindPropertyByName(TEXT("OnTabButtonClicked"))))
		{
			FScriptDelegate D;
			D.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(URMRoadToolsTab, OnGameTabClicked));
			P->AddDelegate(D, Window);
			mBoundWindow = Window;
		}
	}
	if (UUserWidget* Ours = mOurButton.Get())
	{
		CallBP(Ours, TEXT("SetActive"), { FRMArg::MakeBool(false) });
	}
}

void URMRoadToolsTab::EnsureOurButton(UUserWidget* Menu)
{
	UObject* Window = GetObjectProp(Menu, TEXT("Widget_Window_DarkMode"));
	UObject* TabsContainer = GetObjectProp(Window, TEXT("Widget_TabsContainer"));
	UCanvasPanel* Container = Cast<UCanvasPanel>(GetObjectProp(TabsContainer, TEXT("mContainer")));
	if (!Container)
	{
		return;
	}
	UUserWidget* Ours = mOurButton.Get();
	if (Ours && Ours->GetParent() == Container)
	{
		return;
	}
	// The game's tab buttons (Build, Customizer, Blueprints): ours goes after the last one, same size.
	TArray<UUserWidget*> GameButtons;
	for (int32 I = 0; I < Container->GetChildrenCount(); ++I)
	{
		UUserWidget* B = Cast<UUserWidget>(Container->GetChildAt(I));
		if (B && B != Ours && B->GetClass()->GetName().Contains(TEXT("SlidingTabs_Button")))
		{
			GameButtons.Add(B);
		}
	}
	if (GameButtons.Num() == 0)
	{
		return;
	}
	UUserWidget* Last = GameButtons.Last();
	UCanvasPanelSlot* LastSlot = Cast<UCanvasPanelSlot>(Last->Slot);
	if (!LastSlot)
	{
		return;
	}
	Ours = CreateWidget<UUserWidget>(Menu->GetOwningPlayer(), Last->GetClass());
	if (!Ours)
	{
		return;
	}
	// Same look as the game's buttons; clicks are ours (no page of its own to slide to).
	for (const TCHAR* Name : { TEXT("mButtonStyle"), TEXT("TextNormalColor"), TEXT("IsBackgroundVisible") })
	{
		if (FProperty* P = Last->GetClass()->FindPropertyByName(Name))
		{
			P->CopyCompleteValue_InContainer(Ours, Last);
		}
	}
	if (FBoolProperty* P = CastField<FBoolProperty>(Ours->GetClass()->FindPropertyByName(TEXT("mOverrideButtonFunction"))))
	{
		P->SetPropertyValue_InContainer(Ours, true);
	}
	UCanvasPanelSlot* Slot = Container->AddChildToCanvas(Ours);
	FAnchorData Layout = LastSlot->GetLayout();
	float Step = LastSlot->GetSize().X;
	if (GameButtons.Num() >= 2)
	{
		if (UCanvasPanelSlot* Prev = Cast<UCanvasPanelSlot>(GameButtons[GameButtons.Num() - 2]->Slot))
		{
			Step = LastSlot->GetPosition().X - Prev->GetPosition().X;
		}
	}
	Layout.Offsets.Left += Step;
	Slot->SetLayout(Layout);
	Slot->SetZOrder(LastSlot->GetZOrder());

	CallBP(Ours, TEXT("SetTitle"), { FRMArg::MakeText(NSLOCTEXT("RoadMaker", "RoadToolsTab", "Road Tools")) });
	if (UTexture2D* Icon = LoadObject<UTexture2D>(nullptr, TEXT("/RoadMaker/Icons/T_Icon_RoadTools.T_Icon_RoadTools")))
	{
		CallBP(Ours, TEXT("SetIcon"), { FRMArg::MakeObject(Icon) });
	}
	CallBP(Ours, TEXT("SetActive"), { FRMArg::MakeBool(mRoadMode) });
	if (UButton* Button = Cast<UButton>(GetObjectProp(Ours, TEXT("mButton"))))
	{
		Button->OnClicked.AddUniqueDynamic(this, &URMRoadToolsTab::OnOurTabClicked);
	}
	mOurButton = Ours;
	UE_LOG(LogRoadMaker, Display, TEXT("Road Tools tab: button added after %s"), *Last->GetName());
}

bool URMRoadToolsTab::AnyGameTabActive() const
{
	UUserWidget* Ours = mOurButton.Get();
	UPanelWidget* Container = Ours ? Ours->GetParent() : nullptr;
	if (!Container)
	{
		return false;
	}
	for (int32 I = 0; I < Container->GetChildrenCount(); ++I)
	{
		UUserWidget* B = Cast<UUserWidget>(Container->GetChildAt(I));
		if (B && B != Ours && B->GetClass() == Ours->GetClass())
		{
			if (FBoolProperty* P = CastField<FBoolProperty>(B->GetClass()->FindPropertyByName(TEXT("IsActive"))))
			{
				if (P->GetPropertyValue_InContainer(B))
				{
					return true;
				}
			}
		}
	}
	return false;
}

void URMRoadToolsTab::SetButtonsActive(bool bOurs)
{
	UUserWidget* Ours = mOurButton.Get();
	UPanelWidget* Container = Ours ? Ours->GetParent() : nullptr;
	if (!Container)
	{
		return;
	}
	if (bOurs)
	{
		for (int32 I = 0; I < Container->GetChildrenCount(); ++I)
		{
			UUserWidget* B = Cast<UUserWidget>(Container->GetChildAt(I));
			if (B && B != Ours && B->GetClass() == Ours->GetClass())
			{
				CallBP(B, TEXT("SetActive"), { FRMArg::MakeBool(false) });
			}
		}
	}
	CallBP(Ours, TEXT("SetActive"), { FRMArg::MakeBool(bOurs) });
}

void URMRoadToolsTab::ApplyCategoryFilter(UUserWidget* Menu, bool bRoadToolsOnly)
{
	UClass* RoadTools = RoadToolsCategory();
	TArray<UClass*> Current;
	ReadCategories(Menu, Current);
	// Remember the game's full list (refreshed whenever it is not our filtered one).
	if (!(Current.Num() == 1 && Current[0] == RoadTools))
	{
		mFullCategories.Reset();
		for (UClass* C : Current)
		{
			mFullCategories.Add(C);
		}
	}
	TArray<UClass*> Wanted;
	if (bRoadToolsOnly)
	{
		Wanted.Add(RoadTools);
	}
	else
	{
		for (const TWeakObjectPtr<UClass>& C : mFullCategories)
		{
			if (C.Get() && C.Get() != RoadTools)
			{
				Wanted.Add(C.Get());
			}
		}
	}
	if (Wanted.Num() == 0)
	{
		return;
	}
	WriteCategories(Menu, Wanted);
	CallBP(Menu, TEXT("SetupCategoryButtons"));
	// Show the first category if the selected one is not in the list any more (or for Road Tools).
	UClass* Selected = Cast<UClass>(GetObjectProp(Menu, TEXT("mSelectedBuildCategory")));
	if (bRoadToolsOnly || !Wanted.Contains(Selected))
	{
		CallBP(Menu, TEXT("OnCategoryClicked"), { FRMArg::MakeInt(0) });
	}
}

void URMRoadToolsTab::UpdateCategoryButtons(UUserWidget* Menu)
{
	// Build page category buttons: Widget_CategoryButton_C in mCategoryButtonsContainer.Container,
	// each with mCategory (the category object; its class is the category, e.g. BC_RoadTools_C).
	UObject* Browser = GetObjectProp(Menu, TEXT("mBrowser_Builder"));
	UObject* ButtonsContainer = GetObjectProp(Browser, TEXT("mCategoryButtonsContainer"));
	UPanelWidget* List = Cast<UPanelWidget>(GetObjectProp(ButtonsContainer, TEXT("Container")));
	UClass* RoadTools = RoadToolsCategory();
	if (!List || !RoadTools)
	{
		return;
	}
	bool bSelectedHidden = false;
	for (int32 I = 0; I < List->GetChildrenCount(); ++I)
	{
		UWidget* Button = List->GetChildAt(I);
		UObject* Category = Button ? GetObjectProp(Button, TEXT("mCategory")) : nullptr;
		if (!Category)
		{
			continue;
		}
		const bool bOurs = Category->IsA(RoadTools) || Category->GetClass() == RoadTools || Category == RoadTools->GetDefaultObject();
		// Road Tools tab: only our categories; Build tab: everything but ours.
		const bool bHide = mRoadMode ? !bOurs : bOurs;
		const TWeakObjectPtr<UWidget> Key(Button);
		if (bHide)
		{
			if (Button->GetVisibility() != ESlateVisibility::Collapsed)
			{
				Button->SetVisibility(ESlateVisibility::Collapsed);
				mHiddenCategoryButtons.Add(Key);
			}
			if (FBoolProperty* P = CastField<FBoolProperty>(Button->GetClass()->FindPropertyByName(TEXT("mIsSelected"))))
			{
				bSelectedHidden |= P->GetPropertyValue_InContainer(Button);
			}
		}
		else if (mHiddenCategoryButtons.Contains(Key))
		{
			Button->SetVisibility(ESlateVisibility::Visible);
			mHiddenCategoryButtons.Remove(Key);
		}
	}
	// The open category was just hidden: open the first one shown instead.
	if (bSelectedHidden)
	{
		for (int32 I = 0; I < List->GetChildrenCount(); ++I)
		{
			UWidget* Button = List->GetChildAt(I);
			if (Button && Button->GetVisibility() != ESlateVisibility::Collapsed && GetObjectProp(Button, TEXT("mCategory")))
			{
				CallBP(Menu, TEXT("OnCategoryClicked"), { FRMArg::MakeInt(I) });
				break;
			}
		}
	}
}

void URMRoadToolsTab::OnOurTabClicked()
{
	UE_LOG(LogRoadMaker, Display, TEXT("Road Tools tab: clicked"));
	EnterRoadMode();
}

void URMRoadToolsTab::EnterRoadMode()
{
	UUserWidget* Menu = mMenu.Get();
	if (!IsShown(Menu))
	{
		return;
	}
	// Build page and the build gun's build mode, like the Build tab.
	if (GetMenuMode(Menu) != 0)
	{
		CallBP(Menu, TEXT("UpdateBuildMenuMode"), { FRMArg::MakeInt(0), FRMArg::MakeBool(true), FRMArg::MakeBool(true), FRMArg::MakeBool(true) });
	}
	mRoadMode = true;
	// For now: open the Road Tools category (hiding the other categories needs the category buttons,
	// being mapped with the logger).
	TArray<UClass*> Current;
	ReadCategories(Menu, Current);
	const int32 Index = Current.IndexOfByKey(RoadToolsCategory());
	UE_LOG(LogRoadMaker, Display, TEXT("Road Tools tab: Road Tools is category %d of %d"), Index, Current.Num());
	if (Index != INDEX_NONE)
	{
		CallBP(Menu, TEXT("OnCategoryClicked"), { FRMArg::MakeInt(Index) });
	}
	SetButtonsActive(true);
}

void URMRoadToolsTab::OnGameTabClicked(int32 ButtonIndex)
{
	LeaveRoadMode(ButtonIndex == 0);
}

void URMRoadToolsTab::LeaveRoadMode(bool bRefreshBuildPage)
{
	if (!mRoadMode)
	{
		return;
	}
	mRoadMode = false;
	if (UUserWidget* Ours = mOurButton.Get())
	{
		CallBP(Ours, TEXT("SetActive"), { FRMArg::MakeBool(false) });
	}
	(void)bRefreshBuildPage;
}
