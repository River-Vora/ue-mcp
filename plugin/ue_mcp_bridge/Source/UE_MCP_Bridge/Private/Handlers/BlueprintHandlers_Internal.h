#pragma once

// Helpers shared between BlueprintHandlers.cpp and BlueprintHandlers_Graph.cpp
// after the file was split. Kept in Private/ because it is internal to the
// plugin - no downstream code is expected to include this.

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UActorComponent;

// Resolve the named component template on a blueprint, honouring inheritance.
// See definition in BlueprintHandlers_Graph.cpp for the full contract (bForWrite
// semantics, ICH-override creation on write, CDO fallback on read, etc.).
UActorComponent* ResolveComponentTemplate(
	UBlueprint* Blueprint,
	const FString& ComponentName,
	bool bForWrite,
	bool& bOutIsInherited,
	TArray<FString>& OutAvailable);

// #942: a level script Blueprint is not an asset of its own. It lives inside
// the map package at "<Map>.<Map>:PersistentLevel.<Map>", so every Blueprint
// action handed the umap path a caller actually has answered "Blueprint not
// found". FBlueprintHandlers::LoadBlueprint now resolves a World path to that
// object; the two helpers below carry the shared reporting around it, so read,
// list_graphs, read_graph and get_execution_flow behave identically.
//
// Builds the "not found" response for a failed Blueprint lookup. When the path
// names a World whose level script has never been created, the message says so
// and prints the object path, rather than claiming the Blueprint is missing.
TSharedPtr<FJsonValue> BlueprintNotFoundError(const FString& AssetPath);

// Record which object actually answered the request. A caller that passed a
// umap path gets back the level script Blueprint's object path, so the alias it
// used is visible rather than implied.
void AnnotateResolvedBlueprint(const TSharedPtr<FJsonObject>& Result, UBlueprint* Blueprint);
