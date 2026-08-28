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
#include "BehaviorTree/BTNode.h"

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

// #888: BTDecorator_Blackboard keeps its whole configuration in protected C++
// fields, so the graph read used to report class and name only. Every field a
// "the tree runs but the wrong branch fires" investigation needs is surfaced
// now, straight off the node.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPBehaviorTreeDecoratorConfigTest,
	"UE.MCP.Gameplay.BehaviorTree.DecoratorConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPBehaviorTreeDecoratorConfigTest::RunTest(const FString& Parameters)
{
	UClass* DecoratorClass = MCPBTTestFindClass(TEXT("/Script/AIModule.BTDecorator_Blackboard"));
	if (!DecoratorClass)
	{
		AddInfo(TEXT("BTDecorator_Blackboard is not present in this build; skipping."));
		return true;
	}
	UBTNode* Decorator = NewObject<UBTNode>(GetTransientPackage(), DecoratorClass);

	const TSharedPtr<FJsonObject> Described = FGameplayHandlers::DescribeBTNode(
		Decorator, TEXT("Root.Children[0].Decorators[0]"), TEXT("Root.Children[0]"));

	TestEqual(TEXT("kind"), MCPBTTestGetString(Described, TEXT("kind")), FString(TEXT("decorator")));
	TestEqual(TEXT("path"), MCPBTTestGetString(Described, TEXT("path")), FString(TEXT("Root.Children[0].Decorators[0]")));
	TestEqual(TEXT("parentPath"), MCPBTTestGetString(Described, TEXT("parentPath")), FString(TEXT("Root.Children[0]")));
	TestEqual(TEXT("class"), MCPBTTestGetString(Described, TEXT("class")), FString(TEXT("BTDecorator_Blackboard")));

	TestTrue(TEXT("flowAbortMode is reported"), Described->HasField(TEXT("flowAbortMode")));
	TestTrue(TEXT("notifyObserver is reported"), Described->HasField(TEXT("notifyObserver")));
	TestTrue(TEXT("inverseCondition is reported"), Described->HasField(TEXT("inverseCondition")));
#if WITH_EDITORONLY_DATA
	// The comparison operation is authored data, so the engine declares it
	// editor-only. It is the field that says whether the branch tests for
	// "is set" or "is not set".
	TestTrue(TEXT("basicOperation is reported"), Described->HasField(TEXT("basicOperation")));
#endif

	const TSharedPtr<FJsonObject>* BlackboardKey = nullptr;
	TestTrue(TEXT("blackboardKey is reported"), Described->TryGetObjectField(TEXT("blackboardKey"), BlackboardKey));
	if (BlackboardKey && (*BlackboardKey).IsValid())
	{
		TestTrue(TEXT("blackboardKey carries the selected key name"),
			(*BlackboardKey)->HasField(TEXT("selectedKeyName")));
	}

	// A null node is answered, not dereferenced.
	const TSharedPtr<FJsonObject> Empty = FGameplayHandlers::DescribeBTNode(nullptr, TEXT("Root"), FString());
	TestTrue(TEXT("a null node yields an empty object"), Empty.IsValid() && Empty->Values.Num() == 0);
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

	const TCHAR* Methods[] = {
		TEXT("get_behavior_tree_info"),
		TEXT("read_behavior_tree_graph"),
	};
	for (const TCHAR* Method : Methods)
	{
		TestTrue(FString::Printf(TEXT("%s is registered"), Method), Registry.HasHandler(Method));

		const TSharedPtr<FJsonValue> Response = Registry.ExecuteHandler(Method, MakeShared<FJsonObject>());
		TestTrue(FString::Printf(TEXT("%s returns an object"), Method),
			Response.IsValid() && Response->Type == EJson::Object);
		if (!Response.IsValid() || Response->Type != EJson::Object) continue;

		const TSharedPtr<FJsonObject> Object = Response->AsObject();
		TestFalse(FString::Printf(TEXT("%s rejects an empty call"), Method), Object->GetBoolField(TEXT("success")));
		TestTrue(FString::Printf(TEXT("%s names assetPath"), Method),
			Object->GetStringField(TEXT("error")).Contains(TEXT("assetPath")));
	}
	return true;
}

#endif
