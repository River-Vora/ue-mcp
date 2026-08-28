// BehaviorTree inspection.
//
// Split out of GameplayHandlers.cpp when the BT surface grew past "what asset
// is this" into reading the configuration each node carries (#887 crash fix,
// #888 decorator config).
//
// Everything here reads through UPROPERTY reflection rather than typed casts to
// concrete engine node classes. A project's own decorator that does not declare
// BlackboardKey, or an engine version that renames a field, is then described
// by what it does have instead of failing the whole read.

#include "GameplayHandlers.h"
#include "HandlerRegistry.h"
#include "HandlerUtils.h"

#include "JsonObjectConverter.h"
#include "EditorScriptingUtilities/Public/EditorAssetLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"

namespace
{
	// How deep a node walk goes before it stops. A BehaviorTree asset is a
	// tree, but a corrupted one is still an asset the bridge has to answer
	// about, and a handler must return a result rather than recurse forever.
	constexpr int32 MCPBTMaxDepth = 64;

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

	// One property value as JSON. Enum-valued bytes and enum-class properties
	// are emitted as their enumerator name, which is what a caller reads and
	// writes back; everything else goes through the JSON converter, with the
	// export text as the last resort for a type it cannot express.
	TSharedPtr<FJsonValue> MCPBTPropertyToJson(FProperty* Prop, const void* Addr)
	{
		if (!Prop || !Addr) return nullptr;

		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			if (UEnum* Enum = ByteProp->Enum)
			{
				const int64 Raw = ByteProp->GetSignedIntPropertyValue(Addr);
				return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(Raw));
			}
		}
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			if (UEnum* Enum = EnumProp->GetEnum())
			{
				const int64 Raw = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(Addr);
				return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(Raw));
			}
		}

		TSharedPtr<FJsonValue> Json = FJsonObjectConverter::UPropertyToJsonValue(Prop, Addr, 0, 0);
		if (Json.IsValid()) return Json;

		FString Text;
		Prop->ExportTextItem_Direct(Text, Addr, nullptr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(Text);
	}

	// Which node kind this object is, in the vocabulary the BT editor uses.
	FString MCPBTKind(const UBTNode* Node)
	{
		if (!Node) return FString();
		if (Node->IsA<UBTCompositeNode>()) return TEXT("composite");
		if (Node->IsA<UBTTaskNode>()) return TEXT("task");
		if (Node->IsA<UBTDecorator>()) return TEXT("decorator");
		if (Node->IsA<UBTService>()) return TEXT("service");
		return TEXT("node");
	}

	// One node in a walked tree, with the structural address a caller passes
	// back to target it: "Root.Children[1].Decorators[0]".
	struct FMCPBTNodeRef
	{
		UBTNode* Node = nullptr;
		FString Path;
		FString ParentPath;
		FString Kind;
	};

	void MCPBTCollectFrom(UBTNode* Node, const FString& Path, const FString& ParentPath,
		TArray<FMCPBTNodeRef>& Out, TSet<UBTNode*>& Seen, int32 Depth)
	{
		if (!Node || Depth > MCPBTMaxDepth) return;

		bool bAlreadySeen = false;
		Seen.Add(Node, &bAlreadySeen);
		if (bAlreadySeen) return;

		FMCPBTNodeRef Ref;
		Ref.Node = Node;
		Ref.Path = Path;
		Ref.ParentPath = ParentPath;
		Ref.Kind = MCPBTKind(Node);
		Out.Add(MoveTemp(Ref));

		if (UBTCompositeNode* Comp = Cast<UBTCompositeNode>(Node))
		{
			for (int32 s = 0; s < Comp->Services.Num(); ++s)
			{
				if (UBTService* Svc = Comp->Services[s])
				{
					MCPBTCollectFrom(Svc, FString::Printf(TEXT("%s.Services[%d]"), *Path, s), Path, Out, Seen, Depth + 1);
				}
			}
			for (int32 c = 0; c < Comp->Children.Num(); ++c)
			{
				const FBTCompositeChild& Child = Comp->Children[c];
				const FString ChildPath = FString::Printf(TEXT("%s.Children[%d]"), *Path, c);
				for (int32 d = 0; d < Child.Decorators.Num(); ++d)
				{
					if (UBTDecorator* Dec = Child.Decorators[d])
					{
						MCPBTCollectFrom(Dec, FString::Printf(TEXT("%s.Decorators[%d]"), *ChildPath, d), ChildPath, Out, Seen, Depth + 1);
					}
				}
				if (Child.ChildComposite)
				{
					MCPBTCollectFrom(Child.ChildComposite, ChildPath, Path, Out, Seen, Depth + 1);
				}
				else if (Child.ChildTask)
				{
					MCPBTCollectFrom(Child.ChildTask, ChildPath, Path, Out, Seen, Depth + 1);
				}
			}
		}
		else if (UBTTaskNode* Task = Cast<UBTTaskNode>(Node))
		{
			for (int32 s = 0; s < Task->Services.Num(); ++s)
			{
				if (UBTService* Svc = Task->Services[s])
				{
					MCPBTCollectFrom(Svc, FString::Printf(TEXT("%s.Services[%d]"), *Path, s), Path, Out, Seen, Depth + 1);
				}
			}
		}
	}

	void MCPBTCollectTree(UBehaviorTree* BT, TArray<FMCPBTNodeRef>& Out)
	{
		if (!BT) return;
		TSet<UBTNode*> Seen;
		for (int32 d = 0; d < BT->RootDecorators.Num(); ++d)
		{
			if (UBTDecorator* Dec = BT->RootDecorators[d])
			{
				MCPBTCollectFrom(Dec, FString::Printf(TEXT("RootDecorators[%d]"), d), FString(), Out, Seen, 0);
			}
		}
		if (BT->RootNode)
		{
			MCPBTCollectFrom(BT->RootNode, TEXT("Root"), FString(), Out, Seen, 0);
		}
	}

	// Load one BehaviorTree from a caller-supplied path.
	UBehaviorTree* MCPBTLoad(const FString& AssetPath)
	{
		if (UBehaviorTree* Direct = LoadObject<UBehaviorTree>(nullptr, *AssetPath)) return Direct;
		return Cast<UBehaviorTree>(UEditorAssetLibrary::LoadAsset(AssetPath));
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
// Node description (#888)
// -----------------------------------------------------------------

TSharedPtr<FJsonObject> FGameplayHandlers::DescribeBTNode(
	UBTNode* Node,
	const FString& NodePath,
	const FString& ParentPath)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Node) return Obj;

	UClass* NodeClass = Node->GetClass();
	Obj->SetStringField(TEXT("path"), NodePath);
	Obj->SetStringField(TEXT("parentPath"), ParentPath);
	Obj->SetStringField(TEXT("kind"), MCPBTKind(Node));
	Obj->SetStringField(TEXT("class"), NodeClass->GetName());
	Obj->SetStringField(TEXT("classPath"), NodeClass->GetPathName());
	Obj->SetStringField(TEXT("objectName"), Node->GetName());
	// `name` is what read_behavior_tree_graph has always reported for a node.
	Obj->SetStringField(TEXT("name"), Node->GetName());
	Obj->SetStringField(TEXT("nodeName"), Node->NodeName);
	Obj->SetStringField(TEXT("staticDescription"), Node->GetStaticDescription());

	// #888: the runtime configuration of a decorator is what answers "the tree
	// is running but the wrong branch fires". BlackboardKey, the comparison
	// operation, the abort mode and the observer mode are all protected C++
	// fields, so reflection is the only path to them.
	auto AddField = [&Obj, Node, NodeClass](const TCHAR* PropertyName, const TCHAR* JsonKey)
	{
		FProperty* Prop = NodeClass->FindPropertyByName(FName(PropertyName));
		if (!Prop) return;
		TSharedPtr<FJsonValue> Json = MCPBTPropertyToJson(Prop, Prop->ContainerPtrToValuePtr<void>(Node));
		if (Json.IsValid()) Obj->SetField(JsonKey, Json);
	};

	if (FStructProperty* KeyProp = CastField<FStructProperty>(NodeClass->FindPropertyByName(TEXT("BlackboardKey"))))
	{
		void* KeyAddr = KeyProp->ContainerPtrToValuePtr<void>(Node);
		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		if (FNameProperty* SelectedName = CastField<FNameProperty>(KeyProp->Struct->FindPropertyByName(TEXT("SelectedKeyName"))))
		{
			const FName Selected = SelectedName->GetPropertyValue(SelectedName->ContainerPtrToValuePtr<void>(KeyAddr));
			KeyObj->SetStringField(TEXT("selectedKeyName"), Selected.IsNone() ? FString() : Selected.ToString());
		}
		if (FObjectPropertyBase* SelectedType = CastField<FObjectPropertyBase>(KeyProp->Struct->FindPropertyByName(TEXT("SelectedKeyType"))))
		{
			const UClass* TypeClass = Cast<UClass>(SelectedType->GetObjectPropertyValue(SelectedType->ContainerPtrToValuePtr<void>(KeyAddr)));
			KeyObj->SetStringField(TEXT("selectedKeyType"), MCPBTShortKeyTypeName(TypeClass));
			KeyObj->SetStringField(TEXT("selectedKeyTypeClass"), TypeClass ? TypeClass->GetPathName() : FString());
		}
		Obj->SetObjectField(TEXT("blackboardKey"), KeyObj);
	}

	if (Node->IsA<UBTDecorator>())
	{
		AddField(TEXT("FlowAbortMode"), TEXT("flowAbortMode"));
		AddField(TEXT("bInverseCondition"), TEXT("inverseCondition"));
		AddField(TEXT("NotifyObserver"), TEXT("notifyObserver"));
		AddField(TEXT("BasicOperation"), TEXT("basicOperation"));
		AddField(TEXT("ArithmeticOperation"), TEXT("arithmeticOperation"));
		AddField(TEXT("TextOperation"), TEXT("textOperation"));
		AddField(TEXT("IntValue"), TEXT("intValue"));
		AddField(TEXT("FloatValue"), TEXT("floatValue"));
		AddField(TEXT("StringValue"), TEXT("stringValue"));
	}

	return Obj;
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

	TArray<FMCPBTNodeRef> Nodes;
	MCPBTCollectTree(BT, Nodes);
	Result->SetNumberField(TEXT("nodeCount"), Nodes.Num());
	Result->SetNumberField(TEXT("rootDecoratorCount"), BT->RootDecorators.Num());
	return MCPResult(Result);
}

