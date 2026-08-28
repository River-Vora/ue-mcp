// BehaviorTree inspection.
//
// Split out of GameplayHandlers.cpp so the blackboard walk that used to take
// the editor down (#887) sits next to the types it reads.

#include "GameplayHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"

#include "EditorScriptingUtilities/Public/EditorAssetLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTDecorator.h"

namespace
{
	// Longest parent chain a Blackboard asset may declare before the walk stops.
	constexpr int32 MCPBTMaxBlackboardParents = 32;

	// "BlackboardKeyType_Object" reads as "Object" everywhere a key type is
	// reported. The full class path travels alongside it.
	FString MCPBTShortKeyTypeName(const UClass* KeyTypeClass)
	{
		if (!KeyTypeClass) return FString();
		FString Name = KeyTypeClass->GetName();
		const FString Prefix = TEXT("BlackboardKeyType_");
		if (Name.StartsWith(Prefix)) Name = Name.RightChop(Prefix.Len());
		return Name;
	}
}

// -----------------------------------------------------------------
// Blackboard keys
// -----------------------------------------------------------------

TArray<TSharedPtr<FJsonValue>> FGameplayHandlers::DescribeBlackboardKeys(const UBlackboardData* Blackboard)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	if (!Blackboard) return Out;

	// #887: UBlackboardData::Keys is a TArray<FBlackboardEntry>, a struct
	// array. The previous reader took each element's address, reinterpreted it
	// as a UObject** and dereferenced the result, which is the entry's FName
	// read as a pointer. Every blackboard with at least one key therefore
	// access-violated the editor. FBlackboardEntry is read as the struct it is.
	for (const FBlackboardEntry& Entry : Blackboard->Keys)
	{
		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());

		const UBlackboardKeyType* KeyType = Entry.KeyType.Get();
		const UClass* KeyTypeClass = KeyType ? KeyType->GetClass() : nullptr;
		KeyObj->SetStringField(TEXT("type"), MCPBTShortKeyTypeName(KeyTypeClass));
		KeyObj->SetStringField(TEXT("typeClass"), KeyTypeClass ? KeyTypeClass->GetPathName() : FString());
		KeyObj->SetBoolField(TEXT("instanceSynced"), Entry.bInstanceSynced != 0);
		KeyObj->SetStringField(TEXT("owner"), Blackboard->GetPathName());
#if WITH_EDITORONLY_DATA
		KeyObj->SetStringField(TEXT("description"), Entry.EntryDescription);
		KeyObj->SetStringField(TEXT("category"), Entry.EntryCategory.ToString());
#endif
		Out.Add(MakeShared<FJsonValueObject>(KeyObj));
	}
	return Out;
}

// -----------------------------------------------------------------
// get_behavior_tree_info (#887)
// -----------------------------------------------------------------

TSharedPtr<FJsonValue> FGameplayHandlers::GetBehaviorTreeInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("assetPath"), TEXT("path"), AssetPath)) return Err;

	UObject* Asset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!Asset)
	{
		return MCPError(FString::Printf(TEXT("BehaviorTree not found: %s"), *AssetPath));
	}

	UBehaviorTree* BT = Cast<UBehaviorTree>(Asset);
	if (!BT)
	{
		return MCPError(FString::Printf(
			TEXT("%s is a %s. get_behavior_tree_info reads a BehaviorTree asset - use asset(read_properties) for other types."),
			*AssetPath, *Asset->GetClass()->GetName()));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("path"), AssetPath);
	Result->SetStringField(TEXT("name"), BT->GetName());
	Result->SetStringField(TEXT("className"), BT->GetClass()->GetName());

	UBlackboardData* Blackboard = BT->BlackboardAsset;
	if (Blackboard)
	{
		Result->SetStringField(TEXT("blackboardAsset"), Blackboard->GetPathName());
		Result->SetArrayField(TEXT("blackboardKeys"), DescribeBlackboardKeys(Blackboard));

		// The parent chain contributes keys the tree can bind to just as well
		// as its own, and the seen set keeps a mis-authored cycle finite.
		TArray<TSharedPtr<FJsonValue>> Inherited;
		TSet<UBlackboardData*> Visited;
		Visited.Add(Blackboard);
		UBlackboardData* Parent = Blackboard->Parent;
		for (int32 Guard = 0; Parent && Guard < MCPBTMaxBlackboardParents; ++Guard)
		{
			bool bAlreadySeen = false;
			Visited.Add(Parent, &bAlreadySeen);
			if (bAlreadySeen) break;
			Inherited.Append(DescribeBlackboardKeys(Parent));
			Parent = Parent->Parent;
		}
		Result->SetArrayField(TEXT("inheritedBlackboardKeys"), Inherited);
	}
	else
	{
		Result->SetArrayField(TEXT("blackboardKeys"), TArray<TSharedPtr<FJsonValue>>());
	}

	if (BT->RootNode)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("path"), TEXT("Root"));
		Root->SetStringField(TEXT("class"), BT->RootNode->GetClass()->GetName());
		Root->SetStringField(TEXT("objectName"), BT->RootNode->GetName());
		Root->SetStringField(TEXT("nodeName"), BT->RootNode->NodeName);
		Result->SetObjectField(TEXT("rootNode"), Root);
	}

	Result->SetNumberField(TEXT("rootDecoratorCount"), BT->RootDecorators.Num());
	return MCPResult(Result);
}
