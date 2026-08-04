#include "EngineStatus.h"
#include "UE_MCP_BridgeModule.h"
#include "Handlers/DialogHandlers.h"

#include "CoreGlobals.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FeedbackContext.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SlowTask.h"
#include "Misc/SlowTaskStack.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Framework/Application/SlateApplication.h"

#include "AssetCompilingManager.h"
#include "ShaderCompiler.h"

namespace
{
	constexpr float StatusFlushIntervalSeconds = 0.25f;

	/** A slow task's most specific label: this frame's message, else the scope's. */
	FString SlowTaskLabel(const FSlowTask& Task)
	{
		const FString FrameMessage = Task.FrameMessage.ToString();
		return FrameMessage.IsEmpty() ? Task.DefaultMessage.ToString() : FrameMessage;
	}
}

FMCPEngineStatus& FMCPEngineStatus::Get()
{
	static FMCPEngineStatus Instance;
	return Instance;
}

void FMCPEngineStatus::Install()
{
	if (bInstalled)
	{
		return;
	}
	bInstalled = true;
	InstallSeconds = FPlatformTime::Seconds();
	LastCaptureSeconds = InstallSeconds;

	// Three capture hooks, because no single one survives every kind of block:
	//
	//  - the core ticker covers ordinary frames, and is the only one that runs
	//    before Slate exists;
	//  - Slate's pre-tick keeps firing while a FSlowTask pumps the UI to draw
	//    its progress bar, which is exactly the window where the core ticker is
	//    suspended and every bridge request times out;
	//  - the modal loop tick fires while a dialog owns the main loop, so the
	//    snapshot can name the dialog that is blocking the caller.
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool
		{
			CaptureOnGameThread();
			return true;
		}), 0.0f);

	if (FSlateApplication::IsInitialized())
	{
		PreTickHandle = FSlateApplication::Get().OnPreTick().AddLambda([this](float)
		{
			CaptureOnGameThread();
		});
		ModalLoopHandle = FSlateApplication::Get().GetOnModalLoopTickEvent().AddLambda([this](float)
		{
			CaptureOnGameThread();
		});
	}

	bStopWriter = false;
	WriterThread = FRunnableThread::Create(this, TEXT("MCPEngineStatusWriter"), 0, TPri_BelowNormal);

	UE_LOG(LogMCPBridge, Log, TEXT("[UE-MCP] Engine status snapshot installed -> %s"), *StatusFilePath());
}

void FMCPEngineStatus::Shutdown()
{
	if (!bInstalled)
	{
		return;
	}
	bInstalled = false;

	bStopWriter = true;
	if (WriterThread)
	{
		WriterThread->Kill(true);
		delete WriterThread;
		WriterThread = nullptr;
	}

	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (FSlateApplication::IsInitialized())
	{
		if (PreTickHandle.IsValid())
		{
			FSlateApplication::Get().OnPreTick().Remove(PreTickHandle);
			PreTickHandle.Reset();
		}
		if (ModalLoopHandle.IsValid())
		{
			FSlateApplication::Get().GetOnModalLoopTickEvent().Remove(ModalLoopHandle);
			ModalLoopHandle.Reset();
		}
	}

	// Leave no stale file behind: a status.json from a dead editor claiming a
	// 40% slow task is worse than no file at all.
	IFileManager::Get().Delete(*StatusFilePath(), false, false, true);
}

void FMCPEngineStatus::SetPhase(const FString& InPhase)
{
	FScopeLock Lock(&Mutex);
	Phase = InPhase;
}

void FMCPEngineStatus::NoteHandlerBegin(const FString& Method)
{
	FScopeLock Lock(&Mutex);
	HandlerMethod = Method;
	HandlerStartSeconds = FPlatformTime::Seconds();
}

void FMCPEngineStatus::NoteHandlerEnd(const FString& Method)
{
	FScopeLock Lock(&Mutex);
	// Requests are dispatched one at a time per connection, but a second
	// connection can overlap; only clear the entry that is actually ours.
	if (HandlerMethod == Method)
	{
		HandlerMethod.Empty();
		HandlerStartSeconds = 0.0;
	}
}

void FMCPEngineStatus::CaptureOnGameThread()
{
	if (!IsInGameThread())
	{
		return;
	}

	// Read everything first, then take the lock once. Slate widget traversal
	// for a modal dialog is the expensive part and must not be done under a
	// lock the socket thread is waiting on.
	FString LocalSlowName;
	float LocalSlowFraction = 0.0f;
	TArray<FSlowTaskEntry> LocalStack;
	bool bLocalSlowActive = false;

	if (GWarn)
	{
		// The scope stack is what the editor's own progress dialog renders, so
		// this is literally the percentage the user is looking at.
		const FSlowTaskStack& Stack = GWarn->GetScopeStack();
		for (int32 Index = 0; Index < Stack.Num(); ++Index)
		{
			const FSlowTask* Task = Stack[Index];
			if (!Task)
			{
				continue;
			}
			FSlowTaskEntry Entry;
			Entry.Name = SlowTaskLabel(*Task);
			Entry.Fraction = Stack.GetProgressFraction(Index);
			LocalStack.Add(MoveTemp(Entry));
		}
		if (LocalStack.Num() > 0)
		{
			bLocalSlowActive = true;
			LocalSlowFraction = LocalStack[0].Fraction;
			// Report the innermost scope that actually has a label: outer
			// scopes are often unnamed wrappers.
			for (int32 Index = LocalStack.Num() - 1; Index >= 0; --Index)
			{
				if (!LocalStack[Index].Name.IsEmpty())
				{
					LocalSlowName = LocalStack[Index].Name;
					break;
				}
			}
		}
	}

	FString LocalModalTitle;
	FString LocalModalMessage;
	TArray<FString> LocalModalButtons;
	const bool bLocalModal = FDialogHandlers::DescribeActiveModal(LocalModalTitle, LocalModalMessage, LocalModalButtons);

	int32 LocalShaderJobs = 0;
	if (GShaderCompilingManager)
	{
		LocalShaderJobs = GShaderCompilingManager->GetNumRemainingJobs();
	}
	const int32 LocalAssetCompiles = FAssetCompilingManager::Get().GetNumRemainingAssets();

	const double Now = FPlatformTime::Seconds();

	{
		FScopeLock Lock(&Mutex);
		LastCaptureSeconds = Now;
		bSlowTaskActive = bLocalSlowActive;
		SlowTaskName = MoveTemp(LocalSlowName);
		SlowTaskFraction = LocalSlowFraction;
		SlowTaskStack = MoveTemp(LocalStack);
		bModalActive = bLocalModal;
		ModalTitle = MoveTemp(LocalModalTitle);
		ModalMessage = MoveTemp(LocalModalMessage);
		ModalButtons = MoveTemp(LocalModalButtons);
		RemainingShaderJobs = LocalShaderJobs;
		RemainingAssetCompiles = LocalAssetCompiles;
	}
}

