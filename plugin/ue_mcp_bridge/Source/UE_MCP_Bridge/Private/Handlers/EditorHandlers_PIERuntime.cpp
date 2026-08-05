// Runtime (PIE) inspection and control that previously required Python.
//
// Covers the cluster of agent reports #739, #756, #757, #761, #764, #770,
// #777, #778: calling functions on non-actor UObjects, reading live skeletal
// bone and socket transforms, teleporting a possessed character in a way that
// CharacterMovement does not immediately undo, and reaching PIE worlds other
// than the primary one in a multiplayer session.
//
// Translation-unit partition of FEditorHandlers; registrations live in
// EditorHandlers.cpp::RegisterHandlers.

#include "EditorHandlers.h"

#include "HandlerRegistry.h"
#include "HandlerUtils.h"
#include "HandlerJsonProperty.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/Script.h"
#include "UObject/UObjectIterator.h"
#include "UObject/GCObjectScopeGuard.h"

namespace
{
	TSharedPtr<FJsonObject> VectorJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	TSharedPtr<FJsonObject> RotatorJson(const FRotator& R)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("pitch"), R.Pitch);
		O->SetNumberField(TEXT("yaw"), R.Yaw);
		O->SetNumberField(TEXT("roll"), R.Roll);
		return O;
	}

	TSharedPtr<FJsonObject> TransformJson(const FTransform& T)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("location"), VectorJson(T.GetLocation()));
		O->SetObjectField(TEXT("rotation"), RotatorJson(T.Rotator()));
		O->SetObjectField(TEXT("scale"), VectorJson(T.GetScale3D()));
		return O;
	}

	/**
	 * #739: resolve any UObject a caller might want to call into, not just a
	 * placed actor. Accepts an explicit object path, or one of the well-known
	 * runtime singletons an agent actually reaches for, none of which have an
	 * actor label to target: the GameInstance, GameMode, GameState, a player
	 * controller/pawn, or a named subsystem.
	 */
	UObject* ResolveRuntimeObject(
		const TSharedPtr<FJsonObject>& Params,
		UWorld* World,
		FString& OutDescription,
		FString& OutError)
	{
		const FString ObjectPath = OptionalString(Params, TEXT("objectPath"));
		if (!ObjectPath.IsEmpty())
		{
			// FindObject first: in PIE the live instance already exists and
			// loading would resolve the editor-world asset instead.
			UObject* Found = FindObject<UObject>(nullptr, *ObjectPath);
			if (!Found) Found = LoadObject<UObject>(nullptr, *ObjectPath);
			if (!Found)
			{
				OutError = FString::Printf(TEXT("Object not found: %s"), *ObjectPath);
				return nullptr;
			}
			OutDescription = Found->GetPathName();
			return Found;
		}

		const FString Target = OptionalString(Params, TEXT("target")).ToLower();
		if (Target.IsEmpty())
		{
			OutError = TEXT("Provide 'objectPath', or 'target' (gameinstance|gamemode|gamestate|playercontroller|playerpawn|subsystem)");
			return nullptr;
		}
		if (!World)
		{
			OutError = TEXT("No world available to resolve a runtime target against");
			return nullptr;
		}

		const int32 PlayerIndex = OptionalInt(Params, TEXT("playerIndex"), 0);

		if (Target == TEXT("gameinstance"))
		{
			UGameInstance* GI = World->GetGameInstance();
			if (!GI) { OutError = TEXT("World has no GameInstance"); return nullptr; }
			OutDescription = GI->GetPathName();
			return GI;
		}
		if (Target == TEXT("gamemode"))
		{
			AGameModeBase* GM = World->GetAuthGameMode();
			if (!GM) { OutError = TEXT("World has no authoritative GameMode (clients do not have one)"); return nullptr; }
			OutDescription = GM->GetPathName();
			return GM;
		}
		if (Target == TEXT("gamestate"))
		{
			AGameStateBase* GS = World->GetGameState();
			if (!GS) { OutError = TEXT("World has no GameState"); return nullptr; }
			OutDescription = GS->GetPathName();
			return GS;
		}
		if (Target == TEXT("playercontroller"))
		{
			APlayerController* PC = UGameplayStatics::GetPlayerController(World, PlayerIndex);
			if (!PC) { OutError = FString::Printf(TEXT("No player controller at index %d"), PlayerIndex); return nullptr; }
			OutDescription = PC->GetPathName();
			return PC;
		}
		if (Target == TEXT("playerpawn"))
		{
			APawn* Pawn = UGameplayStatics::GetPlayerPawn(World, PlayerIndex);
			if (!Pawn) { OutError = FString::Printf(TEXT("No player pawn at index %d"), PlayerIndex); return nullptr; }
			OutDescription = Pawn->GetPathName();
			return Pawn;
		}
		if (Target == TEXT("subsystem"))
		{
			FString SubsystemName;
			if (!Params->TryGetStringField(TEXT("subsystemClass"), SubsystemName) || SubsystemName.IsEmpty())
			{
				OutError = TEXT("target=subsystem requires 'subsystemClass'");
				return nullptr;
			}
			UClass* Cls = FindFirstObject<UClass>(*SubsystemName, EFindFirstObjectOptions::None);
			if (!Cls) Cls = LoadObject<UClass>(nullptr, *SubsystemName);
			if (!Cls)
			{
				OutError = FString::Printf(TEXT("Subsystem class not found: %s"), *SubsystemName);
				return nullptr;
			}
			UObject* Found = nullptr;
			if (Cls->IsChildOf(UWorldSubsystem::StaticClass()))
			{
				Found = World->GetSubsystemBase(Cls);
			}
			else if (Cls->IsChildOf(UGameInstanceSubsystem::StaticClass()))
			{
				if (UGameInstance* GI = World->GetGameInstance())
				{
					Found = GI->GetSubsystemBase(Cls);
				}
			}
			else if (Cls->IsChildOf(UEngineSubsystem::StaticClass()) && GEngine)
			{
				Found = GEngine->GetEngineSubsystemBase(Cls);
			}
			else if (GEditor)
			{
				Found = GEditor->GetEditorSubsystemBase(Cls);
			}
			if (!Found)
			{
				OutError = FString::Printf(TEXT("Subsystem '%s' is not active in this context"), *SubsystemName);
				return nullptr;
			}
			OutDescription = Found->GetPathName();
			return Found;
		}

		OutError = FString::Printf(TEXT("Unknown target '%s'. Use gameinstance|gamemode|gamestate|playercontroller|playerpawn|subsystem, or pass objectPath."), *Target);
		return nullptr;
	}

	/** Marshal JSON args into a UFunction frame, call it, and read outputs back. */
	TSharedPtr<FJsonValue> CallFunctionWithJsonArgs(
		UObject* CallTarget,
		const FString& FunctionName,
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Result)
	{
		UFunction* Func = CallTarget->FindFunction(FName(*FunctionName));
		if (!Func)
		{
			// List candidates: guessing a UFUNCTION name is the main failure mode.
			TArray<FString> Names;
			for (TFieldIterator<UFunction> It(CallTarget->GetClass()); It && Names.Num() < 40; ++It)
			{
				Names.Add(It->GetName());
			}
			return MCPError(FString::Printf(
				TEXT("Function '%s' not found on %s. Available: [%s]"),
				*FunctionName, *CallTarget->GetClass()->GetName(), *FString::Join(Names, TEXT(", "))));
		}

		TArray<uint8> ParamBuf;
		ParamBuf.SetNumZeroed(Func->ParmsSize);
		for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(ParamBuf.GetData());
		}

		auto Cleanup = [&]()
		{
			for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
			{
				It->DestroyValue_InContainer(ParamBuf.GetData());
			}
		};

		const TSharedPtr<FJsonObject>* ArgObj = nullptr;
		Params->TryGetObjectField(TEXT("args"), ArgObj);
		if (ArgObj && (*ArgObj).IsValid())
		{
			for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
			{
				FProperty* P = *It;
				if (P->PropertyFlags & CPF_ReturnParm) continue;
				if ((P->PropertyFlags & CPF_OutParm) && !(P->PropertyFlags & CPF_ReferenceParm)) continue;
				TSharedPtr<FJsonValue> Val = (*ArgObj)->TryGetField(P->GetName());
				if (!Val.IsValid()) continue;
				FString E;
				if (!MCPJsonProperty::SetJsonOnProperty(P, P->ContainerPtrToValuePtr<void>(ParamBuf.GetData()), Val, E))
				{
					Cleanup();
					return MCPError(FString::Printf(TEXT("Argument '%s': %s"), *P->GetName(), *E));
				}
			}
		}

		// ProcessEvent can run arbitrary game code, including code that tears
		// down the world and collects garbage, and CallTarget is read again
		// below to export out params.
		FGCObjectScopeGuard TargetGuard(CallTarget);
		// #806: an actor whose world never initialised for play (every editor
		// world) silently skips ProcessEvent unless the function is marked
		// CallInEditor, leaving the zeroed frame to be exported as the result.
		// The guard opens that gate for the duration of this call only.
		{
			FEditorScriptExecutionGuard ScriptGuard;
			CallTarget->ProcessEvent(Func, ParamBuf.GetData());
		}
		// NOTE: UObject* out-params live in ParamBuf, which is raw bytes and
		// invisible to GC. Guarding them after the fact cannot help - by then a
		// collection has already happened - so out-param objects are validated
		// with IsValid() at export time instead.

		TSharedPtr<FJsonObject> OutVals = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			FProperty* P = *It;
			if (P->PropertyFlags & (CPF_ReturnParm | CPF_OutParm))
			{
				// An object out-param may have been collected during the call:
				// ParamBuf is raw bytes and invisible to GC, so exporting it
				// would dereference freed memory. Check first, then export
				// through the same path as every other property so the wire
				// format does not diverge between call actions.
				if (FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(P))
				{
					UObject* Out = OP->GetObjectPropertyValue(OP->ContainerPtrToValuePtr<void>(ParamBuf.GetData()));
					if (!Out)
					{
						OutVals->SetStringField(P->GetName(), TEXT("None"));
						continue;
					}
					if (!IsValid(Out))
					{
						// Distinct from "None": the call returned an object and
						// then something destroyed it. Reporting an empty string
						// for both would read as a null return.
						OutVals->SetStringField(P->GetName(), TEXT("(collected during the call)"));
						continue;
					}
				}
				FString S;
				P->ExportTextItem_Direct(S, P->ContainerPtrToValuePtr<void>(ParamBuf.GetData()), nullptr, CallTarget, PPF_None);
				OutVals->SetStringField(P->GetName(), S);
			}
		}
		Cleanup();

		Result->SetStringField(TEXT("functionName"), FunctionName);
		Result->SetObjectField(TEXT("returnValues"), OutVals);
		return MCPResult(Result);
	}
}

