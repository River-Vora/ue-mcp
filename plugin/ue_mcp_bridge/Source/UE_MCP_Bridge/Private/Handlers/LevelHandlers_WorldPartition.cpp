// World Partition actor descriptors (#746).
//
// In a World Partition map an actor whose cell is not loaded is not spawned in
// the editor world at all, so every actor-facing action that iterates spawned
// instances - get_actors_by_class, count_actors_by_class, get_outliner,
// get_actor_details, set_actor_property - silently returned zero for it. A
// caller could not tell "this actor does not exist" from "this actor exists but
// is not streamed in", which quietly corrupts any measurement built on those
// counts. These handlers read the on-disk descriptors instead, so unloaded
// actors are visible and can be streamed in on demand.
//
// This file is a translation-unit partition of FLevelHandlers, like
// LevelHandlers_MultiLevel.cpp - the registrations live in LevelHandlers.cpp.

#include "LevelHandlers.h"

#include "HandlerUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

#if WITH_EDITOR
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionBlueprintLibrary.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#endif

namespace
{
#if WITH_EDITOR
	constexpr int32 DefaultActorDescLimit = 500;

	/** GUIDs of actors currently spawned in the editor world. */
	TSet<FGuid> CollectLoadedActorGuids(UWorld* World)
	{
		TSet<FGuid> Loaded;
		if (!World) return Loaded;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (AActor* Actor = *It)
			{
				const FGuid Guid = Actor->GetActorGuid();
				if (Guid.IsValid()) Loaded.Add(Guid);
			}
		}
		return Loaded;
	}

	FString ActorDescClassName(const FActorDesc& Desc)
	{
		if (Desc.NativeClass) return Desc.NativeClass->GetName();
		return Desc.Class.IsValid() ? Desc.Class.GetAssetName() : FString();
	}

	/** Case-insensitive match of a needle against every identifying field. */
	bool ActorDescMatchesFilter(const FActorDesc& Desc, const FString& LowerFilter)
	{
		if (LowerFilter.IsEmpty()) return true;
		const FString Blob = (Desc.Label.ToString() + TEXT("|") + Desc.Name.ToString() + TEXT("|") +
			ActorDescClassName(Desc) + TEXT("|") + Desc.Class.ToString() + TEXT("|") +
			Desc.ActorPath.ToString()).ToLower();
		return Blob.Contains(LowerFilter);
	}

	TSharedPtr<FJsonObject> ActorDescToJson(const FActorDesc& Desc, bool bLoaded)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("guid"), Desc.Guid.ToString());
		Obj->SetStringField(TEXT("label"), Desc.Label.ToString());
		Obj->SetStringField(TEXT("name"), Desc.Name.ToString());
		Obj->SetStringField(TEXT("class"), ActorDescClassName(Desc));
		Obj->SetStringField(TEXT("classPath"), Desc.Class.ToString());
		Obj->SetStringField(TEXT("actorPath"), Desc.ActorPath.ToString());
		Obj->SetStringField(TEXT("package"), Desc.ActorPackage.ToString());
		Obj->SetStringField(TEXT("runtimeGrid"), Desc.RuntimeGrid.ToString());
		Obj->SetBoolField(TEXT("spatiallyLoaded"), Desc.bIsSpatiallyLoaded);
		Obj->SetBoolField(TEXT("editorOnly"), Desc.bActorIsEditorOnly);
		Obj->SetBoolField(TEXT("loaded"), bLoaded);

		if (Desc.Bounds.IsValid)
		{
			const FVector Min = Desc.Bounds.Min;
			const FVector Max = Desc.Bounds.Max;
			TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
			TSharedPtr<FJsonObject> MinObj = MakeShared<FJsonObject>();
			MinObj->SetNumberField(TEXT("x"), Min.X);
			MinObj->SetNumberField(TEXT("y"), Min.Y);
			MinObj->SetNumberField(TEXT("z"), Min.Z);
			TSharedPtr<FJsonObject> MaxObj = MakeShared<FJsonObject>();
			MaxObj->SetNumberField(TEXT("x"), Max.X);
			MaxObj->SetNumberField(TEXT("y"), Max.Y);
			MaxObj->SetNumberField(TEXT("z"), Max.Z);
			BoundsObj->SetObjectField(TEXT("min"), MinObj);
			BoundsObj->SetObjectField(TEXT("max"), MaxObj);
			Obj->SetObjectField(TEXT("bounds"), BoundsObj);
		}

		TArray<TSharedPtr<FJsonValue>> DataLayers;
		for (const FSoftObjectPath& Layer : Desc.DataLayerAssets)
		{
			DataLayers.Add(MakeShared<FJsonValueString>(Layer.ToString()));
		}
		Obj->SetArrayField(TEXT("dataLayers"), DataLayers);
		return Obj;
	}

	bool TryReadBounds(const TSharedPtr<FJsonObject>& Params, FBox& OutBox)
	{
		const TSharedPtr<FJsonObject>* BoundsObj = nullptr;
		if (!Params->TryGetObjectField(TEXT("bounds"), BoundsObj) || !BoundsObj) return false;
		const TSharedPtr<FJsonObject>* MinObj = nullptr;
		const TSharedPtr<FJsonObject>* MaxObj = nullptr;
		if (!(*BoundsObj)->TryGetObjectField(TEXT("min"), MinObj) || !MinObj) return false;
		if (!(*BoundsObj)->TryGetObjectField(TEXT("max"), MaxObj) || !MaxObj) return false;

		auto ReadVec = [](const TSharedPtr<FJsonObject>& Obj) -> FVector
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			Obj->TryGetNumberField(TEXT("x"), X);
			Obj->TryGetNumberField(TEXT("y"), Y);
			Obj->TryGetNumberField(TEXT("z"), Z);
			return FVector(X, Y, Z);
		};
		const FVector Min = ReadVec(*MinObj);
		const FVector Max = ReadVec(*MaxObj);
		// An inverted box silently matches nothing, which reads as "no actors".
		OutBox = FBox(FVector::Min(Min, Max), FVector::Max(Min, Max));
		return true;
	}

	/** Descriptors matching the caller's filters, honouring an explicit guid list. */
	bool GatherMatchingDescs(
		const TSharedPtr<FJsonObject>& Params,
		UWorld* World,
		TArray<FActorDesc>& OutMatches,
		FString& OutError)
	{
		UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;
		if (!WorldPartition)
		{
			OutError = TEXT("The current editor world has no World Partition");
			return false;
		}

		// UWorldPartitionBlueprintLibrary is MinimalAPI, so its statics cannot be
		// linked from another module. FWorldPartitionHelpers is exported and
		// gives the same traversal.
		TArray<FActorDesc> AllDescs;
		FBox Box(ForceInit);
		const bool bHasBounds = TryReadBounds(Params, Box);
		auto Collect = [&AllDescs](const FWorldPartitionActorDescInstance* Instance) -> bool
		{
			if (Instance) AllDescs.Emplace(*Instance);
			return true;
		};
		if (bHasBounds)
		{
			FWorldPartitionHelpers::ForEachIntersectingActorDescInstance(
				WorldPartition, Box, AActor::StaticClass(), Collect);
		}
		else
		{
			FWorldPartitionHelpers::ForEachActorDescInstance(
				WorldPartition, AActor::StaticClass(), Collect);
		}

		TSet<FString> WantedGuids;
		const TArray<TSharedPtr<FJsonValue>>* GuidValues = nullptr;
		if (Params->TryGetArrayField(TEXT("guids"), GuidValues) && GuidValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *GuidValues)
			{
				FString Guid;
				if (Value.IsValid() && Value->TryGetString(Guid) && !Guid.IsEmpty())
				{
					WantedGuids.Add(Guid.ToLower());
				}
			}
		}

		const FString LowerFilter = OptionalString(Params, TEXT("filter")).ToLower();
		const FString ClassName = OptionalString(Params, TEXT("className")).ToLower();
		const bool bLoadedOnly = OptionalBool(Params, TEXT("loadedOnly"), false);
		const bool bUnloadedOnly = OptionalBool(Params, TEXT("unloadedOnly"), false);
		const TSet<FGuid> LoadedGuids = CollectLoadedActorGuids(World);

		for (const FActorDesc& Desc : AllDescs)
		{
			if (WantedGuids.Num() > 0 && !WantedGuids.Contains(Desc.Guid.ToString().ToLower())) continue;
			if (!ActorDescMatchesFilter(Desc, LowerFilter)) continue;
			if (!ClassName.IsEmpty() && !ActorDescClassName(Desc).ToLower().Contains(ClassName)) continue;

			const bool bLoaded = LoadedGuids.Contains(Desc.Guid);
			if (bLoadedOnly && !bLoaded) continue;
			if (bUnloadedOnly && bLoaded) continue;

			OutMatches.Add(Desc);
		}
		return true;
	}

	bool RequirePartitionedWorld(UWorld* World, TSharedPtr<FJsonValue>& OutError)
	{
		if (!World || !World->IsPartitionedWorld())
		{
			OutError = MCPError(TEXT("The current editor world is not a World Partition map, so it has no actor descriptors. Every actor is already spawned - use get_outliner or get_actors_by_class."));
			return false;
		}
		return true;
	}
