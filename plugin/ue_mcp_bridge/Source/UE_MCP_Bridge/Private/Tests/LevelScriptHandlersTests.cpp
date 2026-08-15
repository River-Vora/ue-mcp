#if WITH_DEV_AUTOMATION_TESTS

#include "Handlers/LevelHandlers.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLevelScriptClearGraphNodesTest,
	"UE.MCP.Level.ClearLevelScript.GraphNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLevelScriptClearGraphNodesTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage());
	UEdGraph* Graph = NewObject<UEdGraph>(Blueprint, TEXT("EventGraph"));
	Blueprint->UbergraphPages.Add(Graph);
	Graph->AddNode(NewObject<UEdGraphNode>(Graph, TEXT("FirstNode")));
	Graph->AddNode(NewObject<UEdGraphNode>(Graph, TEXT("SecondNode")));

	TArray<TSharedPtr<FJsonValue>> Graphs;
	TestEqual(TEXT("dry run reports both nodes"),
		FLevelHandlers::ClearBlueprintGraphNodes(Blueprint, true, Graphs), 2);
	TestEqual(TEXT("dry run keeps both nodes"), Graph->Nodes.Num(), 2);
	TestEqual(TEXT("dry run reports the graph"), Graphs.Num(), 1);

	Graphs.Reset();
	TestEqual(TEXT("apply removes both nodes"),
		FLevelHandlers::ClearBlueprintGraphNodes(Blueprint, false, Graphs), 2);
	TestEqual(TEXT("apply leaves the graph empty"), Graph->Nodes.Num(), 0);
	return true;
}

#endif