// #778: enumerate the running PIE worlds so a caller can see that a client
// exists at all, and learn the instance id to address it with.
TSharedPtr<FJsonValue> FEditorHandlers::ListPIEInstances(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEngine) return MCPError(TEXT("Engine not available"));

	TArray<TSharedPtr<FJsonValue>> Instances;
	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
	{
		if (Ctx.WorldType != EWorldType::PIE && Ctx.WorldType != EWorldType::Game) continue;
		UWorld* World = Ctx.World();
		if (!World) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("pieInstance"), Ctx.PIEInstance);
		Entry->SetStringField(TEXT("worldPath"), World->GetPathName());
		Entry->SetStringField(TEXT("worldName"), World->GetName());
		Entry->SetStringField(TEXT("netMode"), DescribePIENetMode(World));
		Entry->SetBoolField(TEXT("isServer"), World->GetNetMode() != NM_Client);
		Entry->SetBoolField(TEXT("hasGameViewport"), World->GetGameViewport() != nullptr);
		Entry->SetNumberField(TEXT("playerCount"), World->GetNumPlayerControllers());
		if (AGameStateBase* GS = World->GetGameState())
		{
			Entry->SetStringField(TEXT("gameState"), GS->GetClass()->GetName());
		}
		Instances.Add(MakeShared<FJsonValueObject>(Entry));
	}

	auto Result = MCPSuccess();
	Result->SetArrayField(TEXT("instances"), Instances);
	Result->SetNumberField(TEXT("count"), Instances.Num());
	if (Instances.Num() == 0)
	{
		Result->SetStringField(TEXT("note"), TEXT("PIE is not running. Start it with editor(play_in_editor)."));
	}
	return MCPResult(Result);
}

