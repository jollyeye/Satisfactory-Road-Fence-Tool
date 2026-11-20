#include "Map/RMEditorTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"

bool URMEditorTools::ConnectCustomizedUV(UMaterial* Material, UMaterialExpression* Expression, int32 Index)
{
#if WITH_EDITOR
	if (!Material || !Expression || Index < 0 || Index > 7)
	{
		return false;
	}
	FExpressionInput* Input = Material->GetExpressionInputForProperty(EMaterialProperty(MP_CustomizedUVs0 + Index));
	if (!Input)
	{
		return false;
	}
	Input->Connect(0, Expression);
	return true;
#else
	return false;
#endif
}

FText URMEditorTools::TableText(UStringTable* Table, const FString& Key, const FString& Source)
{
#if WITH_EDITOR
	if (!Table || Key.IsEmpty())
	{
		return FText::FromString(Source);
	}
	FStringTableRef Entries = Table->GetMutableStringTable();
	bool bChanged = false;
	if (Entries->GetNamespace() != TEXT("RoadMakerAssets"))
	{
		Entries->SetNamespace(TEXT("RoadMakerAssets"));
		bChanged = true;
	}
	FString Existing;
	if (!Entries->GetSourceString(FTextKey(Key), Existing) || !Existing.Equals(Source, ESearchCase::CaseSensitive))
	{
		Entries->SetSourceString(FTextKey(Key), Source);
		bChanged = true;
	}
	if (bChanged)
	{
		Table->MarkPackageDirty();
	}
	return FText::FromStringTable(Table->GetStringTableId(), FTextKey(Key));
#else
	return FText::FromString(Source);
#endif
}