// -----------------------------------------------------------------
// read_behavior_tree_graph (#124, decorator config added for #888)
// -----------------------------------------------------------------

TSharedPtr<FJsonValue> FGameplayHandlers::ReadBehaviorTreeGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (auto Err = RequireStringAlt(Params, TEXT("assetPath"), TEXT("path"), AssetPath)) return Err;

	UBehaviorTree* BT = MCPBTLoad(AssetPath);
	if (!BT) return MCPError(FString::Printf(TEXT("BehaviorTree not found: %s"), *AssetPath));

	int32 DecoratorCount = 0;
	int32 ServiceCount = 0;
	TSet<UBTNode*> Seen;

	auto Describe = [&](UBTNode* Node, const FString& Path, const FString& ParentPath)
	{
		return DescribeBTNode(Node, Path, ParentPath);
	};

	TFunction<TSharedPtr<FJsonObject>(UBTNode*, const FString&, const FString&, int32)> Walk;
	Walk = [&](UBTNode* Node, const FString& Path, const FString& ParentPath, int32 Depth) -> TSharedPtr<FJsonObject>
	{
		if (!Node || Depth > MCPBTMaxDepth) return nullptr;
		bool bAlreadySeen = false;
		Seen.Add(Node, &bAlreadySeen);
		if (bAlreadySeen) return nullptr;

		TSharedPtr<FJsonObject> NObj = Describe(Node, Path, ParentPath);

		if (UBTCompositeNode* Comp = Cast<UBTCompositeNode>(Node))
		{
			TArray<TSharedPtr<FJsonValue>> ChildrenArr;
			for (int32 c = 0; c < Comp->Children.Num(); ++c)
			{
				const FBTCompositeChild& Child = Comp->Children[c];
				const FString ChildPath = FString::Printf(TEXT("%s.Children[%d]"), *Path, c);

				TSharedPtr<FJsonObject> ChildEntry = MakeShared<FJsonObject>();
				ChildEntry->SetStringField(TEXT("path"), ChildPath);
				if (Child.ChildComposite)
				{
					if (TSharedPtr<FJsonObject> Sub = Walk(Child.ChildComposite, ChildPath, Path, Depth + 1))
					{
						ChildEntry->SetObjectField(TEXT("child"), Sub);
					}
				}
				else if (Child.ChildTask)
				{
					if (TSharedPtr<FJsonObject> Sub = Walk(Child.ChildTask, ChildPath, Path, Depth + 1))
					{
						ChildEntry->SetObjectField(TEXT("child"), Sub);
					}
				}

				TArray<TSharedPtr<FJsonValue>> Decorators;
				for (int32 d = 0; d < Child.Decorators.Num(); ++d)
				{
					UBTDecorator* Dec = Child.Decorators[d];
					if (!Dec) continue;
					const FString DecoratorPath = FString::Printf(TEXT("%s.Decorators[%d]"), *ChildPath, d);
					Decorators.Add(MakeShared<FJsonValueObject>(Describe(Dec, DecoratorPath, ChildPath)));
					++DecoratorCount;
				}
				ChildEntry->SetArrayField(TEXT("decorators"), Decorators);
				ChildrenArr.Add(MakeShared<FJsonValueObject>(ChildEntry));
			}
			NObj->SetArrayField(TEXT("children"), ChildrenArr);

			TArray<TSharedPtr<FJsonValue>> Services;
			for (int32 s = 0; s < Comp->Services.Num(); ++s)
			{
				UBTService* Svc = Comp->Services[s];
				if (!Svc) continue;
				Services.Add(MakeShared<FJsonValueObject>(Describe(Svc, FString::Printf(TEXT("%s.Services[%d]"), *Path, s), Path)));
				++ServiceCount;
			}
			NObj->SetArrayField(TEXT("services"), Services);
		}
		else if (UBTTaskNode* Task = Cast<UBTTaskNode>(Node))
		{
			TArray<TSharedPtr<FJsonValue>> Services;
			for (int32 s = 0; s < Task->Services.Num(); ++s)
			{
				UBTService* Svc = Task->Services[s];
				if (!Svc) continue;
				Services.Add(MakeShared<FJsonValueObject>(Describe(Svc, FString::Printf(TEXT("%s.Services[%d]"), *Path, s), Path)));
				++ServiceCount;
			}
			NObj->SetArrayField(TEXT("services"), Services);
		}
		return NObj;
	};

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("name"), BT->GetName());
	if (BT->BlackboardAsset)
	{
		Result->SetStringField(TEXT("blackboardAsset"), BT->BlackboardAsset->GetPathName());
		Result->SetArrayField(TEXT("blackboardKeys"), DescribeBlackboardKeys(BT->BlackboardAsset));
	}

	TArray<TSharedPtr<FJsonValue>> RootDecorators;
	for (int32 d = 0; d < BT->RootDecorators.Num(); ++d)
	{
		UBTDecorator* Dec = BT->RootDecorators[d];
		if (!Dec) continue;
		RootDecorators.Add(MakeShared<FJsonValueObject>(
			Describe(Dec, FString::Printf(TEXT("RootDecorators[%d]"), d), FString())));
		++DecoratorCount;
	}
	Result->SetArrayField(TEXT("rootDecorators"), RootDecorators);

	if (BT->RootNode)
	{
		if (TSharedPtr<FJsonObject> Root = Walk(BT->RootNode, TEXT("Root"), FString(), 0))
		{
			Result->SetObjectField(TEXT("root"), Root);
		}
	}

	Result->SetNumberField(TEXT("decoratorCount"), DecoratorCount);
	Result->SetNumberField(TEXT("serviceCount"), ServiceCount);
	return MCPResult(Result);
}