// #739: invoke a UFUNCTION on any UObject, not just a placed actor. The
// GameInstance, GameMode, GameState and subsystems have no actor label, so
// invoke_function could never reach them and every save-game or subsystem test
// fell back to execute_python.
TSharedPtr<FJsonValue> FEditorHandlers::InvokeObjectFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString FunctionName;
	if (auto Err = RequireString(Params, TEXT("functionName"), FunctionName)) return Err;

	UWorld* World = ResolveWorldFromParams(Params, TEXT("auto"));

	FString Description, Error;
	UObject* Target = ResolveRuntimeObject(Params, World, Description, Error);
	if (!Target) return MCPError(Error);

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("objectPath"), Description);
	Result->SetStringField(TEXT("objectClass"), Target->GetClass()->GetName());
	if (World)
	{
		Result->SetStringField(TEXT("world"), World->GetPathName());
		Result->SetStringField(TEXT("netMode"), DescribePIENetMode(World));
	}
	return CallFunctionWithJsonArgs(Target, FunctionName, Params, Result);
}

// #739: read reflected properties off any UObject, same resolution rules as
// invoke_object_function. Reading a GameInstance's save-game variable had the
// same "no actor label" problem as calling a function on it.
TSharedPtr<FJsonValue> FEditorHandlers::GetObjectProperties(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = ResolveWorldFromParams(Params, TEXT("auto"));

	FString Description, Error;
	UObject* Target = ResolveRuntimeObject(Params, World, Description, Error);
	if (!Target) return MCPError(Error);

	TArray<FString> Wanted;
	const TArray<TSharedPtr<FJsonValue>>* NameValues = nullptr;
	if (Params->TryGetArrayField(TEXT("propertyNames"), NameValues) && NameValues)
	{
		for (const TSharedPtr<FJsonValue>& V : *NameValues)
		{
			FString N;
			if (V.IsValid() && V->TryGetString(N) && !N.IsEmpty()) Wanted.Add(N);
		}
	}

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Missing;
	TSet<FString> Emitted;
	int32 Count = 0;
	// Bound the response. Exporting every reflected property of something like
	// a GameState with replicated arrays builds a payload big enough to drop
	// the bridge - the same failure asset(list) was just paginated for.
	const int32 MaxProperties = FMath::Clamp(OptionalInt(Params, TEXT("limit"), 200), 1, 5000);
	const int32 MaxValueChars = FMath::Clamp(OptionalInt(Params, TEXT("maxValueLength"), 2000), 64, 100000);
	int32 Skipped = 0;
	int32 TruncatedValues = 0;
	for (TFieldIterator<FProperty> It(Target->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* P = *It;
		if (!P) continue;
		if (Wanted.Num() > 0 && !Wanted.ContainsByPredicate(
				[&](const FString& N) { return N.Equals(P->GetName(), ESearchCase::IgnoreCase); }))
		{
			continue;
		}
		if (Count >= MaxProperties)
		{
			// Record it as seen so a capped-but-real property is not reported
			// under missingProperties, which means "no such property".
			Emitted.Add(P->GetName().ToLower());
			++Skipped;
			continue;
		}
		FString S;
		P->ExportTextItem_Direct(S, P->ContainerPtrToValuePtr<void>(Target), nullptr, Target, PPF_None);
		if (S.Len() > MaxValueChars)
		{
			S = S.Left(MaxValueChars) + FString::Printf(TEXT("... [truncated, %d chars]"), S.Len());
			++TruncatedValues;
		}
		Props->SetStringField(P->GetName(), S);
		Emitted.Add(P->GetName().ToLower());
		++Count;
	}
	// Name a requested property that does not exist, so a typo is reported
	// rather than quietly returning an empty object.
	for (const FString& N : Wanted)
	{
		if (!Emitted.Contains(N.ToLower()))
		{
			Missing.Add(MakeShared<FJsonValueString>(N));
		}
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("objectPath"), Description);
	Result->SetStringField(TEXT("objectClass"), Target->GetClass()->GetName());
	Result->SetNumberField(TEXT("propertyCount"), Count);
	Result->SetNumberField(TEXT("skippedProperties"), Skipped);
	Result->SetNumberField(TEXT("truncatedValues"), TruncatedValues);
	Result->SetObjectField(TEXT("properties"), Props);
	Result->SetArrayField(TEXT("missingProperties"), Missing);
	if (Skipped > 0)
	{
		Result->SetStringField(TEXT("note"), FString::Printf(
			TEXT("%d properties omitted past the %d limit. Pass propertyNames to target specific ones, or raise 'limit'."),
			Skipped, MaxProperties));
	}
	return MCPResult(Result);
}

// #756/#757/#761/#764: sample live skeletal bone and socket transforms. Every
// one of these reports had to drop to Python purely to read where a hand or
// foot actually was at runtime, which is the evidence an animation check is
// built on.
TSharedPtr<FJsonValue> FEditorHandlers::ReadBoneTransforms(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = ResolveWorldFromParams(Params, TEXT("auto"));
	if (!World) return MCPError(TEXT("No world available"));

	FString ActorLabel;
	if (auto Err = RequireString(Params, TEXT("actorLabel"), ActorLabel)) return Err;

	AActor* Actor = FindActorByLabelNameOrPath(World, ActorLabel);
	if (!Actor)
	{
		return MCPError(FString::Printf(TEXT("Actor not found (by label, name or path): %s"), *ActorLabel));
	}

	const FString ComponentName = OptionalString(Params, TEXT("componentName"));
	USkeletalMeshComponent* Mesh = nullptr;
	TArray<FString> SkeletalComponents;
	for (UActorComponent* Comp : Actor->GetComponents())
	{
		USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(Comp);
		if (!SkelComp) continue;
		SkeletalComponents.Add(SkelComp->GetName());
		if (ComponentName.IsEmpty() || SkelComp->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
		{
			if (!Mesh) Mesh = SkelComp;
		}
	}
	if (!Mesh)
	{
		return MCPError(FString::Printf(
			TEXT("No matching SkeletalMeshComponent on '%s'. Available: [%s]"),
			*ActorLabel, *FString::Join(SkeletalComponents, TEXT(", "))));
	}

	// A component whose transforms have never been evaluated returns
	// FTransform::Identity for every bone, and reporting that as measurement
	// data is worse than failing - this handler exists to supply evidence.
	//
	// GetNumComponentSpaceTransforms alone is NOT a validity test:
	// AllocateTransformData fills the array with identity on register, so an
	// unevaluated component sails past a count check. The engine's own flag
	// (bHasValidBoneTransform / AreBoneTransformsValid) is protected and not
	// reflected, so detect the pathological signature directly: a multi-bone
	// skeleton whose component-space transforms are ALL exactly identity has
	// not been posed. A posed mesh exits on the first non-identity bone.
	// A leader-pose follower deliberately has an EMPTY component-space array
	// (AllocateTransformData skips it when a leader is set) yet resolves
	// transforms correctly through the leader, which is what GetSocketTransform
	// below actually uses. Only validate components that own their pose.
	const bool bFollowsLeader = Mesh->LeaderPoseComponent.IsValid();
	if (!Mesh->GetSkinnedAsset())
	{
		return MCPError(FString::Printf(
			TEXT("SkeletalMeshComponent '%s' on '%s' has no skinned asset."),
			*Mesh->GetName(), *ActorLabel));
	}
	if (!bFollowsLeader && Mesh->GetNumComponentSpaceTransforms() == 0)
	{
		return MCPError(FString::Printf(
			TEXT("SkeletalMeshComponent '%s' on '%s' has no bone transform data (not registered)."),
			*Mesh->GetName(), *ActorLabel));
	}
	{
		// A follower's own array is empty by design; the pose it resolves comes
		// from the leader, so that is what has to have been evaluated.
		const USkinnedMeshComponent* PoseSource =
			bFollowsLeader ? Mesh->LeaderPoseComponent.Get() : static_cast<const USkinnedMeshComponent*>(Mesh);
		const TArray<FTransform>& Spaces = PoseSource->GetComponentSpaceTransforms();
		if (Spaces.Num() == 0)
		{
			// The zero-transform guard above is skipped for followers, so this
			// is the only thing standing between an unregistered LEADER and a
			// full set of identity transforms reported as measurements.
			return MCPError(FString::Printf(
				TEXT("SkeletalMeshComponent '%s' on '%s' has no bone transform data%s (not registered)."),
				*Mesh->GetName(), *ActorLabel,
				bFollowsLeader ? TEXT(" on its leader pose component") : TEXT("")));
		}
		// A single-bone skeleton is legitimately identity; more than one is not.
		bool bAnyPosed = Spaces.Num() == 1;
		const int32 Probe = FMath::Min(Spaces.Num(), 32);
		for (int32 i = 0; i < Probe && !bAnyPosed; ++i)
		{
			if (!Spaces[i].Equals(FTransform::Identity, UE_KINDA_SMALL_NUMBER)) bAnyPosed = true;
		}
		if (!bAnyPosed)
		{
			return MCPError(FString::Printf(
				TEXT("SkeletalMeshComponent '%s' on '%s' has not evaluated its bone transforms yet - every bone reads as identity, which would be reported as real measurement data. Is PIE running, and has the mesh ticked?%s"),
				*Mesh->GetName(), *ActorLabel,
				bFollowsLeader ? TEXT(" (the pose comes from its leader pose component, which is the one that has not evaluated)") : TEXT("")));
		}
	}

	// "world" (default) or "component" space. Component space is what an
	// animation assertion usually wants, since it is independent of where the
	// actor happens to be standing.
	const bool bComponentSpace = OptionalString(Params, TEXT("space"), TEXT("world")).ToLower() == TEXT("component");
	const FTransform ComponentToWorld = Mesh->GetComponentTransform();

	TArray<FString> RequestedBones;
	const TArray<TSharedPtr<FJsonValue>>* BoneValues = nullptr;
	if (Params->TryGetArrayField(TEXT("bones"), BoneValues) && BoneValues)
	{
		for (const TSharedPtr<FJsonValue>& V : *BoneValues)
		{
			FString N;
			if (V.IsValid() && V->TryGetString(N) && !N.IsEmpty()) RequestedBones.Add(N);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Samples;
	TArray<TSharedPtr<FJsonValue>> Unknown;

	auto AddSample = [&](const FString& Name, bool bIsSocket)
	{
		// GetSocketTransform resolves sockets first, then bones, so one call
		// covers both; bIsSocket only labels which one answered.
		const FTransform WorldTransform = Mesh->GetSocketTransform(FName(*Name), RTS_World);
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetBoolField(TEXT("isSocket"), bIsSocket);
		Entry->SetObjectField(TEXT("transform"), TransformJson(
			bComponentSpace ? WorldTransform.GetRelativeTransform(ComponentToWorld) : WorldTransform));
		Samples.Add(MakeShared<FJsonValueObject>(Entry));
	};

	if (RequestedBones.Num() > 0)
	{
		for (const FString& Name : RequestedBones)
		{
			const FName AsName(*Name);
			const bool bIsSocket = Mesh->DoesSocketExist(AsName);
			const bool bIsBone = Mesh->GetBoneIndex(AsName) != INDEX_NONE;
			if (!bIsSocket && !bIsBone)
			{
				Unknown.Add(MakeShared<FJsonValueString>(Name));
				continue;
			}
			// A socket wins the lookup even when a bone shares its name, so
			// report isSocket by what actually resolved, not by exclusion.
			AddSample(Name, bIsSocket);
		}
	}
	else
	{
		// No explicit list: report every bone, capped so a full skeleton on a
		// dense rig cannot blow up the response.
		const int32 Limit = FMath::Max(1, OptionalInt(Params, TEXT("limit"), 200));
		const int32 NumBones = Mesh->GetNumBones();
		for (int32 i = 0; i < NumBones && Samples.Num() < Limit; ++i)
		{
			AddSample(Mesh->GetBoneName(i).ToString(), false);
		}
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetStringField(TEXT("component"), Mesh->GetName());
	Result->SetStringField(TEXT("space"), bComponentSpace ? TEXT("component") : TEXT("world"));
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetNumberField(TEXT("boneCount"), Mesh->GetNumBones());
	Result->SetArrayField(TEXT("samples"), Samples);
	Result->SetArrayField(TEXT("unknownNames"), Unknown);
	if (UAnimInstance* Anim = Mesh->GetAnimInstance())
	{
		Result->SetStringField(TEXT("animInstanceClass"), Anim->GetClass()->GetName());
		Result->SetStringField(TEXT("animInstancePath"), Anim->GetPathName());
	}
	return MCPResult(Result);
}

// #770/#777: move a live actor in PIE and have it stay moved. Plain
// SetActorLocation on a Character is immediately undone by CharacterMovement,
// so the reports had to stop movement, teleport, and stop movement again by
// hand in Python.
TSharedPtr<FJsonValue> FEditorHandlers::TeleportRuntimeActor(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = ResolveWorldFromParams(Params, TEXT("pie"));
	if (!World) return MCPError(TEXT("PIE is not running - teleport_runtime_actor targets a live world"));

	FString ActorLabel;
	if (auto Err = RequireString(Params, TEXT("actorLabel"), ActorLabel)) return Err;

	AActor* Actor = FindActorByLabelNameOrPath(World, ActorLabel);
	if (!Actor)
	{
		return MCPError(FString::Printf(TEXT("Actor not found in the live world (by label, name or path): %s"), *ActorLabel));
	}

	const FVector StartLocation = Actor->GetActorLocation();
	const FVector Location = Params->HasField(TEXT("location"))
		? OptionalVec3(Params, TEXT("location"))
		: StartLocation;
	const bool bHasRotation = Params->HasField(TEXT("rotation"));
	const FRotator Rotation = bHasRotation ? OptionalRotator(Params, TEXT("rotation")) : Actor->GetActorRotation();

	// Stop the movement component first, otherwise the pending velocity is
	// re-applied on the next tick and the actor slides straight back.
	const bool bStopMovement = OptionalBool(Params, TEXT("stopMovement"), true);
	UPawnMovementComponent* Movement = nullptr;
	if (APawn* Pawn = Cast<APawn>(Actor))
	{
		Movement = Pawn->GetMovementComponent();
	}
	if (bStopMovement && Movement)
	{
		Movement->StopMovementImmediately();
	}

	const bool bSweep = OptionalBool(Params, TEXT("sweep"), false);
	bool bMoved = Actor->TeleportTo(Location, Rotation, /*bIsATest=*/false, /*bNoCheck=*/!bSweep);
	if (!bMoved)
	{
		// TeleportTo refuses when the destination is blocked; fall back to a
		// direct set so a deliberate test placement is not silently ignored.
		bMoved = Actor->SetActorLocationAndRotation(Location, Rotation, /*bSweep=*/false);
	}

	// Stop again after the move: a character that was mid-fall regains velocity
	// during the teleport itself.
	if (bStopMovement && Movement)
	{
		Movement->StopMovementImmediately();
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetStringField(TEXT("netMode"), DescribePIENetMode(World));
	Result->SetBoolField(TEXT("teleported"), bMoved);
	Result->SetBoolField(TEXT("movementStopped"), bStopMovement && Movement != nullptr);
	Result->SetObjectField(TEXT("requestedLocation"), VectorJson(Location));
	// Read the transform back rather than reporting what was asked for.
	Result->SetObjectField(TEXT("actualLocation"), VectorJson(Actor->GetActorLocation()));
	Result->SetObjectField(TEXT("actualRotation"), RotatorJson(Actor->GetActorRotation()));
	if (!Movement)
	{
		Result->SetStringField(TEXT("note"),
			TEXT("Actor has no movement component; nothing would have fought the move."));
	}
	return MCPResult(Result);
}


// #757: set a live character's movement mode and velocity directly.
//
// The generic paths do exist - invoke_function with component=CharMoveComp can
// reach SetMovementMode, and set_component_property can write Velocity - but
// both require the caller to already know the component's name, the enum's
// numeric value, and that MOVE_Custom needs a second argument. That is exactly
// the knowledge the original report had to reconstruct in Python, and getting
// the enum wrong fails silently: the character simply keeps its old mode.
//
// Params:
//   actorLabel (the character), mode? ("walking"|"falling"|"flying"|"swimming"|
//   "none"|"custom"), customMode? (0-255, only with mode=custom),
//   velocity? {x,y,z}, world? (default pie), pieInstance?
TSharedPtr<FJsonValue> FEditorHandlers::SetMovementMode(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = ResolveWorldFromParams(Params, TEXT("pie"));
	if (!World) return MCPError(TEXT("PIE is not running - set_movement_mode targets a live world"));

	FString ActorLabel;
	if (auto Err = RequireString(Params, TEXT("actorLabel"), ActorLabel)) return Err;

	AActor* Actor = FindActorByLabelNameOrPath(World, ActorLabel);
	if (!Actor)
	{
		return MCPError(FString::Printf(TEXT("Actor not found in the live world (by label, name or path): %s"), *ActorLabel));
	}

	UCharacterMovementComponent* Movement = Actor->FindComponentByClass<UCharacterMovementComponent>();
	if (!Movement)
	{
		return MCPError(FString::Printf(
			TEXT("'%s' has no CharacterMovementComponent. Movement modes are a character concept; for other pawns write the movement component's properties with level(set_component_property, world='pie')."),
			*ActorLabel));
	}

	const FString PrevMode = UEnum::GetValueAsString(Movement->MovementMode);
	const uint8 PrevCustom = Movement->CustomMovementMode;
	const FVector PrevVelocity = Movement->Velocity;

	bool bModeChanged = false;
	EMovementMode RequestedMode = MOVE_None;
	const FString ModeStr = OptionalString(Params, TEXT("mode"));
	if (!ModeStr.IsEmpty())
	{
		// Named modes only. Accepting a raw number here would let a caller set a
		// value outside the enum, which reads as success and then behaves as None.
		EMovementMode Mode = MOVE_None;
		const FString Lower = ModeStr.ToLower();
		if      (Lower == TEXT("none"))     Mode = MOVE_None;
		else if (Lower == TEXT("walking"))  Mode = MOVE_Walking;
		else if (Lower == TEXT("navwalking")) Mode = MOVE_NavWalking;
		else if (Lower == TEXT("falling"))  Mode = MOVE_Falling;
		else if (Lower == TEXT("swimming")) Mode = MOVE_Swimming;
		else if (Lower == TEXT("flying"))   Mode = MOVE_Flying;
		else if (Lower == TEXT("custom"))   Mode = MOVE_Custom;
		else
		{
			return MCPError(FString::Printf(
				TEXT("Unknown movement mode '%s'. Expected one of: none, walking, navwalking, falling, swimming, flying, custom."),
				*ModeStr));
		}

		int32 CustomMode = OptionalInt(Params, TEXT("customMode"), 0);
		if (Mode != MOVE_Custom && Params->HasField(TEXT("customMode")))
		{
			return MCPError(TEXT("customMode only applies with mode='custom'; passing it with another mode would be silently ignored."));
		}
		if (CustomMode < 0 || CustomMode > 255)
		{
			return MCPError(TEXT("customMode must be 0-255 (it is a uint8 on CharacterMovementComponent)."));
		}

		Movement->SetMovementMode(Mode, static_cast<uint8>(CustomMode));
		RequestedMode = Mode;
		bModeChanged = true;
	}

	bool bVelocityChanged = false;
	FString Result_VelocityNote;
	if (Params->HasField(TEXT("velocity")))
	{
		// Write through the component, not the actor: the actor has no velocity
		// of its own and CharacterMovement is what integrates this next tick.
		Movement->Velocity = OptionalVec3(Params, TEXT("velocity"));
		bVelocityChanged = true;
		if (Actor->GetLocalRole() != ROLE_Authority)
		{
			// On a simulated proxy or a corrected autonomous client the next
			// replicated move overwrites this, and the response would otherwise
			// report a value that is already gone.
			Result_VelocityNote = TEXT("This actor is not the authority, so the next replicated move will overwrite the velocity written here. Drive it on the server (or use a listen-server PIE instance) for a value that persists.");
		}
	}

	if (!bModeChanged && !bVelocityChanged)
	{
		return MCPError(TEXT("Nothing to do: pass 'mode' and/or 'velocity'."));
	}

	auto Result = MCPSuccess();
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetStringField(TEXT("component"), Movement->GetName());
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetStringField(TEXT("netMode"), DescribePIENetMode(World));
	Result->SetStringField(TEXT("previousMode"), PrevMode);
	Result->SetNumberField(TEXT("previousCustomMode"), PrevCustom);
	Result->SetObjectField(TEXT("previousVelocity"), VectorJson(PrevVelocity));
	// Read back rather than echoing the request. SetMovementMode substitutes
	// MOVE_NavWalking with MOVE_Walking when there is no nav data; that is the
	// only substitution it makes, so this catches that one case honestly
	// instead of implying a broader validation the engine does not do.
	Result->SetStringField(TEXT("mode"), UEnum::GetValueAsString(Movement->MovementMode));
	Result->SetNumberField(TEXT("customMode"), Movement->CustomMovementMode);
	Result->SetObjectField(TEXT("velocity"), VectorJson(Movement->Velocity));
	if (!Result_VelocityNote.IsEmpty()) Result->SetStringField(TEXT("velocityNote"), Result_VelocityNote);
	if (bModeChanged && Movement->MovementMode != RequestedMode)
	{
		Result->SetStringField(TEXT("note"), TEXT("The component substituted a different mode (SetMovementMode falls back from NavWalking to Walking when the world has no navigation data)."));
	}
	// The mode is accepted now but the physics update decides whether it holds:
	// PhysSwimming drops back to Falling outside a water volume on the next
	// tick, and nothing rejects it here. Say so rather than let a same-frame
	// read-back read as confirmation that it stuck.
	if (bModeChanged)
	{
		Result->SetStringField(TEXT("modeNote"),
			TEXT("This is the mode as of this call. CharacterMovement re-evaluates on the next tick and can leave it (e.g. Swimming outside a water volume falls back to Falling) - sample it again after a tick to confirm it held."));
	}
	return MCPResult(Result);
}
