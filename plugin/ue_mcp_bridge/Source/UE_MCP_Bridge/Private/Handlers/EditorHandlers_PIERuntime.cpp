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
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/UObjectIterator.h"
#include "UObject/GCObjectScopeGuard.h"
#include "Misc/ScopeExit.h"

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

		// ProcessEvent can run arbitrary game code - including one that tears
		// down the world and collects garbage - and the target is read again
		// below to export out params.
		FGCObjectScopeGuard TargetGuard(CallTarget);
		// ParamBuf is raw bytes, invisible to GC. Any UObject* out-param written
		// into it during the call would dangle if ProcessEvent collected
		// garbage, and it is dereferenced below to export return values.
		TArray<FGCObjectScopeGuard*> ArgGuards;
		ON_SCOPE_EXIT { for (FGCObjectScopeGuard* G : ArgGuards) delete G; };
		CallTarget->ProcessEvent(Func, ParamBuf.GetData());
		for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			if (FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(*It))
			{
				if (UObject* Out = OP->GetObjectPropertyValue(OP->ContainerPtrToValuePtr<void>(ParamBuf.GetData())))
				{
					ArgGuards.Add(new FGCObjectScopeGuard(Out));
				}
			}
		}

		TSharedPtr<FJsonObject> OutVals = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
		{
			FProperty* P = *It;
			if (P->PropertyFlags & (CPF_ReturnParm | CPF_OutParm))
			{
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
		if (Count >= MaxProperties) { ++Skipped; continue; }
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
	// not been posed. A posed mesh - including a leader-pose-driven one, which
	// a flag check would have wrongly rejected - exits on the first
	// non-identity bone and costs nothing.
	if (!Mesh->GetSkinnedAsset() || Mesh->GetNumComponentSpaceTransforms() == 0)
	{
		return MCPError(FString::Printf(
			TEXT("SkeletalMeshComponent '%s' on '%s' has no bone transform data (no skinned asset, or not registered)."),
			*Mesh->GetName(), *ActorLabel));
	}
	{
		const TArray<FTransform>& Spaces = Mesh->GetComponentSpaceTransforms();
		bool bAnyPosed = Spaces.Num() <= 1;
		const int32 Probe = FMath::Min(Spaces.Num(), 32);
		for (int32 i = 0; i < Probe && !bAnyPosed; ++i)
		{
			if (!Spaces[i].Equals(FTransform::Identity, UE_KINDA_SMALL_NUMBER)) bAnyPosed = true;
		}
		if (!bAnyPosed)
		{
			return MCPError(FString::Printf(
				TEXT("SkeletalMeshComponent '%s' on '%s' has not evaluated its bone transforms yet - every bone reads as identity, which would be reported as real measurement data. Is PIE running, and has the mesh ticked?"),
				*Mesh->GetName(), *ActorLabel));
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
