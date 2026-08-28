// Engine-side coverage for the Blueprint category surface that is safe to run
// anywhere.
//
// run_automation_tests dispatches every EditorContext/EngineFilter test in the
// process when it is called without a filter, against whatever project the
// bridge is attached to. Nothing below creates, compiles, saves or deletes an
// asset: every case is registration, argument validation, or a question asked
// of the engine's own reflection data. The behaviour that needs real assets is
// covered by the smoke suite against the dedicated test project instead.

#if WITH_DEV_AUTOMATION_TESTS

#include "HandlerRegistry.h"
#include "Handlers/BlueprintHandlers.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "UObject/UnrealType.h"

namespace
{
	TSharedPtr<FJsonObject> BlueprintTestResponseObject(const TSharedPtr<FJsonValue>& Response)
	{
		return (Response.IsValid() && Response->Type == EJson::Object) ? Response->AsObject() : nullptr;
	}
}

// ---------------------------------------------------------------------------
// #942: a World/umap path now resolves to that map's level script Blueprint.
// The half of that contract which needs no assets is the other half: a path
// that is neither a Blueprint nor a World must still report the plain
// "Blueprint not found" message, so the level-script hint cannot leak into an
// ordinary typo and send a caller looking for a map that was never named.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintNotFoundReportingTest,
	"UE.MCP.Blueprint.Surface.NotFoundReporting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintNotFoundReportingTest::RunTest(const FString& Parameters)
{
	FMCPHandlerRegistry Registry;
	FBlueprintHandlers::RegisterHandlers(Registry);

	// Every action named in #942 shares one lookup, so they share one message.
	const TCHAR* SharedLookupActions[] = {
		TEXT("read_blueprint"),
		TEXT("list_blueprint_graphs"),
		TEXT("read_blueprint_graph"),
		TEXT("get_blueprint_execution_flow"),
	};

	for (const TCHAR* Action : SharedLookupActions)
	{
		TestTrue(*FString::Printf(TEXT("%s is registered"), Action), Registry.HasHandler(Action));

		TSharedPtr<FJsonObject> BadPath = MakeShared<FJsonObject>();
		BadPath->SetStringField(TEXT("assetPath"), TEXT("/Game/UEMCPTests/BP_ThisAssetDoesNotExist"));
		BadPath->SetStringField(TEXT("graphName"), TEXT("EventGraph"));

		const TSharedPtr<FJsonObject> Response = BlueprintTestResponseObject(
			Registry.ExecuteHandler(Action, BadPath));
		if (!TestTrue(*FString::Printf(TEXT("%s returns an object"), Action), Response.IsValid()))
		{
			continue;
		}
		TestFalse(*FString::Printf(TEXT("%s rejects an unknown path"), Action),
			Response->GetBoolField(TEXT("success")));
		TestTrue(*FString::Printf(TEXT("%s reports a missing Blueprint, not a missing level script"), Action),
			Response->GetStringField(TEXT("error")).Contains(TEXT("Blueprint not found")));
	}

	// The lookup is also what rejects a request with no path at all, and that
	// has to stay a parameter error rather than becoming the World hint.
	{
		const TSharedPtr<FJsonObject> Response = BlueprintTestResponseObject(
			Registry.ExecuteHandler(TEXT("list_blueprint_graphs"), MakeShared<FJsonObject>()));
		if (TestTrue(TEXT("missing path returns an object"), Response.IsValid()))
		{
			TestFalse(TEXT("missing path is unsuccessful"), Response->GetBoolField(TEXT("success")));
			TestFalse(TEXT("missing path is not reported as a level script miss"),
				Response->GetStringField(TEXT("error")).Contains(TEXT("PersistentLevel")));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// #902: get_variable_default has to be reachable by name and has to reject a
// malformed request rather than throw. A handler that fails to register reports
// "Unknown method" at runtime with a clean build behind it, which is the
// failure mode this asserts away. The value path itself needs a real compiled
// Blueprint and is covered by the smoke suite.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintVariableDefaultRegistrationTest,
	"UE.MCP.Blueprint.VariableDefault.RegistrationAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintVariableDefaultRegistrationTest::RunTest(const FString& Parameters)
{
	FMCPHandlerRegistry Registry;
	FBlueprintHandlers::RegisterHandlers(Registry);

	TestTrue(TEXT("get_blueprint_variable_default is registered"),
		Registry.HasHandler(TEXT("get_blueprint_variable_default")));

	// assetPath is required.
	{
		TSharedPtr<FJsonObject> MissingPath = MakeShared<FJsonObject>();
		MissingPath->SetStringField(TEXT("name"), TEXT("Health"));
		const TSharedPtr<FJsonObject> Response = BlueprintTestResponseObject(
			Registry.ExecuteHandler(TEXT("get_blueprint_variable_default"), MissingPath));
		if (TestTrue(TEXT("missing assetPath returns an object"), Response.IsValid()))
		{
			TestFalse(TEXT("missing assetPath is unsuccessful"), Response->GetBoolField(TEXT("success")));
		}
	}

	// name is required, and is checked before the asset is touched so a caller
	// that forgot it is told which field it forgot.
	{
		TSharedPtr<FJsonObject> MissingName = MakeShared<FJsonObject>();
		MissingName->SetStringField(TEXT("assetPath"), TEXT("/Game/UEMCPTests/BP_ThisAssetDoesNotExist"));
		const TSharedPtr<FJsonObject> Response = BlueprintTestResponseObject(
			Registry.ExecuteHandler(TEXT("get_blueprint_variable_default"), MissingName));
		if (TestTrue(TEXT("missing name returns an object"), Response.IsValid()))
		{
			TestFalse(TEXT("missing name is unsuccessful"), Response->GetBoolField(TEXT("success")));
			TestTrue(TEXT("missing name identifies the field"),
				Response->GetStringField(TEXT("error")).Contains(TEXT("name")));
		}
	}

	// An unknown asset reports the shared Blueprint lookup failure, not a
	// property failure: the two are different problems with different fixes.
	{
		TSharedPtr<FJsonObject> UnknownAsset = MakeShared<FJsonObject>();
		UnknownAsset->SetStringField(TEXT("assetPath"), TEXT("/Game/UEMCPTests/BP_ThisAssetDoesNotExist"));
		UnknownAsset->SetStringField(TEXT("name"), TEXT("Health"));
		const TSharedPtr<FJsonObject> Response = BlueprintTestResponseObject(
			Registry.ExecuteHandler(TEXT("get_blueprint_variable_default"), UnknownAsset));
		if (TestTrue(TEXT("unknown asset returns an object"), Response.IsValid()))
		{
			TestFalse(TEXT("unknown asset is unsuccessful"), Response->GetBoolField(TEXT("success")));
			TestTrue(TEXT("unknown asset reports a missing Blueprint"),
				Response->GetStringField(TEXT("error")).Contains(TEXT("Blueprint not found")));
		}
	}

	// list_variables keeps its default payload: includeValues is opt-in, so a
	// request without it must not start resolving CDO values.
	{
		TSharedPtr<FJsonObject> MissingPath = MakeShared<FJsonObject>();
		const TSharedPtr<FJsonObject> Response = BlueprintTestResponseObject(
			Registry.ExecuteHandler(TEXT("list_blueprint_variables"), MissingPath));
		if (TestTrue(TEXT("list_variables with no path returns an object"), Response.IsValid()))
		{
			TestFalse(TEXT("list_variables with no path is unsuccessful"),
				Response->GetBoolField(TEXT("success")));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// #886: authoring a BTTask Blueprint's event graph needs ReceiveExecuteAI and
// ReceiveAbortAI as override events and FinishExecute / FinishAbort as call
// nodes. The existing override_function and add_node surface already places
// all four; what makes that true is the shape those two actions test for on
// the engine side, so that shape is what is asserted here. If Epic ever
// changes one of these functions, the recipe the skill documents becomes wrong
// here first, in a test, rather than in somebody's build.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintBTTaskEventShapeTest,
	"UE.MCP.Blueprint.BTTask.EventShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBTTaskEventShapeTest::RunTest(const FString& Parameters)
{
	UClass* TaskBase = UBTTask_BlueprintBase::StaticClass();
	if (!TestNotNull(TEXT("UBTTask_BlueprintBase resolves"), TaskBase))
	{
		return false;
	}

	// override_function accepts a name only when GetOverrideFunctionClass can
	// resolve it, which needs CanKismetOverrideFunction; it places an override
	// EVENT rather than a function graph only when FunctionCanBePlacedAsEvent
	// agrees. Both have to hold for the documented call sequence to work.
	const TCHAR* OverrideEvents[] = {
		TEXT("ReceiveExecuteAI"), TEXT("ReceiveAbortAI"), TEXT("ReceiveTickAI"),
		TEXT("ReceiveExecute"), TEXT("ReceiveAbort"), TEXT("ReceiveTick"),
	};
	for (const TCHAR* EventName : OverrideEvents)
	{
		UFunction* Function = TaskBase->FindFunctionByName(FName(EventName));
		if (!TestNotNull(*FString::Printf(TEXT("%s exists on UBTTask_BlueprintBase"), EventName), Function))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s is overridable from Kismet"), EventName),
			UEdGraphSchema_K2::CanKismetOverrideFunction(Function));
		TestTrue(*FString::Printf(TEXT("%s can be placed as an override event"), EventName),
			UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Function));
	}

	// add_node(nodeClass="CallFunction") binds a K2Node_CallFunction to a
	// UFUNCTION resolved by name on the named class, and only a
	// BlueprintCallable function yields a usable node.
	const TCHAR* CallableFinishers[] = { TEXT("FinishExecute"), TEXT("FinishAbort") };
	for (const TCHAR* CallName : CallableFinishers)
	{
		UFunction* Function = TaskBase->FindFunctionByName(FName(CallName));
		if (!TestNotNull(*FString::Printf(TEXT("%s exists on UBTTask_BlueprintBase"), CallName), Function))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s is BlueprintCallable"), CallName),
			Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	// The skill documents setting bSuccess as a pin default on FinishExecute,
	// which only holds while the parameter is there and is a bool.
	if (UFunction* FinishExecute = TaskBase->FindFunctionByName(TEXT("FinishExecute")))
	{
		FProperty* SuccessParam = FinishExecute->FindPropertyByName(TEXT("bSuccess"));
		if (TestNotNull(TEXT("FinishExecute exposes bSuccess"), SuccessParam))
		{
			TestTrue(TEXT("FinishExecute's bSuccess is a bool"), SuccessParam->IsA<FBoolProperty>());
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
