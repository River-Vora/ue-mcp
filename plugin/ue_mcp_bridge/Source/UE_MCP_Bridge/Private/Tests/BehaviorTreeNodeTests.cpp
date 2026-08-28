#if WITH_DEV_AUTOMATION_TESTS

#include "Handlers/GameplayHandlers.h"
#include "HandlerRegistry.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"

namespace
{
	// The key type class is resolved by path rather than included by header, so
	// a build without it skips that assertion instead of failing to link.
	UClass* MCPBTTestFindClass(const TCHAR* ClassPath)
	{
		return FindObject<UClass>(nullptr, ClassPath);
	}

	FString MCPBTTestGetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid()) Object->TryGetStringField(Field, Value);
		return Value;
	}
}

// #887: get_behavior_tree_info walked UBlackboardData::Keys as if it were an
// array of UObject pointers. It is an array of FBlackboardEntry structs, so the
// walk read each entry's FName as a pointer and dereferenced it, and every
// blackboard with at least one key took the editor down with an access
// violation. This test builds that exact shape in memory.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPBehaviorTreeBlackboardKeysTest,
	"UE.MCP.Gameplay.BehaviorTree.BlackboardKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPBehaviorTreeBlackboardKeysTest::RunTest(const FString& Parameters)
{
	UBlackboardData* Blackboard = NewObject<UBlackboardData>(GetTransientPackage());
	TestNotNull(TEXT("blackboard was created"), Blackboard);
	if (!Blackboard) return false;

	UClass* ObjectKeyClass = MCPBTTestFindClass(TEXT("/Script/AIModule.BlackboardKeyType_Object"));

	FBlackboardEntry Typed;
	Typed.EntryName = TEXT("TargetActor");
	if (ObjectKeyClass)
	{
		Typed.KeyType = NewObject<UBlackboardKeyType>(Blackboard, ObjectKeyClass);
	}
	Blackboard->Keys.Add(Typed);

	// A key whose type was never picked is a real authoring state, and the
	// reader has to describe it rather than dereference it.
	FBlackboardEntry Untyped;
	Untyped.EntryName = TEXT("PatrolIndex");
	Blackboard->Keys.Add(Untyped);

	const TArray<TSharedPtr<FJsonValue>> Keys = FGameplayHandlers::DescribeBlackboardKeys(Blackboard);
	TestEqual(TEXT("both keys are described"), Keys.Num(), 2);
	if (Keys.Num() != 2) return false;

	const TSharedPtr<FJsonObject> First = Keys[0]->AsObject();
	const TSharedPtr<FJsonObject> Second = Keys[1]->AsObject();
	TestEqual(TEXT("first key name"), MCPBTTestGetString(First, TEXT("name")), FString(TEXT("TargetActor")));
	TestEqual(TEXT("second key name"), MCPBTTestGetString(Second, TEXT("name")), FString(TEXT("PatrolIndex")));
	TestEqual(TEXT("untyped key reports an empty type instead of crashing"),
		MCPBTTestGetString(Second, TEXT("type")), FString());
	if (ObjectKeyClass)
	{
		TestEqual(TEXT("typed key reports its short type name"),
			MCPBTTestGetString(First, TEXT("type")), FString(TEXT("Object")));
	}

	// A null blackboard is an empty answer, never a dereference.
	TestEqual(TEXT("null blackboard yields no keys"),
		FGameplayHandlers::DescribeBlackboardKeys(nullptr).Num(), 0);
	return true;
}

// The action has to stay reachable under the name the TypeScript schema
// advertises, and answer a call with no parameters with a structured error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPBehaviorTreeRegistrationTest,
	"UE.MCP.Gameplay.BehaviorTree.RegistrationAndPreflight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPBehaviorTreeRegistrationTest::RunTest(const FString& Parameters)
{
	FMCPHandlerRegistry Registry;
	FGameplayHandlers::RegisterHandlers(Registry);

	TestTrue(TEXT("get_behavior_tree_info is registered"), Registry.HasHandler(TEXT("get_behavior_tree_info")));

	const TSharedPtr<FJsonValue> Response = Registry.ExecuteHandler(
		TEXT("get_behavior_tree_info"), MakeShared<FJsonObject>());
	TestTrue(TEXT("an empty call returns an object"), Response.IsValid() && Response->Type == EJson::Object);
	if (Response.IsValid() && Response->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Response->AsObject();
		TestFalse(TEXT("an empty call is unsuccessful"), Object->GetBoolField(TEXT("success")));
		TestTrue(TEXT("the error names assetPath"), Object->GetStringField(TEXT("error")).Contains(TEXT("assetPath")));
	}
	return true;
}

#endif
