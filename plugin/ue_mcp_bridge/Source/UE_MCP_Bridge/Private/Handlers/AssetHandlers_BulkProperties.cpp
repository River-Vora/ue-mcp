#include "AssetHandlers.h"
#include "HandlerUtils.h"
#include "HandlerJsonProperty.h"
#include "JsonSerializer.h"
#include "EditorScriptingUtilities/Public/EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "UObject/Package.h"

namespace
{
	constexpr int32 MaxBulkPropertyAssets = 500;

	UObject* ResolveBulkPropertyTarget(UObject* Asset)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			if (UClass* GeneratedClass = Blueprint->GeneratedClass)
			{
				if (UObject* CDO = GeneratedClass->GetDefaultObject()) return CDO;
			}
		}
		return Asset;
	}

	struct FPreparedPropertyWrite
	{
		FString PropertyName;
		TSharedPtr<FJsonValue> RequestedValue;
		TSharedPtr<FJsonValue> PreviousValue;
		FString PreviousText;
		FString ProposedText;
	};

	struct FPreparedAssetWrite
	{
		FString AssetPath;
		UObject* Asset = nullptr;
		TArray<FPreparedPropertyWrite> Properties;
	};

	bool IsProtectedBulkAssetPath(const FString& Path)
	{
		FString Normalized = Path;
		Normalized.TrimStartAndEndInline();
		if (!Normalized.StartsWith(TEXT("/"))) Normalized = TEXT("/") + Normalized;
		const FString Lower = Normalized.ToLower();
		return Lower.StartsWith(TEXT("/engine/"))
			|| Lower.StartsWith(TEXT("/script/"))
			|| Lower.StartsWith(TEXT("/memory/"))
			|| Lower.StartsWith(TEXT("/temp/"));
	}

	TSharedPtr<FJsonObject> MakePropertyReadback(
		const FPreparedPropertyWrite& Prepared,
		const TSharedPtr<FJsonValue>& ActualValue,
		const FString& ActualText)
	{
		TSharedPtr<FJsonObject> Readback = MakeShared<FJsonObject>();
		Readback->SetStringField(TEXT("propertyName"), Prepared.PropertyName);
		Readback->SetField(TEXT("previousValue"), Prepared.PreviousValue);
		Readback->SetField(TEXT("value"), ActualValue);
		Readback->SetStringField(TEXT("previousValueText"), Prepared.PreviousText);
		Readback->SetStringField(TEXT("valueText"), ActualText);
		Readback->SetBoolField(TEXT("changed"), Prepared.PreviousText != ActualText);
		return Readback;
	}
}

