// Test-project fixture for #820: a TMap keyed by a struct, reachable both
// directly and through a wrapping struct.
//
// The bug this exists to catch is a read-then-write round trip on such a map
// emptying it, so the smoke test needs a property of exactly this shape. It
// lives in the test project (never shipped to users) rather than in the bridge
// plugin, because nothing but the tests should carry it.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "MCPStructKeyMapFixture.generated.h"

/** A struct key: a tag plus a slot, so keys stay distinct without a tag table. */
USTRUCT()
struct FMCPTestMapKey
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "MCP")
	FGameplayTag Tag;

	UPROPERTY(EditAnywhere, Category = "MCP")
	int32 Slot = 0;

	bool operator==(const FMCPTestMapKey& Other) const
	{
		return Tag == Other.Tag && Slot == Other.Slot;
	}
};

FORCEINLINE uint32 GetTypeHash(const FMCPTestMapKey& Key)
{
	return HashCombine(GetTypeHash(Key.Tag), ::GetTypeHash(Key.Slot));
}

/** The wrapping struct: writing this whole struct is the path that used to wipe the map. */
USTRUCT()
struct FMCPTestMapHolder
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "MCP")
	TMap<FMCPTestMapKey, TSoftObjectPtr<UObject>> Entries;

	UPROPERTY(EditAnywhere, Category = "MCP")
	int32 Revision = 0;
};

UCLASS()
class UE_MCP_API UMCPStructKeyMapFixture : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Write target for the wrapping-struct path ("Config" / "Config.Entries"). */
	UPROPERTY(EditAnywhere, Category = "MCP")
	FMCPTestMapHolder Config;

	/** Write target for the direct path, keyed by a plain name. */
	UPROPERTY(EditAnywhere, Category = "MCP")
	TMap<FName, int32> NamedCounts;
};