#endif // WITH_EDITOR
}

TSharedPtr<FJsonValue> FLevelHandlers::ListActorDescs(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	REQUIRE_EDITOR_WORLD(World);
	TSharedPtr<FJsonValue> Err;
	if (!RequirePartitionedWorld(World, Err)) return Err;

	TArray<FActorDesc> Matches;
	FString Error;
	if (!GatherMatchingDescs(Params, World, Matches, Error))
	{
		return MCPError(Error);
	}

	const int32 Limit = FMath::Max(1, OptionalInt(Params, TEXT("limit"), DefaultActorDescLimit));
	const TSet<FGuid> LoadedGuids = CollectLoadedActorGuids(World);

	int32 LoadedCount = 0;
	for (const FActorDesc& Desc : Matches)
	{
		if (LoadedGuids.Contains(Desc.Guid)) ++LoadedCount;
	}

	TArray<TSharedPtr<FJsonValue>> Entries;
	const int32 Emitted = FMath::Min(Limit, Matches.Num());
	for (int32 i = 0; i < Emitted; ++i)
	{
		Entries.Add(MakeShared<FJsonValueObject>(ActorDescToJson(Matches[i], LoadedGuids.Contains(Matches[i].Guid))));
	}

	auto Result = MCPSuccess();
	Result->SetNumberField(TEXT("matched"), Matches.Num());
	Result->SetNumberField(TEXT("returned"), Entries.Num());
	Result->SetNumberField(TEXT("loadedMatches"), LoadedCount);
	Result->SetNumberField(TEXT("unloadedMatches"), Matches.Num() - LoadedCount);
	Result->SetBoolField(TEXT("truncated"), Matches.Num() > Entries.Num());
	Result->SetArrayField(TEXT("actorDescs"), Entries);
	if (Matches.Num() > Entries.Num())
	{
		Result->SetStringField(TEXT("note"), FString::Printf(
			TEXT("%d of %d descriptors returned; raise 'limit' or narrow 'filter' to see the rest."),
			Entries.Num(), Matches.Num()));
	}
	return MCPResult(Result);
#else
	return MCPError(TEXT("World Partition actor descriptors are editor-only"));
#endif
}