TSharedPtr<FJsonValue> FAssetHandlers::BulkSetAssetProperties(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Params->TryGetArrayField(TEXT("items"), Items) || !Items)
	{
		return MCPError(TEXT("Missing 'items' array"));
	}
	if (Items->Num() == 0)
	{
		return MCPError(TEXT("'items' must contain at least one asset update"));
	}
	if (Items->Num() > MaxBulkPropertyAssets)
	{
		return MCPError(FString::Printf(
			TEXT("'items' exceeds the maximum batch size of %d (received %d)"),
			MaxBulkPropertyAssets, Items->Num()));
	}

	bool bSave = true;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dryRun"), bDryRun);

	TArray<FPreparedAssetWrite> PreparedAssets;
	PreparedAssets.Reserve(Items->Num());
	TSet<FString> SeenAssetPaths;
	int32 RequestedPropertyCount = 0;

	// Full preflight: validate every descriptor, load every target, resolve every
	// dotted path, and deserialize every proposed value into temporary property
	// storage. No UObject is modified until this entire pass succeeds.
	for (int32 ItemIndex = 0; ItemIndex < Items->Num(); ++ItemIndex)
	{
		const TSharedPtr<FJsonObject>* ItemObject = nullptr;
		if (!(*Items)[ItemIndex].IsValid() || !(*Items)[ItemIndex]->TryGetObject(ItemObject) || !ItemObject || !(*ItemObject).IsValid())
		{
			return MCPError(FString::Printf(TEXT("items[%d] must be an object"), ItemIndex));
		}

		FString AssetPath;
		if (!(*ItemObject)->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
		{
			return MCPError(FString::Printf(TEXT("items[%d].assetPath must be a non-empty string"), ItemIndex));
		}
		if (IsProtectedBulkAssetPath(AssetPath))
		{
			return MCPError(FString::Printf(TEXT("Refusing to mutate protected mount: %s"), *AssetPath));
		}
		if (SeenAssetPaths.Contains(AssetPath))
		{
			return MCPError(FString::Printf(TEXT("Duplicate assetPath in batch: %s"), *AssetPath));
		}
		SeenAssetPaths.Add(AssetPath);

		const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
		if (!(*ItemObject)->TryGetObjectField(TEXT("properties"), PropertiesObject)
			|| !PropertiesObject || !(*PropertiesObject).IsValid() || (*PropertiesObject)->Values.Num() == 0)
		{
			return MCPError(FString::Printf(TEXT("items[%d].properties must be a non-empty object"), ItemIndex));
		}

		UObject* LoadedAsset = LoadObject<UObject>(nullptr, *AssetPath);
		if (!LoadedAsset)
		{
			return MCPError(FString::Printf(TEXT("Preflight failed: could not load asset '%s'"), *AssetPath));
		}
		UObject* Asset = ResolveBulkPropertyTarget(LoadedAsset);

		FPreparedAssetWrite PreparedAsset;
		PreparedAsset.AssetPath = AssetPath;
		PreparedAsset.Asset = Asset;
		PreparedAsset.Properties.Reserve((*PropertiesObject)->Values.Num());

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesObject)->Values)
		{
			if (Pair.Key.IsEmpty() || !Pair.Value.IsValid())
			{
				return MCPError(FString::Printf(TEXT("Preflight failed for '%s': property names and values must be non-empty"), *AssetPath));
			}

			FProperty* Property = nullptr;
			void* ValueAddress = nullptr;
			UObject* LeafOwner = nullptr;
			FString ResolveError;
			if (!MCPJsonProperty::ResolveDottedPath(Asset, Pair.Key, Property, ValueAddress, LeafOwner, ResolveError))
			{
				return MCPError(FString::Printf(TEXT("Preflight failed for '%s.%s': %s"), *AssetPath, *Pair.Key, *ResolveError));
			}

			void* TemporaryValue = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
			Property->InitializeValue(TemporaryValue);
			Property->CopyCompleteValue(TemporaryValue, ValueAddress);
			FString SetError;
			const bool bValidValue = MCPJsonProperty::SetJsonOnProperty(Property, TemporaryValue, Pair.Value, SetError);
			FString ProposedText;
			if (bValidValue)
			{
				Property->ExportText_Direct(ProposedText, TemporaryValue, TemporaryValue, LeafOwner, PPF_None);
			}
			Property->DestroyValue(TemporaryValue);
			FMemory::Free(TemporaryValue);

			if (!bValidValue)
			{
				return MCPError(FString::Printf(TEXT("Preflight failed for '%s.%s': %s"), *AssetPath, *Pair.Key, *SetError));
			}

			FPreparedPropertyWrite PreparedProperty;
			PreparedProperty.PropertyName = Pair.Key;
			PreparedProperty.RequestedValue = Pair.Value;
			PreparedProperty.PreviousValue = FMCPJsonSerializer::SerializeValue(ValueAddress, Property);
			Property->ExportText_Direct(PreparedProperty.PreviousText, ValueAddress, ValueAddress, LeafOwner, PPF_None);
			PreparedProperty.ProposedText = MoveTemp(ProposedText);
			PreparedAsset.Properties.Add(MoveTemp(PreparedProperty));
			++RequestedPropertyCount;
		}

		PreparedAssets.Add(MoveTemp(PreparedAsset));
	}

	TArray<TSharedPtr<FJsonValue>> ItemResults;
	TArray<TSharedPtr<FJsonValue>> RollbackItems;
	ItemResults.Reserve(PreparedAssets.Num());
	RollbackItems.Reserve(PreparedAssets.Num());
	int32 UpdatedAssetCount = 0;
	int32 UpdatedPropertyCount = 0;
	int32 SavedAssetCount = 0;
	int32 SaveFailedCount = 0;

	for (FPreparedAssetWrite& PreparedAsset : PreparedAssets)
	{
		TSharedPtr<FJsonObject> ItemResult = MakeShared<FJsonObject>();
		ItemResult->SetStringField(TEXT("assetPath"), PreparedAsset.AssetPath);
		TArray<TSharedPtr<FJsonValue>> PropertyResults;
		TSharedPtr<FJsonObject> RollbackProperties = MakeShared<FJsonObject>();
		bool bAssetChanged = false;

		for (FPreparedPropertyWrite& PreparedProperty : PreparedAsset.Properties)
		{
			RollbackProperties->SetField(PreparedProperty.PropertyName, PreparedProperty.PreviousValue);

			if (bDryRun)
			{
				PropertyResults.Add(MakeShared<FJsonValueObject>(MakePropertyReadback(
					PreparedProperty, PreparedProperty.RequestedValue, PreparedProperty.ProposedText)));
				continue;
			}

			FProperty* Property = nullptr;
			void* ValueAddress = nullptr;
			UObject* LeafOwner = nullptr;
			FString ResolveError;
			if (!MCPJsonProperty::ResolveDottedPath(PreparedAsset.Asset, PreparedProperty.PropertyName, Property, ValueAddress, LeafOwner, ResolveError))
			{
				return MCPError(FString::Printf(TEXT("Apply failed after preflight for '%s.%s': %s"),
					*PreparedAsset.AssetPath, *PreparedProperty.PropertyName, *ResolveError));
			}

			PreparedAsset.Asset->Modify();
			if (LeafOwner && LeafOwner != PreparedAsset.Asset) LeafOwner->Modify();
			FString SetError;
			if (!MCPJsonProperty::SetJsonOnProperty(Property, ValueAddress, PreparedProperty.RequestedValue, SetError))
			{
				return MCPError(FString::Printf(TEXT("Apply failed after preflight for '%s.%s': %s"),
					*PreparedAsset.AssetPath, *PreparedProperty.PropertyName, *SetError));
			}
			if (LeafOwner) LeafOwner->PostEditChange();

			FString ActualText;
			Property->ExportText_Direct(ActualText, ValueAddress, ValueAddress, LeafOwner, PPF_None);
			const bool bChanged = PreparedProperty.PreviousText != ActualText;
			bAssetChanged |= bChanged;
			if (bChanged) ++UpdatedPropertyCount;
			PropertyResults.Add(MakeShared<FJsonValueObject>(MakePropertyReadback(
				PreparedProperty, FMCPJsonSerializer::SerializeValue(ValueAddress, Property), ActualText)));
		}

		if (!bDryRun && bAssetChanged)
		{
			PreparedAsset.Asset->PostEditChange();
			PreparedAsset.Asset->MarkPackageDirty();
			++UpdatedAssetCount;
		}

		bool bSaved = false;
		if (!bDryRun && bSave && bAssetChanged)
		{
			bSaved = UEditorAssetLibrary::SaveAsset(PreparedAsset.AssetPath, false);
			if (bSaved) ++SavedAssetCount;
			else ++SaveFailedCount;
		}

		ItemResult->SetBoolField(TEXT("changed"), !bDryRun && bAssetChanged);
		ItemResult->SetBoolField(TEXT("wouldChange"), bDryRun && PreparedAsset.Properties.ContainsByPredicate(
			[](const FPreparedPropertyWrite& Property) { return Property.PreviousText != Property.ProposedText; }));
		ItemResult->SetBoolField(TEXT("saved"), bSaved);
		ItemResult->SetArrayField(TEXT("properties"), PropertyResults);
		ItemResults.Add(MakeShared<FJsonValueObject>(ItemResult));

		TSharedPtr<FJsonObject> RollbackItem = MakeShared<FJsonObject>();
		RollbackItem->SetStringField(TEXT("assetPath"), PreparedAsset.AssetPath);
		RollbackItem->SetObjectField(TEXT("properties"), RollbackProperties);
		RollbackItems.Add(MakeShared<FJsonValueObject>(RollbackItem));
	}

	auto Result = MCPSuccess();
	Result->SetBoolField(TEXT("dryRun"), bDryRun);
	Result->SetBoolField(TEXT("preflightPassed"), true);
	Result->SetNumberField(TEXT("requestedAssetCount"), PreparedAssets.Num());
	Result->SetNumberField(TEXT("requestedPropertyCount"), RequestedPropertyCount);
	Result->SetNumberField(TEXT("updatedAssetCount"), UpdatedAssetCount);
	Result->SetNumberField(TEXT("updatedPropertyCount"), UpdatedPropertyCount);
	Result->SetNumberField(TEXT("savedAssetCount"), SavedAssetCount);
	Result->SetNumberField(TEXT("saveFailedCount"), SaveFailedCount);
	Result->SetArrayField(TEXT("items"), ItemResults);
	if (!bDryRun)
	{
		if (UpdatedAssetCount > 0) MCPSetUpdated(Result);
		TSharedPtr<FJsonObject> RollbackPayload = MakeShared<FJsonObject>();
		RollbackPayload->SetArrayField(TEXT("items"), RollbackItems);
		RollbackPayload->SetBoolField(TEXT("save"), bSave);
		RollbackPayload->SetBoolField(TEXT("dryRun"), false);
		MCPSetRollback(Result, TEXT("bulk_set_asset_properties"), RollbackPayload);
	}
	return MCPResult(Result);
}