TSharedPtr<FJsonObject> FMCPEngineStatus::Snapshot() const
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	const double Now = FPlatformTime::Seconds();

	FScopeLock Lock(&Mutex);

	Out->SetStringField(TEXT("phase"), Phase);
	Out->SetNumberField(TEXT("uptimeSeconds"), Now - InstallSeconds);

	// The single most useful number here: how long the game thread has gone
	// without reaching any of the capture hooks. Everything below is as old as
	// this value says it is.
	Out->SetNumberField(TEXT("gameThreadStalledSeconds"), Now - LastCaptureSeconds);

	if (bSlowTaskActive)
	{
		TSharedPtr<FJsonObject> SlowTask = MakeShared<FJsonObject>();
		SlowTask->SetStringField(TEXT("name"), SlowTaskName);
		SlowTask->SetNumberField(TEXT("fraction"), SlowTaskFraction);

		TArray<TSharedPtr<FJsonValue>> StackJson;
		for (const FSlowTaskEntry& Entry : SlowTaskStack)
		{
			TSharedPtr<FJsonObject> EntryJson = MakeShared<FJsonObject>();
			EntryJson->SetStringField(TEXT("name"), Entry.Name);
			EntryJson->SetNumberField(TEXT("fraction"), Entry.Fraction);
			StackJson.Add(MakeShared<FJsonValueObject>(EntryJson));
		}
		SlowTask->SetArrayField(TEXT("stack"), StackJson);
		Out->SetObjectField(TEXT("slowTask"), SlowTask);
	}
	else
	{
		Out->SetField(TEXT("slowTask"), MakeShared<FJsonValueNull>());
	}

	if (bModalActive)
	{
		TSharedPtr<FJsonObject> Modal = MakeShared<FJsonObject>();
		Modal->SetStringField(TEXT("title"), ModalTitle);
		Modal->SetStringField(TEXT("message"), ModalMessage);
		TArray<TSharedPtr<FJsonValue>> ButtonsJson;
		for (const FString& Button : ModalButtons)
		{
			ButtonsJson.Add(MakeShared<FJsonValueString>(Button));
		}
		Modal->SetArrayField(TEXT("buttons"), ButtonsJson);
		Out->SetObjectField(TEXT("modal"), Modal);
	}
	else
	{
		Out->SetField(TEXT("modal"), MakeShared<FJsonValueNull>());
	}

	TSharedPtr<FJsonObject> Compiling = MakeShared<FJsonObject>();
	Compiling->SetNumberField(TEXT("shaders"), RemainingShaderJobs);
	Compiling->SetNumberField(TEXT("assets"), RemainingAssetCompiles);
	Out->SetObjectField(TEXT("compiling"), Compiling);

	if (!HandlerMethod.IsEmpty())
	{
		TSharedPtr<FJsonObject> Handler = MakeShared<FJsonObject>();
		Handler->SetStringField(TEXT("method"), HandlerMethod);
		Handler->SetNumberField(TEXT("elapsedSeconds"), Now - HandlerStartSeconds);
		Out->SetObjectField(TEXT("handler"), Handler);
	}
	else
	{
		Out->SetField(TEXT("handler"), MakeShared<FJsonValueNull>());
	}

	return Out;
}

FString FMCPEngineStatus::StatusFilePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UE_MCP_Bridge"), TEXT("status.json"));
}

void FMCPEngineStatus::FlushToDisk()
{
	TSharedPtr<FJsonObject> Json = Snapshot();
	Json->SetStringField(TEXT("writtenAt"), FDateTime::UtcNow().ToIso8601());

	FString Serialised;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialised);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	// Write then move: a reader polling at the same rate would otherwise catch
	// a half-written file and report "no snapshot" at the exact moment the
	// snapshot matters.
	const FString Final = StatusFilePath();
	const FString Temp = Final + TEXT(".tmp");
	if (FFileHelper::SaveStringToFile(Serialised, *Temp))
	{
		IFileManager::Get().Move(*Final, *Temp, true, true);
	}
}

uint32 FMCPEngineStatus::Run()
{
	// Deliberately its own thread: the whole point is to keep publishing while
	// the game thread is inside a dialog, a slow task, or a hang.
	while (!bStopWriter)
	{
		FlushToDisk();
		FPlatformProcess::Sleep(StatusFlushIntervalSeconds);
	}
	return 0;
}

void FMCPEngineStatus::Stop()
{
	bStopWriter = true;
}
