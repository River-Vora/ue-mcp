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

#endif // WITH_DEV_AUTOMATION_TESTS
