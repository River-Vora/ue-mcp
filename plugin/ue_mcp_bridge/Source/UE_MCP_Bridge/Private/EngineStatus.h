#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/Runnable.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeBool.h"
#include "Dom/JsonObject.h"

class FRunnableThread;

/**
 * A picture of what the engine is doing, kept up to date by the game thread and
 * readable by everyone else.
 *
 * Every other sensor in the bridge routes through FMCPGameThreadExecutor, so
 * the instant the game thread stops returning to the tick loop - a modal
 * dialog, a long FSlowTask, a hitching import - every request degrades to
 * "Handler execution timed out" and the caller learns nothing about why. This
 * class exists to answer that question from outside:
 *
 *   - the game thread refreshes the snapshot from three hooks, one of which
 *     (Slate's pre-tick) keeps firing inside slow tasks and modal loops even
 *     though the core ticker is suspended there;
 *   - the socket thread serves the snapshot directly, never scheduling game
 *     thread work;
 *   - a writer thread flushes it to Saved/UE_MCP_Bridge/status.json so the
 *     state survives a genuinely wedged process and is readable before a
 *     WebSocket client has connected at all.
 *
 * Every field is guarded by a single critical section. Captures are cheap
 * (reading GWarn's scope stack and two compile-manager counters), so the cost
 * of running one per Slate tick is noise next to the frame itself.
 */
class FMCPEngineStatus : public FRunnable
{
public:
	static FMCPEngineStatus& Get();

	/** Install the game-thread hooks and start the status writer thread. */
	void Install();

	/** Remove hooks and stop the writer thread. Safe to call twice. */
	void Shutdown();

	/** Thread-safe. Never touches the game thread. */
	TSharedPtr<FJsonObject> Snapshot() const;

	/** Coarse lifecycle label ("starting", "modules loaded", "ready"). */
	void SetPhase(const FString& InPhase);

	/** Called by the socket thread around a dispatched request. */
	void NoteHandlerBegin(const FString& Method);
	void NoteHandlerEnd(const FString& Method);

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	struct FSlowTaskEntry
	{
		FString Name;
		float Fraction = 0.0f;
	};

	/** Reads live engine state. Game thread only. */
	void CaptureOnGameThread();

	/** Serialise under the lock, then write atomically (temp file + move). */
	void FlushToDisk();

	static FString StatusFilePath();

	mutable FCriticalSection Mutex;

	FString Phase = TEXT("starting");
	double LastCaptureSeconds = 0.0;
	double InstallSeconds = 0.0;

	bool bSlowTaskActive = false;
	FString SlowTaskName;
	float SlowTaskFraction = 0.0f;
	TArray<FSlowTaskEntry> SlowTaskStack;

	bool bModalActive = false;
	FString ModalTitle;
	FString ModalMessage;
	TArray<FString> ModalButtons;

	int32 RemainingShaderJobs = 0;
	int32 RemainingAssetCompiles = 0;

	FString HandlerMethod;
	double HandlerStartSeconds = 0.0;

	FDelegateHandle PreTickHandle;
	FDelegateHandle ModalLoopHandle;
	FTSTicker::FDelegateHandle TickerHandle;

	FRunnableThread* WriterThread = nullptr;
	FThreadSafeBool bStopWriter{false};
	bool bInstalled = false;
};