TSharedPtr<FJsonValue> FLevelHandlers::LoadActorDescs(const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	REQUIRE_EDITOR_WORLD(World);
	TSharedPtr<FJsonValue> Err;
	if (!RequirePartitionedWorld(World, Err)) return Err;

	// Pinning is the exported, durable way to make an unloaded actor resident:
	// it keeps the actor loaded regardless of the current streaming sources,
	// which is exactly what a measurement or edit pass needs. (The engine's
	// transient "load" path lives on a MinimalAPI Blueprint library that cannot
	// be linked from here, and would be dropped again by streaming anyway.)
	const FString Mode = OptionalString(Params, TEXT("mode"), TEXT("pin")).ToLower();
	if (Mode != TEXT("pin") && Mode != TEXT("unpin"))
	{
		return MCPError(TEXT("'mode' must be 'pin' (make resident) or 'unpin' (release)"));
	}

	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition)
	{
		return MCPError(TEXT("The current editor world has no World Partition"));
	}

	TArray<FActorDesc> Matches;
	FString Error;
	if (!GatherMatchingDescs(Params, World, Matches, Error))
	{
		return MCPError(Error);
	}

	if (Matches.Num() == 0)
	{
		auto Empty = MCPSuccess();
		Empty->SetStringField(TEXT("mode"), Mode);
		Empty->SetNumberField(TEXT("matched"), 0);
		Empty->SetNumberField(TEXT("affected"), 0);
		Empty->SetStringField(TEXT("note"), TEXT("No actor descriptor matched. Use list_actor_descs to see what the map contains."));
		return MCPResult(Empty);
	}

	// Refuse to stream the whole map in by accident: an unfiltered call on a
	// large World Partition level would load thousands of actors.
	const int32 MaxAffected = FMath::Max(1, OptionalInt(Params, TEXT("maxActors"), 256));
	if (Matches.Num() > MaxAffected)
	{
		return MCPError(FString::Printf(
			TEXT("%d descriptors matched, above the %d limit for a single %s. Narrow 'filter'/'className'/'bounds' or raise 'maxActors' deliberately."),
			Matches.Num(), MaxAffected, *Mode));
	}

	const bool bDryRun = OptionalBool(Params, TEXT("dryRun"), false);
	const TSet<FGuid> ResidentBefore = CollectLoadedActorGuids(World);
	TArray<FGuid> Guids;
	TArray<TSharedPtr<FJsonValue>> Affected;
	Guids.Reserve(Matches.Num());
	for (const FActorDesc& Desc : Matches)
	{
		Guids.Add(Desc.Guid);
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("guid"), Desc.Guid.ToString());
		Entry->SetStringField(TEXT("label"), Desc.Label.ToString());
		Entry->SetStringField(TEXT("class"), ActorDescClassName(Desc));
		Affected.Add(MakeShared<FJsonValueObject>(Entry));
	}

	if (!bDryRun)
	{
		if (Mode == TEXT("pin")) WorldPartition->PinActors(Guids);
		else                     WorldPartition->UnpinActors(Guids);
	}

	// Read the world back so the caller sees what actually became resident
	// rather than trusting the request.
	const TSet<FGuid> NowLoaded = CollectLoadedActorGuids(World);
	int32 ResidentAfter = 0;
	for (const FGuid& Guid : Guids)
	{
		if (NowLoaded.Contains(Guid)) ++ResidentAfter;
	}
	// A real delta: how many actually changed residency. Without the
	// before-snapshot, an already-loaded actor counted as "affected" by a call
	// that did nothing.
	int32 Changed = 0;
	for (const FGuid& Guid : Guids)
	{
		const bool bWas = ResidentBefore.Contains(Guid);
		const bool bNow = NowLoaded.Contains(Guid);
		if (bWas != bNow) ++Changed;
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetBoolField(TEXT("dryRun"), bDryRun);
	Result->SetNumberField(TEXT("matched"), Matches.Num());
	// Report what actually changed, not what was requested: PinActors is a
	// no-op when the world partition has no pinned-actor adapter.
	Result->SetNumberField(TEXT("requested"), Guids.Num());
	Result->SetNumberField(TEXT("affected"), bDryRun ? 0 : Changed);
	Result->SetNumberField(TEXT("residentAfter"), ResidentAfter);
	Result->SetArrayField(TEXT("actors"), Affected);
	return MCPResult(Result);
#else
	return MCPError(TEXT("World Partition streaming control is editor-only"));
#endif
}
