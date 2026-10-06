// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "Atomic.h"
#include "CellTransitioner.h"
#include "IdsAndOffsets.h"
#include "LoadingProgress.h"
#include "ProgressMeter.h"
#include "Settings.h"

#include <hde64.h>
#include <xbyak/xbyak.h>

namespace load_progress
{
    /**
     * @brief Returns the singleton that owns loading aggregation and event handling.
     */
    LoadingProgress& LoadingProgress::GetSingleton()
    {
        static LoadingProgress singleton;
        return singleton;
    }

    /**
     * @brief Sums the directly observed work that is still queued across all pools.
     */
    std::uint64_t LoadingProgress::GetLiveRemaining()
    {
        std::uint64_t remaining = 0;
        for (const auto& queue : liveRemaining) {
            remaining += Atomic::get(queue, std::memory_order_relaxed);
        }
        return remaining;
    }

    /**
     * @brief Returns elapsed steady-clock time in milliseconds for progress diagnostics.
     */
    std::uint64_t LoadingProgress::MonotonicMilliseconds() noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }

    /**
     * @brief Applies queue deltas outside the engine's queue-mutation call paths.
     */
    void LoadingProgress::DrainQueueMutations()
    {
        if (!Atomic::get(epochActive)) {
            return;
        }

        std::scoped_lock lock(stateLock);
        for (std::size_t i = 0; i < queueCount; ++i) {
            // Workers update atomic deltas without taking the UI state lock. Exchanging with zero
            // claims this batch of deltas; increments arriving afterward belong to the next drain.
            const auto enqueued = Atomic::get_and_set(pendingEnqueued[i], 0);
            const auto completed = Atomic::get_and_set(pendingCompleted[i], 0);
            const auto queue = static_cast<Queue>(i);

            if (enqueued != 0) {
                aggregator.Enqueue(queue, enqueued);
            }

            if (completed != 0) {
                aggregator.Complete(queue, completed);
            }
        }
        UpdateDisplayedProgress(aggregator.Current());
    }

    /**
     * @brief Resolves captured numeric IDs on the loading-menu thread and releases their ring slots.
     */
    void LoadingProgress::DrainLoadedEntries(bool a_writeLog)
    {
        static constexpr auto typeNames = std::to_array<std::string_view>(
            { "object-reference", "transferred-reference", "distant-reference" });

        for (auto& slot : loadedEntries) {
            if (Atomic::get(slot.state) != 2) {
                continue;
            }

            const auto entry = slot.entry;
            // Release returns ownership only after this thread has copied the published record.
            Atomic::set(slot.state, 0);

            if (!a_writeLog) {
                continue;
            }

            const auto* reference = RE::TESForm::LookupByID(entry.referenceID);
            const auto* base = RE::TESForm::LookupByID(entry.baseID);
            const auto* cell = RE::TESForm::LookupByID(entry.cellID);
            const auto* referenceEditorID = reference ? reference->GetFormEditorID() : nullptr;
            const auto* baseEditorID = base ? base->GetFormEditorID() : nullptr;
            const auto* cellEditorID = cell ? cell->GetFormEditorID() : nullptr;
            const auto  typeIndex = static_cast<std::size_t>(entry.type);
            const auto  typeName = typeIndex < typeNames.size() ? typeNames[typeIndex] : "unknown";

            logger::info(
                "loaded entry: type={} reference={:08X} editorID='{}' base={:08X} editorID='{}' cell={:08X} editorID='{}'",
                typeName, entry.referenceID, referenceEditorID ? referenceEditorID : "", entry.baseID,
                baseEditorID ? baseEditorID : "", entry.cellID, cellEditorID ? cellEditorID : "");
        }
    }

    /**
     * @brief Clears stale details and enables the lock-free producer path for a new Loading Menu lifetime.
     */
    void LoadingProgress::BeginLoadedEntryCapture()
    {
        const auto& settings = Settings::GetSingleton();
        if (!settings.IsLoadedEntryLoggingEnabled() ||
            Atomic::get(loadedEntryCaptureActive)) {
            return;
        }

        // Capture is still disabled, so no new producer can enter while stale slots are reclaimed.
        WaitForLoadedEntryWriters();

        DrainLoadedEntries(false);
        for (auto& tally : loadedEntryTallies) {
            Atomic::set(tally, 0, std::memory_order_relaxed);
        }
        Atomic::set(loadedEntryWriteCursor, 0, std::memory_order_relaxed);
        Atomic::set(droppedLoadedEntryDetails, 0, std::memory_order_relaxed);
        Atomic::set(loadedEntryCaptureActive, true);
    }

    /**
     * @brief Stops producers, drains their final details, and writes exact per-category enqueue totals.
     */
    void LoadingProgress::EndLoadedEntryCapture()
    {
        if (!Atomic::get_and_clear(loadedEntryCaptureActive)) {
            return;
        }

        WaitForLoadedEntryWriters();

        DrainLoadedEntries(true);
        logger::info(
            "loaded-entry tally: object-references={} transferred-references={} distant-references={} dropped-details={}",
            Atomic::get(loadedEntryTallies[static_cast<std::size_t>(LoadedEntryType::objectReference)],
                std::memory_order_relaxed),
            Atomic::get(loadedEntryTallies[static_cast<std::size_t>(LoadedEntryType::transferredReference)],
                std::memory_order_relaxed),
            Atomic::get(loadedEntryTallies[static_cast<std::size_t>(LoadedEntryType::distantReference)],
                std::memory_order_relaxed),
            Atomic::get(droppedLoadedEntryDetails, std::memory_order_relaxed));
    }

    /**
     * @brief Waits only after capture has been disabled. Producers never wait on the loading-menu thread.
     */
    void LoadingProgress::WaitForLoadedEntryWriters()
    {
        while (Atomic::get(loadedEntryWriters) != 0) {
            std::this_thread::yield();
        }
    }

    /**
     * @brief Converts the entry type into its independently configurable diagnostic switch.
     */
    bool LoadingProgress::IsLoadedEntryTypeEnabled(LoadedEntryType a_type) noexcept
    {
        const auto& categories = Settings::GetSingleton().GetLoadedEntryLogging();

        switch (a_type) {
        case LoadedEntryType::objectReference:
            return categories.objectReferences;
        case LoadedEntryType::transferredReference:
            return categories.transferredReferences;
        case LoadedEntryType::distantReference:
            return categories.distantReferences;
        default:
            return false;
        }
    }

    /**
     * @brief Reserves one ring slot and publishes the completed record to the loading-menu consumer.
     */
    bool LoadingProgress::TryStoreLoadedEntry(const LoadedEntry& a_entry) noexcept
    {
        // A few retries avoid dropping detail when the cursor meets a slot the consumer has not
        // released yet. The separate tallies remain exact even when this detail buffer is saturated.
        constexpr std::size_t reservationAttempts = 8;

        for (std::size_t attempt = 0; attempt < reservationAttempts; ++attempt) {
            const auto index = Atomic::get_and_add(loadedEntryWriteCursor, 1, std::memory_order_relaxed) % loadedEntryCapacity;
            auto       expectedState = std::uint8_t{ 0 };
            auto&      slot = loadedEntries[index];

            // State 0 is free, state 1 belongs to a producer, and state 2 is ready for the consumer.
            // The acquire/release pair ensures the consumer cannot see a partially written entry.
            if (!Atomic::compare_and_set(slot.state,
                    expectedState, std::uint8_t{ 1 }, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                continue;
            }

            // The slot is already claimed as state 1. Publish its payload with a release write
            // to state 2; the consumer's acquire read then makes the complete record visible.
            slot.entry = a_entry;
            Atomic::set(slot.state, 2);
            return true;
        }

        return false;
    }

    /**
     * @brief Copies only stable numeric identifiers from a loading worker into the diagnostic ring.
     */
    void LoadingProgress::CaptureLoadedEntry(
        LoadedEntryType a_type, RE::TESObjectREFR* a_reference, RE::TESObjectCELL* a_cell) noexcept
    {
        const auto typeIndex = static_cast<std::size_t>(a_type);
        if (!a_reference || !a_cell || typeIndex >= loadedEntryTypeCount ||
            !IsLoadedEntryTypeEnabled(a_type) ||
            !Atomic::get(loadedEntryCaptureActive)) {
            return;
        }

        // Count active producers so EndLoadedEntryCapture cannot reclaim a slot mid-write.
        // Register as an in-flight producer, then recheck the capture gate. Shutdown can close
        // the gate between the first check and registration; that second check lets this writer
        // withdraw without publishing while shutdown waits for registered writers to finish.
        Atomic::get_and_add(loadedEntryWriters, 1);
        if (!Atomic::get(loadedEntryCaptureActive)) {

            Atomic::get_and_subtract(loadedEntryWriters, 1, std::memory_order_release);
            return;
        }

        // Do not resolve Editor IDs here. This callback can run on an engine loading worker, where
        // form lookups and the logger are unsafe. DrainLoadedEntries performs that work later.
        const auto* base = a_reference->GetBaseObject();
        const LoadedEntry entry{
            a_type, a_reference->GetFormID(), base ? base->GetFormID() : 0, a_cell->GetFormID()
        };
        Atomic::get_and_add(loadedEntryTallies[typeIndex], 1, std::memory_order_relaxed);

        if (!TryStoreLoadedEntry(entry)) {
            Atomic::get_and_add(droppedLoadedEntryDetails, 1, std::memory_order_relaxed);
        }

        Atomic::get_and_subtract(loadedEntryWriters, 1, std::memory_order_release);
    }

    /**
     * @brief The ordinary object path keeps the reference in RDI and its cell in RCX at the enqueue call.
     */
    void LoadingProgress::ObjectReferenceQueued(CONTEXT& a_context) noexcept
    {
        CaptureLoadedEntry(LoadedEntryType::objectReference,
            reinterpret_cast<RE::TESObjectREFR*>(a_context.Rdi),
            reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx));
        // The overwritten CALL would return its result in RAX. Store it in the saved CPU
        // context so register restoration presents that same return value to Skyrim.
        // The CONTEXT describes this particular decoded call site, not a general C++ argument list.
        if (originalReferenceEnqueue) {
            a_context.Rax = originalReferenceEnqueue(reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx));
        }
    }

    /**
     * @brief The transfer path keeps the moved reference in RBX and its destination cell in RCX.
     */
    void LoadingProgress::TransferredReferenceQueued(CONTEXT& a_context) noexcept
    {
        CaptureLoadedEntry(LoadedEntryType::transferredReference,
            reinterpret_cast<RE::TESObjectREFR*>(a_context.Rbx),
            reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx));
        // Object and transfer paths share this callee, but preserve the reference in different
        // registers at their call sites. Capture above distinguishes their diagnostic categories;
        // invoking the common original helper preserves the actual enqueue operation.
        if (originalReferenceEnqueue) {
            a_context.Rax = originalReferenceEnqueue(reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx));
        }
    }

    /**
     * @brief The distant path passes the reference to its counter helper in RDX and the cell in RCX.
     */
    void LoadingProgress::DistantReferenceQueued(CONTEXT& a_context) noexcept
    {
        CaptureLoadedEntry(LoadedEntryType::distantReference,
            reinterpret_cast<RE::TESObjectREFR*>(a_context.Rdx),
            reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx));
        // This helper takes two native arguments: RCX supplies the cell and RDX the reference.
        // Use the captured values to reproduce the replaced call, then propagate its RAX result.
        if (originalDistantReferenceEnqueue) {
            a_context.Rax = originalDistantReferenceEnqueue(
                reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx),
                reinterpret_cast<RE::TESObjectREFR*>(a_context.Rdx));
        }
    }

    /**
     * @brief Updates the progress widget and hides Scaleform for warm transitions.
     */
    void LoadingProgress::LoadingMenuAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
    {
        if (originalAdvanceMovie) {
            originalAdvanceMovie(a_menu, a_interval, a_currentTime);
        }

        if (!Atomic::get(hooksEnabled)) {
            return;
        }

        try {
            DrainQueueMutations();
            if (Atomic::get(loadedEntryCaptureActive)) {
                DrainLoadedEntries(true);
            }

            CellTransitioner::HideHUDForLoad();

            if (Settings::GetSingleton().IsVerboseQueueLoggingEnabled()) {
                LogProgressTrace(aggregator.Current(), a_interval);
            }

            if (a_menu && a_menu->uiMovie) {

                const bool seamless = CellTransitioner::IsSeamless();
                const bool vanilla = CellTransitioner::IsVanilla();
                const auto& progressBar = Settings::GetSingleton().GetProgressBar();
                if (!vanilla) {

                    a_menu->uiMovie->SetBackgroundAlpha(0.0F);
                    if (!seamless) {
                        CellTransitioner::ApplyLoadingMenuFade(a_menu, a_interval);
                    }

                    a_menu->uiMovie->SetVisible(!seamless);
                }

                const bool showProgress =
                    progressBar.mode != Settings::ProgressBar::Mode::disabled &&
                    (!vanilla || progressBar.mode == Settings::ProgressBar::Mode::all);
                ProgressMeter::GetSingleton().SetVisible(a_menu, showProgress);
                if (!seamless && showProgress) {

                    const auto basisPoints = Atomic::get(displayedBasisPoints);
                    ProgressMeter::GetSingleton().Update(
                        a_menu, static_cast<double>(basisPoints) / 100.0, a_interval);
                }
            }
        } catch (const std::exception& error) {
            DisableHooks(error.what());
        } catch (...) {
            DisableHooks("unknown exception in LoadingMenu::AdvanceMovie");
        }
    }

    /**
     * @brief Locks the source frame and configures the selected presentation when the menu opens.
     */
    RE::UI_MESSAGE_RESULTS LoadingProgress::LoadingMenuProcessMessage(RE::IMenu* a_menu, RE::UIMessage& a_message)
    {
        if (Atomic::get(hooksEnabled) &&
            a_message.type == RE::UI_MESSAGE_TYPE::kShow) {

            try {
                BeginLoadedEntryCapture();
                CellTransitioner::PrepareForLoad(a_menu);
            } catch (const std::exception& error) {
                DisableHooks(error.what());
            } catch (...) {
                DisableHooks("unknown exception in LoadingMenu::ProcessMessage");
            }
        }

        return originalLoadingProcessMessage ?
                   originalLoadingProcessMessage(a_menu, a_message) :
                   RE::UI_MESSAGE_RESULTS::kPassOn;
    }

    /**
     * @brief Advances displayed progress monotonically, with an initial ramp and a 99 percent loading
     * ceiling.
     */
    void LoadingProgress::UpdateDisplayedProgress(const Progress& a_progress)
    {
        constexpr std::uint32_t finalLoadingBasisPoints = 9900;
        constexpr std::uint64_t initialRampMilliseconds = 500;
        const auto rawCandidate = a_progress.total ?
                                      static_cast<std::uint32_t>(
                                          std::clamp(a_progress.fraction * 10000.0, 0.0, 10000.0)) :
                                      0u;
        const auto now = MonotonicMilliseconds();
        const auto epochStarted = Atomic::get(epochStartedMs, std::memory_order_relaxed);
        const auto elapsed = epochStarted && now >= epochStarted ? now - epochStarted : 0;
        const auto rampCeiling = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            finalLoadingBasisPoints,
            elapsed * finalLoadingBasisPoints / initialRampMilliseconds));
        const auto candidate = std::min({ rawCandidate, rampCeiling, finalLoadingBasisPoints });
        auto       displayed = Atomic::get(displayedBasisPoints, std::memory_order_relaxed);
        // New work can appear after a queue temporarily drains. The initial time ceiling prevents an
        // early completed batch from committing a misleading high value, and LoadingMenu owns the
        // final one percent so the meter cannot report completion while the menu is still visible.
        bool advanced = false;
        while (candidate > displayed) {
            if (Atomic::compare_and_set_weak(displayedBasisPoints,
                    displayed, candidate, std::memory_order_release, std::memory_order_relaxed)) {

                advanced = true;
                break;
            }
        }
        if (advanced && Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            Atomic::set(traceLastProgressAdvanceMs, MonotonicMilliseconds(), std::memory_order_relaxed);
        }

        if (!Settings::GetSingleton().IsVerboseQueueLoggingEnabled()) {
            return;
        }

        if (a_progress.total == lastLogged.total && a_progress.completed == lastLogged.completed &&
            a_progress.remaining == lastLogged.remaining) {
            return;
        }

        if (Settings::GetSingleton().IsVerboseQueueLoggingEnabled()) {
            logger::info("load progress completed={} remaining={} total={} percent={:.1f}",
                a_progress.completed, a_progress.remaining, a_progress.total,
                a_progress.fraction * 100.0);
        }

        lastLogged = a_progress;
    }

    /**
     * @brief Writes a low-frequency timeline sample even when no queue mutation changes the meter. This
     * distinguishes untracked work from a stalled LoadingMenu update callback in test logs.
     */
    void LoadingProgress::LogProgressTrace(const Progress& a_progress, float a_interval)
    {
        if (!Settings::GetSingleton().IsVerboseQueueLoggingEnabled() ||
            !Atomic::get(epochActive)) {
            return;
        }

        constexpr std::uint64_t samplePeriodMs = 250;
        const auto now = MonotonicMilliseconds();
        const auto previousSample = Atomic::get(traceLastSampleMs, std::memory_order_relaxed);
        if (previousSample != 0 && now - previousSample < samplePeriodMs) {
            return;
        }

        Atomic::set(traceLastSampleMs, now, std::memory_order_relaxed);

        const auto epochStarted = Atomic::get(epochStartedMs, std::memory_order_relaxed);
        const auto queueActivity = Atomic::get(traceLastQueueActivityMs, std::memory_order_relaxed);
        const auto progressAdvance = Atomic::get(traceLastProgressAdvanceMs, std::memory_order_relaxed);
        const auto displayed = Atomic::get(displayedBasisPoints);
        const auto rawBasisPoints = a_progress.total ?
                                        static_cast<std::uint32_t>(
                                            std::clamp(a_progress.fraction * 10000.0, 0.0, 10000.0)) :
                                        0u;

        std::array<std::uint64_t, queueCount> live{};
        for (std::size_t i = 0; i < queueCount; ++i) {
            live[i] = Atomic::get(liveRemaining[i], std::memory_order_relaxed);
        }

        logger::info(
            "progress trace: epoch_ms={} sample_delta_ms={} frame_interval_ms={:.3f} displayed={:.2f}% raw={:.2f}% completed={} remaining={} total={} progress_idle_ms={} queue_idle_ms={} live=[critical-refs:{}, refs:{}, distant-refs:{}, background:{}, tasks:{}, post-processing:{}]",
            now - epochStarted, previousSample ? now - previousSample : 0, a_interval * 1000.0F,
            static_cast<double>(displayed) / 100.0, static_cast<double>(rawBasisPoints) / 100.0,
            a_progress.completed, a_progress.remaining, a_progress.total, now - progressAdvance,
            now - queueActivity, live[0], live[1], live[2], live[3], live[4], live[5]);
    }

    /**
     * @brief Disables plugin behavior while leaving every installed hook as a pass-through. Coordinates
     * with the transition compositor so a progress-side failure cannot leave a suppressed FaderMenu
     * or retained-frame compositor owning the screen.
     */
    void LoadingProgress::DisableHooks(std::string_view a_reason) noexcept
    {
        DisablePlugin(a_reason);
    }

    /**
     * @brief Disables progress and transition behavior and restores suppressed presentation state.
     */
    void DisablePlugin(std::string_view a_reason) noexcept
    {
        static std::atomic_bool disabling{ false };
        if (Atomic::get_and_set(disabling, true)) {
            return;
        }

        Atomic::set(LoadingProgress::hooksEnabled, false);
        Atomic::set(LoadingProgress::epochActive, false);
        Atomic::set(LoadingProgress::loadedEntryCaptureActive, false);

        CellTransitioner::ResetOnDisable();

        const bool loggedProgress =
            Atomic::get_and_set(LoadingProgress::failureLogged, true);
        const bool loggedTransition =
            Atomic::get_and_set(CellTransitioner::failureLogged, true);
        if (!loggedProgress && !loggedTransition) {

            try {
                logger::critical("Skyrim Load Progress disabled: {}", a_reason);
            } catch (...) {
                REX::W32::OutputDebugStringA("Skyrim Load Progress: plugin disabled\n");
            }
        }
    }

    /**
     * @brief Records a direct queue increment without locking or logging on the engine worker thread.
     */
    void LoadingProgress::OnEnqueue(Queue a_queue) noexcept
    {
        const auto index = static_cast<std::size_t>(a_queue);
        if (index >= queueCount || !Atomic::get(hooksEnabled, std::memory_order_relaxed)) {
            return;
        }

        Atomic::get_and_add(liveRemaining[index], 1, std::memory_order_relaxed);
        if (Atomic::get(epochActive, std::memory_order_relaxed)) {

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                Atomic::set(traceLastQueueActivityMs, MonotonicMilliseconds(), std::memory_order_relaxed);
            }

            Atomic::get_and_add(pendingEnqueued[index], 1, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Records a direct queue decrement without locking or logging on the engine worker thread.
     */
    void LoadingProgress::OnComplete(Queue a_queue) noexcept
    {
        const auto index = static_cast<std::size_t>(a_queue);
        if (index >= queueCount || !Atomic::get(hooksEnabled, std::memory_order_relaxed)) {
            return;
        }

        auto& live = liveRemaining[index];
        auto  value = Atomic::get(live, std::memory_order_relaxed);
        // Saturate at zero because the plugin may begin observing after Skyrim queued the work.
        while (value != 0 && !Atomic::compare_and_set_weak(live, value, value - 1, std::memory_order_relaxed)) {}
        if (Atomic::get(epochActive, std::memory_order_relaxed)) {

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                Atomic::set(traceLastQueueActivityMs, MonotonicMilliseconds(), std::memory_order_relaxed);
            }

            Atomic::get_and_add(pendingCompleted[index], 1, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Records a critical-reference queue increment from an engine context hook.
     */
    void LoadingProgress::CriticalEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::criticalReferences); }
    /**
     * @brief Records a critical-reference queue decrement from an engine context hook.
     */
    void LoadingProgress::CriticalComplete(CONTEXT&) noexcept { OnComplete(Queue::criticalReferences); }
    /**
     * @brief Records a reference queue increment from an engine context hook.
     */
    void LoadingProgress::ReferenceEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::references); }
    /**
     * @brief Records a reference queue decrement from an engine context hook.
     */
    void LoadingProgress::ReferenceComplete(CONTEXT&) noexcept { OnComplete(Queue::references); }
    /**
     * @brief Records a distant-reference queue increment from an engine context hook.
     */
    void LoadingProgress::DistantEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::distantReferences); }
    /**
     * @brief Records a distant-reference queue decrement from an engine context hook.
     */
    void LoadingProgress::DistantComplete(CONTEXT&) noexcept { OnComplete(Queue::distantReferences); }
    /**
     * @brief Records a background-processing queue increment from an engine context hook.
     */
    void LoadingProgress::BackgroundEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::backgroundProcessing); }
    /**
     * @brief Records a background-processing queue decrement from an engine context hook.
     */
    void LoadingProgress::BackgroundComplete(CONTEXT&) noexcept { OnComplete(Queue::backgroundProcessing); }
    /**
     * @brief Chains the native enqueue operation and records added IO task work.
     */
    void LoadingProgress::IOTaskEnqueue(RE::IOManager* a_manager) noexcept
    {
        if (originalIOTaskEnqueue) {
            originalIOTaskEnqueue(a_manager);
        }

        OnEnqueue(Queue::tasks);
    }

    /**
     * @brief Chains the native completion operation and records completed IO task work.
     */
    void LoadingProgress::IOTaskComplete(RE::IOManager* a_manager) noexcept
    {
        if (originalIOTaskComplete) {
            originalIOTaskComplete(a_manager);
        }

        OnComplete(Queue::tasks);
    }

    /**
     * @brief Chains the native enqueue operation and records added post-processing work.
     */
    void LoadingProgress::PostProcessingEnqueue(void* a_queue) noexcept
    {
        if (originalPostProcessingEnqueue) {
            originalPostProcessingEnqueue(a_queue);
        }

        // This vtable callback also runs for other queues of the same concrete type. Read
        // IOManager's designated post-processing queue pointer and count only that instance.
        // +0xE0 is the manager's pointer field; +0x14 in the signature is the queue's counter.
        const auto* manager = RE::IOManager::GetSingleton();
        const auto* postProcessing = manager ?
                                         *reinterpret_cast<void* const*>(
                                             reinterpret_cast<std::uintptr_t>(manager) + 0xE0) :
                                         nullptr;
        if (a_queue == postProcessing) {
            OnEnqueue(Queue::postProcessing);
        }
    }

    /**
     * @brief Chains the native completion operation and records completed post-processing work.
     */
    void LoadingProgress::PostProcessingComplete(void* a_queue) noexcept
    {
        if (originalPostProcessingComplete) {
            originalPostProcessingComplete(a_queue);
        }

        // Apply the same instance filter as enqueue after running the original queue method.
        // The hook observes the native mutation; it does not replace Skyrim's queue bookkeeping.
        const auto* manager = RE::IOManager::GetSingleton();
        const auto* postProcessing = manager ?
                                         *reinterpret_cast<void* const*>(
                                             reinterpret_cast<std::uintptr_t>(manager) + 0xE0) :
                                         nullptr;
        if (a_queue == postProcessing) {
            OnComplete(Queue::postProcessing);
        }
    }

    namespace
    {
        using ContextCallback = void (*)(CONTEXT&) noexcept;
        using CounterSignature = std::vector<std::uint8_t>;

        enum class CounterOperation : std::uint8_t
        {
            increment,
            decrement
        };

        struct MutationHookDefinition
        {
            // A definition describes the expected site, not an installed patch: Address Library
            // ID selects the function, RuntimeOffset selects the instruction within it, signature
            // validates that instruction, and callback records the observed queue mutation.
            REL::RelocationID id;
            RuntimeOffset     offset;
            ContextCallback   callback;
            std::string_view  name;
            CounterSignature  signature;
        };

        struct ResolvedMutationHook
        {
            // Resolution turns a definition into a validated address and overwrite length.
            // The pointer borrows the definition array, which remains alive through installation.
            const MutationHookDefinition* definition;
            std::uintptr_t                 address;
            std::size_t                    patchSize;
        };

        struct LoadedEntryHookDefinition
        {
            // These optional diagnostics hook a CALL in its higher-level caller, where registers
            // still identify the reference being queued. Counter mutations alone reveal counts,
            // but cannot distinguish ordinary references from references transferred between cells.
            REL::RelocationID caller;
            REL::RelocationID callee;
            ContextCallback   callback;
            std::string_view  name;
            bool              enabled;
        };

        struct ResolvedLoadedEntryHook
        {
            const LoadedEntryHookDefinition* definition;
            std::uintptr_t                    callSite;
        };

        // Uses Xbyak mnemonics to produce the exact instruction expected in Skyrim's executable.
        // The returned bytes are read-only validation data; CommonLib separately uses Xbyak to build
        // the context trampoline that runs after the copied counter instruction.
        CounterSignature BuildCounterSignature(
            CounterOperation a_operation, const Xbyak::Reg64& a_owner, std::int32_t a_counterOffset)
        {
            Xbyak::CodeGenerator assembler;
            // dword selects a 32-bit memory operand; a_owner is the register holding its object.
            // For example, owner=RAX and offset=0x16C describes DWORD PTR [RAX + 0x16C].
            // This offset is a field within the object, not an offset within executable code.
            const auto           counter = assembler.dword[a_owner + a_counterOffset];

            // LOCK is required because several loading workers can mutate the same counter.
            // Xbyak emits x86-64 machine bytes, including the LOCK prefix, addressing mode,
            // displacement, and INC/DEC opcode. Using the assembler avoids hand-maintaining
            // different encodings when the owner register or displacement size changes.
            assembler.lock();
            if (a_operation == CounterOperation::increment) {
                assembler.inc(counter);
            } else {
                assembler.dec(counter);
            }

            // Finalize the assembler buffer before reading it. Copy the bytes into our vector
            // because the local assembler owns its buffer and will destroy it on return.
            // This signature is an exact byte sequence, not a wildcard pattern or a generated hook.
            assembler.ready();

            const auto* bytes = assembler.getCode();
            return { bytes, bytes + assembler.getSize() };
        }

        // Returns eight mutation sites: enqueue/completion for three reference queues and the background worker.
        auto GetMutationHookDefinitions()
        {
            // Each enqueue/completion pair observes the same engine counter going up/down.
            // The relocation ID and runtime instruction offset locate the patch; the register
            // and counter-field offset below independently describe the bytes expected there.
            // A valid Address Library lookup alone is therefore not enough to authorize a patch.
            using namespace Xbyak::util;

            // RAX and RCX are simply the registers holding the counter owner at these decoded sites.
            // Distant enqueue is the only one in this set whose owner remains in RCX.
            return std::to_array<MutationHookDefinition>({
                { IDs::CriticalReferencesEnqueue, Offsets::CriticalReferencesEnqueue,
                    LoadingProgress::CriticalEnqueue, "critical enqueue",
                    BuildCounterSignature(CounterOperation::increment, rax, 0x16C) },
                { IDs::CriticalReferencesComplete, Offsets::CriticalReferencesComplete,
                    LoadingProgress::CriticalComplete, "critical completion",
                    BuildCounterSignature(CounterOperation::decrement, rax, 0x16C) },
                { IDs::ReferencesEnqueue, Offsets::ReferencesEnqueue,
                    LoadingProgress::ReferenceEnqueue, "reference enqueue",
                    BuildCounterSignature(CounterOperation::increment, rax, 0x170) },
                { IDs::ReferencesComplete, Offsets::ReferencesComplete,
                    LoadingProgress::ReferenceComplete, "reference completion",
                    BuildCounterSignature(CounterOperation::decrement, rax, 0x170) },
                { IDs::DistantReferencesEnqueue, Offsets::DistantReferencesEnqueue,
                    LoadingProgress::DistantEnqueue, "distant enqueue",
                    BuildCounterSignature(CounterOperation::increment, rcx, 0x174) },
                { IDs::DistantReferencesComplete, Offsets::DistantReferencesComplete,
                    LoadingProgress::DistantComplete, "distant completion",
                    BuildCounterSignature(CounterOperation::decrement, rax, 0x174) },
                { IDs::BackgroundTasksProcess, Offsets::BackgroundTasksEnqueue,
                    LoadingProgress::BackgroundEnqueue, "background-task enqueue",
                    BuildCounterSignature(CounterOperation::increment,
                        REL::Module::get().version() < Runtimes::SkyrimAEStart ? rsi : r15, 0x68) },
                { IDs::BackgroundTasksProcess, Offsets::BackgroundTasksComplete,
                    LoadingProgress::BackgroundComplete, "background-task completion",
                    BuildCounterSignature(CounterOperation::decrement,
                        REL::Module::get().version() < Runtimes::SkyrimAEStart ? rsi : r15, 0x68) },

            });
        }

        // Full CONTEXT stubs are larger than ordinary branch islands, so fail before modifying code
        // when the shared SKSE trampoline no longer has a conservative amount of working space.
        void RequireQueueHookTrampolineSpace()
        {
            constexpr std::size_t minimumTrampolineBytes = 8 * 1024;

            // A trampoline is executable memory for the displaced instructions and register-saving
            // callback stub. The eight-kilobyte threshold is a conservative reserve for this set,
            // not the five-byte patch length or an exact measurement of one generated stub.
            const auto freeBytes = SKSE::GetTrampoline().free_size();
            if (freeBytes < minimumTrampolineBytes) {
                throw std::runtime_error(fmt::format(
                    "queue hooks require at least {} free trampoline bytes; {} remain",
                    minimumTrampolineBytes, freeBytes));
            }
        }

        // Resolves and verifies one decoded counter instruction without changing executable memory.
        ResolvedMutationHook ResolveMutationHook(
            const MutationHookDefinition& a_hook, const REL::Segment& a_text)
        {
            const auto functionAddress = REL::Relocation<std::uintptr_t>(a_hook.id).address();
            const auto offset = a_hook.offset.Get();
            if (!functionAddress || offset == 0 || !a_hook.callback) {
                throw std::runtime_error(fmt::format("could not resolve the {} hook", a_hook.name));
            }

            // Add the runtime-specific instruction offset to the relocated function entry.
            // Validate the entire signature lies in the executable text segment before memcmp;
            // the short-circuit test also prevents reading an out-of-range candidate address.
            const auto address = functionAddress + offset;
            const auto instructionSize = a_hook.signature.size();
            const auto textEnd = a_text.address() + a_text.size();
            const bool startsInsideText = address >= a_text.address() && address < textEnd;
            const bool hasCompleteInstruction =
                startsInsideText && instructionSize <= textEnd - address;
            if (!hasCompleteInstruction ||
                std::memcmp(reinterpret_cast<const void*>(address),
                    a_hook.signature.data(), a_hook.signature.size()) != 0) {
                throw std::runtime_error(fmt::format(
                    "{} hook bytes did not match runtime {} at {:X}", a_hook.name,
                    REL::Module::get().version().string("."), address));
            }

            // A near branch needs five bytes. Skyrim SE's background counter mutations are only
            // four bytes, so extend the patch across complete following instructions and replay the
            // entire span in the trampoline. Reject position-dependent instructions because their
            // displacement would not remain valid after relocation.
            std::size_t patchSize = instructionSize;
            while (patchSize < 5) {
                // x86 instructions have variable lengths. Decode the next complete instruction
                // instead of taking an arbitrary extra byte that could split an instruction.
                // The copied bytes will execute from a new address in trampoline storage.
                hde64s decoded{};
                const auto length = hde64_disasm(
                    reinterpret_cast<const void*>(address + patchSize), &decoded);
                // F_RELATIVE rejects control transfers encoded relative to the original instruction.
                // The ModRM check rejects RIP-relative memory addressing (mod=0, r/m=5); its effective
                // address would also change if the bytes were replayed elsewhere without relocation.
                // Fail rather than treating these position-dependent bytes as a safe copy.
                if (length == 0 || (decoded.flags & F_ERROR) != 0 ||
                    address + patchSize + length > textEnd ||
                    (decoded.flags & F_RELATIVE) != 0 ||
                    ((decoded.flags & F_MODRM) != 0 && decoded.modrm_mod == 0 && decoded.modrm_rm == 5)) {
                    throw std::runtime_error(fmt::format(
                        "could not safely extend the {} hook to a five-byte patch at {:X}",
                        a_hook.name, address));
                }

                patchSize += length;
            }

            return { std::addressof(a_hook), address, patchSize };
        }

        // Validates all mutation sites before installation starts; a failed validation writes no patches.
        auto ResolveMutationHooks(const std::span<const MutationHookDefinition> a_hooks)
        {
            std::vector<ResolvedMutationHook> resolved;
            // Complete discovery before installation. A validation failure leaves this mutation
            // hook set unwritten; resolved entries borrow the still-live definition array.
            // This does not promise rollback if a later installation step itself fails.
            resolved.reserve(a_hooks.size());

            const auto text = REL::Module::get().segment(REL::Segment::textx);
            for (const auto& hook : a_hooks) {
                resolved.push_back(ResolveMutationHook(hook, text));
            }

            return resolved;
        }

        // Copies the verified atomic instruction before calling the observer, preserving engine behavior.
        void InstallMutationHook(const ResolvedMutationHook& a_hook)
        {
            const auto& definition = *a_hook.definition;
            // The second argument is the overwritten span; the last positive argument copies
            // that same span BEFORE the callback. Skyrim's original LOCK INC/DEC runs first,
            // then the observer updates our counters, and execution resumes after the patched span.
            // CommonLib captures/restores CPU registers and flags around the CONTEXT callback.
            if (!SKSE::stl::install_context_hook(
                    a_hook.address, static_cast<int>(a_hook.patchSize), definition.callback,
                    static_cast<int>(a_hook.patchSize))) {
                throw std::runtime_error(
                    fmt::format("could not install {} hook at {:X}", definition.name, a_hook.address));
            }

            logger::info("installed direct {} hook at {:X} ({}-byte patch)",
                definition.name, a_hook.address, a_hook.patchSize);
        }

        // Installs direct hooks on the decoded reference queues and loading-task worker.
        void InstallMutationHooks()
        {
            RequireQueueHookTrampolineSpace();

            const auto definitions = GetMutationHookDefinitions();
            const auto resolved = ResolveMutationHooks(definitions);

            // Resolution above validates every site before this loop writes the first branch.
            for (const auto& hook : resolved) {
                InstallMutationHook(hook);
            }
        }

        // The IOManager counter methods are only four bytes plus RET, too short for a context hook.
        // Validate the exact bodies, then replace their virtual slots and chain the originals.
        void InstallIOTaskHooks()
        {
            using namespace Xbyak::util;

            // These tiny functions are too short for a five-byte inline branch without reaching
            // past RET into unrelated code. Replace pointers in the virtual-function table instead.
            // On Windows x64, an ordinary member call supplies this in RCX, hence [RCX + 0x30].
            constexpr std::size_t enqueueIndex = 14;
            constexpr std::size_t completeIndex = 15;
            REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_IOManager[0] };
            const auto enqueue = *reinterpret_cast<const std::uintptr_t*>(
                vtable.address() + enqueueIndex * sizeof(std::uintptr_t));
            const auto complete = *reinterpret_cast<const std::uintptr_t*>(
                vtable.address() + completeIndex * sizeof(std::uintptr_t));
            const auto enqueueSignature = BuildCounterSignature(CounterOperation::increment, rcx, 0x30);
            const auto completeSignature = BuildCounterSignature(CounterOperation::decrement, rcx, 0x30);
            // Require both the expected counter instruction and an immediate RET (opcode 0xC3).
            // Checking only the first bytes would also accept a longer function whose additional
            // behavior we have not established. No slot is written until both bodies match.
            if (!enqueue || !complete ||
                std::memcmp(reinterpret_cast<const void*>(enqueue), enqueueSignature.data(), enqueueSignature.size()) != 0 ||
                std::memcmp(reinterpret_cast<const void*>(complete), completeSignature.data(), completeSignature.size()) != 0 ||
                *reinterpret_cast<const std::uint8_t*>(enqueue + enqueueSignature.size()) != 0xC3 ||
                *reinterpret_cast<const std::uint8_t*>(complete + completeSignature.size()) != 0xC3) {
                throw std::runtime_error("IOManager task-counter hook bytes did not match this runtime");
            }

            // Save the validated function addresses before publishing our callbacks in the vtable.
            // The callbacks call these originals first, preserving engine behavior, then record
            // the mutation; calling through the patched slot would recurse into our own hook.
            LoadingProgress::originalIOTaskEnqueue =
                reinterpret_cast<LoadingProgress::IOTaskMutation_t>(enqueue);
            LoadingProgress::originalIOTaskComplete =
                reinterpret_cast<LoadingProgress::IOTaskMutation_t>(complete);
            vtable.write_vfunc(enqueueIndex, LoadingProgress::IOTaskEnqueue);
            vtable.write_vfunc(completeIndex, LoadingProgress::IOTaskComplete);
            logger::info("installed IOManager task enqueue/completion vtable hooks at {:X}/{:X}",
                enqueue, complete);
        }

        // Skyrim's final IOTask priority queue uses the same four-byte counter methods. Its vtable
        // is shared by other instances, so the callbacks count only IOManager's +0xE0 queue.
        void InstallPostProcessingHooks()
        {
            using namespace Xbyak::util;

            const auto enqueue = REL::Relocation<std::uintptr_t>(IDs::PostProcessingEnqueue).address();
            const auto complete = REL::Relocation<std::uintptr_t>(IDs::PostProcessingComplete).address();
            const auto count = REL::Relocation<std::uintptr_t>(IDs::PostProcessingCount).address();
            const auto enqueueSignature = BuildCounterSignature(CounterOperation::increment, rcx, 0x14);
            const auto completeSignature = BuildCounterSignature(CounterOperation::decrement, rcx, 0x14);
            // This third signature is MOV EAX, DWORD PTR [RCX + 0x14]; RET. The count getter
            // provides an independent check that this is the same queue/counter interface as
            // the two mutation functions, even though we do not replace the getter.
            constexpr auto countSignature = std::to_array<std::uint8_t>({ 0x8B, 0x41, 0x14, 0xC3 });
            if (!enqueue || !complete || !count ||
                std::memcmp(reinterpret_cast<const void*>(enqueue), enqueueSignature.data(), enqueueSignature.size()) != 0 ||
                std::memcmp(reinterpret_cast<const void*>(complete), completeSignature.data(), completeSignature.size()) != 0 ||
                *reinterpret_cast<const std::uint8_t*>(enqueue + enqueueSignature.size()) != 0xC3 ||
                *reinterpret_cast<const std::uint8_t*>(complete + completeSignature.size()) != 0xC3 ||
                std::memcmp(reinterpret_cast<const void*>(count), countSignature.data(), countSignature.size()) != 0) {
                throw std::runtime_error("post-processing counter hook bytes did not match this runtime");
            }

            constexpr std::size_t enqueueIndex = 1;
            constexpr std::size_t completeIndex = 2;
            constexpr std::size_t countIndex = 3;
            REL::Relocation<std::uintptr_t> vtable{
                RE::VTABLE_SynchronizedPriorityQueue_NiPointer_IOTask__[0]
            };
            const auto currentEnqueue = *reinterpret_cast<const std::uintptr_t*>(
                vtable.address() + enqueueIndex * sizeof(std::uintptr_t));
            const auto currentComplete = *reinterpret_cast<const std::uintptr_t*>(
                vtable.address() + completeIndex * sizeof(std::uintptr_t));
            const auto currentCount = *reinterpret_cast<const std::uintptr_t*>(
                vtable.address() + countIndex * sizeof(std::uintptr_t));
            // Match the vtable slots to the independently validated Address Library functions.
            // This detects an unexpected table layout or an already-redirected slot before patching;
            // we do not assume an arbitrary existing replacement implements the same contract.
            if (currentEnqueue != enqueue || currentComplete != complete || currentCount != count) {
                throw std::runtime_error(
                    "post-processing vtable did not reference the validated counter methods");
            }

            // The vtable belongs to a shared queue type. Verify the actual IOManager queue at
            // field +0xE0 uses this table; the callbacks then filter by that instance address
            // so mutations on other objects with the same vtable do not inflate loading progress.
            auto* manager = RE::IOManager::GetSingleton();
            const auto postProcessing = manager ?
                                            *reinterpret_cast<void**>(
                                                reinterpret_cast<std::uintptr_t>(manager) + 0xE0) :
                                            nullptr;
            if (!postProcessing || *reinterpret_cast<const std::uintptr_t*>(postProcessing) != vtable.address()) {
                throw std::runtime_error("could not validate IOManager's post-processing queue instance");
            }

            LoadingProgress::originalPostProcessingEnqueue =
                reinterpret_cast<LoadingProgress::PostProcessingMutation_t>(currentEnqueue);
            LoadingProgress::originalPostProcessingComplete =
                reinterpret_cast<LoadingProgress::PostProcessingMutation_t>(currentComplete);
            vtable.write_vfunc(enqueueIndex, LoadingProgress::PostProcessingEnqueue);
            vtable.write_vfunc(completeIndex, LoadingProgress::PostProcessingComplete);
            logger::info("installed IOManager post-processing enqueue/completion vtable hooks");
        }

        // Resolves the original counter callees that semantic hooks must invoke in place of E8 calls.
        void ResolveLoadedEntryCallees(const Settings::LoadedEntryLogging& a_categories)
        {
            // Unlike direct mutation hooks, these callbacks replace the original CALL rather than
            // replay it. Resolve its target separately so the callback can invoke it exactly once
            // and supply the return value expected by the caller.
            LoadingProgress::originalReferenceEnqueue =
                REL::Relocation<LoadingProgress::ReferenceEnqueue_t>(IDs::ReferencesEnqueue).get();
            LoadingProgress::originalDistantReferenceEnqueue =
                REL::Relocation<LoadingProgress::DistantReferenceEnqueue_t>(IDs::DistantReferencesEnqueue).get();

            const auto referenceAddress =
                reinterpret_cast<std::uintptr_t>(LoadingProgress::originalReferenceEnqueue);
            if ((a_categories.objectReferences || a_categories.transferredReferences) &&
                !CellTransitioner::IsExecutableAddress(referenceAddress)) {
                throw std::runtime_error("could not resolve the reference enqueue helper");
            }

            const auto distantAddress =
                reinterpret_cast<std::uintptr_t>(LoadingProgress::originalDistantReferenceEnqueue);
            if (a_categories.distantReferences &&
                !CellTransitioner::IsExecutableAddress(distantAddress)) {
                throw std::runtime_error("could not resolve the distant-reference enqueue helper");
            }
        }

        // Returns the three higher-level call paths whose registers still identify the queued reference.
        auto GetLoadedEntryHookDefinitions(const Settings::LoadedEntryLogging& a_categories)
        {
            return std::to_array<LoadedEntryHookDefinition>({
                { IDs::ObjectReferenceQueueCaller, IDs::ReferencesEnqueue,
                    LoadingProgress::ObjectReferenceQueued, "object-reference enqueue",
                    a_categories.objectReferences },
                { IDs::TransferredReferenceQueueCaller, IDs::ReferencesEnqueue,
                    LoadingProgress::TransferredReferenceQueued, "transferred-reference enqueue",
                    a_categories.transferredReferences },
                { IDs::DistantReferenceQueueCaller, IDs::DistantReferencesEnqueue,
                    LoadingProgress::DistantReferenceQueued, "distant-reference enqueue",
                    a_categories.distantReferences }
            });
        }

        // Finds exactly one call in each enabled caller and performs no patching during discovery.
        auto ResolveLoadedEntryHooks(const std::span<const LoadedEntryHookDefinition> a_hooks)
        {
            std::vector<ResolvedLoadedEntryHook> resolved;
            resolved.reserve(a_hooks.size());

            for (const auto& hook : a_hooks) {
                if (!hook.enabled) {
                    continue;
                }

                // Search the decoded caller for a unique relative call to the expected callee.
                // The caller/callee relationship identifies the semantic enqueue path without a
                // hard-coded call-site offset; zero or multiple matches are rejected by the resolver.
                const auto callSite =
                    CellTransitioner::FindUniqueRelativeCall(hook.caller, hook.callee, hook.name);
                resolved.push_back({ std::addressof(hook), callSite });
            }

            return resolved;
        }

        // Replaces one E8 rel32 call with a context callback that invokes the same callee itself.
        void InstallLoadedEntryHook(const ResolvedLoadedEntryHook& a_hook)
        {
            // E8 plus its signed rel32 displacement is a five-byte x64 near call. Copying those bytes
            // into a trampoline would preserve the old relative displacement and jump to the wrong
            // address. The callback therefore replaces all five bytes, calls the Address Library
            // target directly, and places its return value back in the captured RAX register.
            constexpr std::size_t relativeCallSize = 5;
            constexpr std::size_t copiedInstructionBytes = 0;

            // includeSize=0 deliberately omits the displaced CALL from replay. The callback
            // captures numeric reference IDs, invokes the original target, and writes its result
            // into CONTEXT::Rax before the stub restores registers and resumes after the CALL.
            // Replaying the CALL as well would enqueue/count the same operation twice.
            const auto& definition = *a_hook.definition;
            if (!SKSE::stl::install_context_hook(
                    a_hook.callSite, relativeCallSize, definition.callback, copiedInstructionBytes)) {
                throw std::runtime_error(
                    fmt::format("could not install {} hook at {:X}", definition.name, a_hook.callSite));
            }

            logger::info("installed {} hook at {:X}", definition.name, a_hook.callSite);
        }

        // Installs only the semantic loaded-entry categories enabled in the startup configuration.
        void InstallLoadedEntryHooks()
        {
            const auto& settings = Settings::GetSingleton();
            if (!settings.IsLoadedEntryLoggingEnabled()) {
                return;
            }

            const auto& categories = settings.GetLoadedEntryLogging();
            ResolveLoadedEntryCallees(categories);

            const auto definitions = GetLoadedEntryHookDefinitions(categories);
            const auto resolved = ResolveLoadedEntryHooks(definitions);

            // As with mutation hooks, validate every enabled call before changing executable memory.
            for (const auto& hook : resolved) {
                InstallLoadedEntryHook(hook);
            }
        }
    }

    /**
     * @brief Installs the LoadingMenu message and movie-advance hooks.
     */
    void InstallLoadingMenuHook()
    {
        // CommonLib's IMenu vtable maps slot 4 to ProcessMessage and slot 5 to AdvanceMovie.
        constexpr std::size_t processMessageIndex = 0x04;
        constexpr std::size_t advanceMovieIndex = 0x05;

        REL::Relocation<std::uintptr_t> vtable{ RE::LoadingMenu::VTABLE[0] };
        if (!vtable.address()) {
            throw std::runtime_error("could not resolve the LoadingMenu vtable");
        }

        // The relocation identifies the vtable array itself. Each slot is one native function pointer,
        // so the byte address is the slot index multiplied by the pointer size.
        const auto processAddress = *reinterpret_cast<const std::uintptr_t*>(
            vtable.address() + processMessageIndex * sizeof(std::uintptr_t));
        const auto advanceAddress = *reinterpret_cast<const std::uintptr_t*>(
            vtable.address() + advanceMovieIndex * sizeof(std::uintptr_t));
        if (!CellTransitioner::IsExecutableAddress(processAddress) ||
            !CellTransitioner::IsExecutableAddress(advanceAddress)) {
            throw std::runtime_error("LoadingMenu had an invalid original vtable function");
        }

        // Save both original virtual functions before redirecting the slots. ProcessMessage
        // prepares presentation when the menu is shown; AdvanceMovie drains worker observations
        // and updates Scaleform on its movie-update path while continuing native menu behavior.
        LoadingProgress::originalLoadingProcessMessage =
            reinterpret_cast<LoadingProgress::ProcessMessage_t>(processAddress);
        LoadingProgress::originalAdvanceMovie =
            reinterpret_cast<LoadingProgress::AdvanceMovie_t>(advanceAddress);

        vtable.write_vfunc(processMessageIndex, LoadingProgress::LoadingMenuProcessMessage);
        vtable.write_vfunc(advanceMovieIndex, LoadingProgress::LoadingMenuAdvanceMovie);

        logger::info("installed hidden LoadingMenu::AdvanceMovie experiment hook");
    }

    /**
     * @brief Seeds the aggregator with work already observed in the live queue counters.
     */
    void LoadingProgress::SeedQueuedWork()
    {
        for (std::size_t i = 0; i < queueCount; ++i) {
            const auto baseline = Atomic::get(liveRemaining[i], std::memory_order_relaxed);
            aggregator.Enqueue(static_cast<Queue>(i), baseline);
        }
    }

    /**
     * @brief Starts aggregation when Skyrim opens LoadingMenu.
     */
    void LoadingProgress::BeginLoadingEpoch()
    {
        std::scoped_lock lock(stateLock);

        // ProcessMessage normally arms this capture first. This fallback also covers UI event-order
        // changes in which the menu-open notification arrives before the show message.
        BeginLoadedEntryCapture();

        Atomic::set(displayedBasisPoints, 0);
        const auto now = MonotonicMilliseconds();
        Atomic::set(epochStartedMs, now, std::memory_order_relaxed);
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {

            Atomic::set(traceLastSampleMs, 0, std::memory_order_relaxed);
            Atomic::set(traceLastQueueActivityMs, now, std::memory_order_relaxed);
            Atomic::set(traceLastProgressAdvanceMs, now, std::memory_order_relaxed);
        }

        for (std::size_t i = 0; i < queueCount; ++i) {
            Atomic::set(pendingEnqueued[i], 0, std::memory_order_relaxed);
            Atomic::set(pendingCompleted[i], 0, std::memory_order_relaxed);
        }

        // Aggregator state is serialized by stateLock; worker hooks only update the atomic
        // counters. A LoadingMenu epoch groups those observations into one progress lifetime.
        aggregator.Begin();

        // Publish the epoch before reading liveRemaining so worker completes cannot land in a
        // "seeded but not yet accepting pending" gap. Stabilise by clearing pending and rereading
        // until a quiet sample across all queues; SeedQueuedWork then snapshots liveRemaining.
        // Mutations after that sample remain in pending for the drain below / AdvanceMovie.
        Atomic::set(epochActive, true);
        constexpr std::uint32_t quietAttempts = 64;
        for (std::uint32_t attempt = 0; attempt < quietAttempts; ++attempt) {
            bool quiet = true;
            for (std::size_t i = 0; i < queueCount; ++i) {
                Atomic::set(pendingEnqueued[i], 0, std::memory_order_relaxed);
                Atomic::set(pendingCompleted[i], 0, std::memory_order_relaxed);
            }
            for (std::size_t i = 0; i < queueCount; ++i) {
                if (Atomic::get(pendingEnqueued[i], std::memory_order_relaxed) != 0 ||
                    Atomic::get(pendingCompleted[i], std::memory_order_relaxed) != 0) {

                    quiet = false;
                    break;
                }
            }
            if (quiet) {
                break;
            }
        }
        SeedQueuedWork();
        for (std::size_t i = 0; i < queueCount; ++i) {
            const auto enqueued = Atomic::get_and_set(pendingEnqueued[i], 0);
            const auto completed = Atomic::get_and_set(pendingCompleted[i], 0);
            const auto queue = static_cast<Queue>(i);
            if (enqueued != 0) {
                aggregator.Enqueue(queue, enqueued);
            }

            if (completed != 0) {
                aggregator.Complete(queue, completed);
            }
        }

        if (Settings::GetSingleton().IsVerboseQueueLoggingEnabled()) {

            lastLogged = {};
        }

        CellTransitioner::BeginLoad();

        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::info(
                "loading epoch began: Loading Menu opened; baseline remaining={}", aggregator.Current().remaining);
        }

        UpdateDisplayedProgress(aggregator.Current());
    }

    /**
     * @brief Ends aggregation and starts the retained-frame transition into gameplay.
     */
    void LoadingProgress::EndLoadingEpoch()
    {
        std::scoped_lock lock(stateLock);

        // Skyrim can broadcast a LoadingMenu close notification for a queued hide even when no
        // corresponding menu-open event established a loading epoch. Treating that notification as a
        // completed load starts CellTransitioner's post-load compositor with a stale retained frame.
        // DA09's scripted same-cell MoveTo sequence exercises this path around its white IMODs.
        if (!Atomic::get_and_clear(epochActive)) {

            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("ignored Loading Menu close without an active loading epoch");
            }

            return;
        }

        // Fold any final worker deltas before discarding pending counters so end-of-load logs and
        // the last meter sample reflect work that completed after the previous AdvanceMovie drain.
        for (std::size_t i = 0; i < queueCount; ++i) {
            const auto enqueued = Atomic::get_and_set(pendingEnqueued[i], 0);
            const auto completed = Atomic::get_and_set(pendingCompleted[i], 0);
            const auto queue = static_cast<Queue>(i);
            if (enqueued != 0) {
                aggregator.Enqueue(queue, enqueued);
            }

            if (completed != 0) {
                aggregator.Complete(queue, completed);
            }
        }
        EndLoadedEntryCapture();
        CellTransitioner::EndLoad();

        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {

            const auto final = aggregator.Current();
            const auto now = MonotonicMilliseconds();
            logger::info(
                "loading epoch ended: Loading Menu closed; epoch_ms={} progress_idle_ms={} queue_idle_ms={} displayed={:.2f}% completed={} remaining={} total={}",
                now - Atomic::get(epochStartedMs, std::memory_order_relaxed),
                now - Atomic::get(traceLastProgressAdvanceMs, std::memory_order_relaxed),
                now - Atomic::get(traceLastQueueActivityMs, std::memory_order_relaxed),
                static_cast<double>(Atomic::get(displayedBasisPoints)) / 100.0,
                final.completed, final.remaining, final.total);
        }

        aggregator.End();
    }

    /**
     * @brief Starts or ends an aggregation epoch with LoadingMenu's lifetime.
     */
    RE::BSEventNotifyControl LoadingProgress::ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
    {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (a_event->menuName == RE::MainMenu::MENU_NAME && a_event->opening) {
            CellTransitioner::ObserveMainMenuOpening();
        }

        if (a_event->menuName == RE::HUDMenu::MENU_NAME && a_event->opening) {

            if (Atomic::get(hooksEnabled)) {

                try {
                    CellTransitioner::ObserveHUDMenuOpening();
                } catch (const std::exception& error) {
                    DisableHooks(error.what());
                } catch (...) {
                    DisableHooks("unknown exception while hiding a newly opened HUDMenu");
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }

        if (a_event->menuName == RE::SleepWaitMenu::MENU_NAME) {

            a_event->opening ?
                CellTransitioner::ObserveSleepWaitMenuOpening() :
                CellTransitioner::ObserveSleepWaitMenuClosing();
            return RE::BSEventNotifyControl::kContinue;
        }

        if (a_event->menuName != RE::LoadingMenu::MENU_NAME) {
            return RE::BSEventNotifyControl::kContinue;
        }

        if (!Atomic::get(hooksEnabled)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        try {
            a_event->opening ? BeginLoadingEpoch() : EndLoadingEpoch();
        } catch (const std::exception& error) {
            DisableHooks(error.what());
        } catch (...) {
            DisableHooks("unknown exception in LoadingMenu event sink");
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    /**
     * @brief Logs the fully-loaded milestone while a transition is being observed.
     */
    RE::BSEventNotifyControl LoadingProgress::ProcessEvent(
        const RE::TESCellFullyLoadedEvent* a_event,
        RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*)
    {
        if (!Settings::GetSingleton().IsLoadingLoggingEnabled() || !Atomic::get(hooksEnabled)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        try {
            if (Atomic::get(CellTransitioner::renderObservationState) != 0 && a_event &&
                a_event->cell) {

                const auto* editorID = a_event->cell->GetFormEditorID();
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    logger::info(
                        "cell fully loaded: formID={:08X} editorID='{}' menuOpen={} liveRemaining={}",
                        a_event->cell->GetFormID(), editorID ? editorID : "",
                        Atomic::get(epochActive), GetLiveRemaining());
                }
            }
        } catch (const std::exception& error) {
            DisableHooks(error.what());
        } catch (...) {
            DisableHooks("unknown exception in cell-loaded event sink");
        }

        return RE::BSEventNotifyControl::kContinue;
    }
    /**
     * @brief Resets all counters and starts a new loading epoch.
     */
    void LoadingProgress::Aggregator::Begin()
    {
        remaining_.fill(0);
        total_.fill(0);
        progress_ = {};
        active_ = true;
    }

    /**
     * @brief Adds the requested number of work items to a tracked queue.
     */
    void LoadingProgress::Aggregator::Enqueue(
        LoadingProgress::Queue a_queue, std::uint64_t a_count)
    {
        if (!active_ || a_count == 0) {
            return;
        }

        const auto index = static_cast<std::size_t>(a_queue);
        if (index >= queueCount) {
            return;
        }

        remaining_[index] += a_count;
        total_[index] += a_count;
        if (Settings::GetSingleton().IsVerboseQueueLoggingEnabled()) {
            logger::info("queue '{}' enqueued {} item(s)", queueNames[index], a_count);
        }

        Recalculate();
    }

    /**
     * @brief Completes the requested number of work items, including work first seen at completion.
     */
    void LoadingProgress::Aggregator::Complete(
        LoadingProgress::Queue a_queue, std::uint64_t a_count)
    {
        if (!active_ || a_count == 0) {
            return;
        }

        const auto index = static_cast<std::size_t>(a_queue);
        if (index >= queueCount) {
            return;
        }

        // A completion can refer to work enqueued before we started tracking. Count that
        // unobserved work as both total and completed rather than underflowing the queue or
        // losing the completion from the aggregate.
        const auto observed = std::min(remaining_[index], a_count);
        const auto unobserved = a_count - observed;
        remaining_[index] -= observed;
        total_[index] += unobserved;
        if (Settings::GetSingleton().IsVerboseQueueLoggingEnabled()) {
            logger::info("queue '{}' completed {} item(s), including {} unobserved",
                queueNames[index], a_count, unobserved);
        }

        Recalculate();
    }

    // Rebuilds aggregate progress from the per-queue counters.
    void LoadingProgress::Aggregator::Recalculate()
    {
        progress_.total = 0;
        progress_.remaining = 0;
        for (std::size_t i = 0; i < queueCount; ++i) {
            progress_.total += total_[i];
            progress_.remaining += remaining_[i];
        }
        progress_.completed = progress_.total - progress_.remaining;
        progress_.fraction = progress_.total ?
                                 static_cast<double>(progress_.completed) / static_cast<double>(progress_.total) :
                                 0.0;
    }

    /**
     * @brief Returns the most recently calculated aggregate progress.
     */
    LoadingProgress::Progress LoadingProgress::Aggregator::Current() const { return progress_; }

    /**
     * @brief Stops accepting queue mutations for the current epoch.
     */
    void LoadingProgress::Aggregator::End() { active_ = false; }

    /**
     * @brief Installs loading progress hooks and registers the singleton event sink.
     */
    void InstallHooks()
    {
        auto& events = LoadingProgress::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        const bool diagnostics = Settings::GetSingleton().IsLoadingLoggingEnabled();
        auto* eventSources = diagnostics ? RE::ScriptEventSourceHolder::GetSingleton() : nullptr;
        if (!ui || (diagnostics && !eventSources)) {
            throw std::runtime_error("could not find Skyrim's UI or script event source holder");
        }

        InstallMutationHooks();
        InstallIOTaskHooks();
        InstallPostProcessingHooks();
        InstallLoadedEntryHooks();
        InstallLoadingMenuHook();

        ui->AddEventSink<RE::MenuOpenCloseEvent>(&events);
        if (diagnostics) {
            eventSources->AddEventSink<RE::TESCellFullyLoadedEvent>(&events);
        }

        Atomic::set(LoadingProgress::hooksEnabled, true);

        logger::info("installed loading-menu event sink; cell diagnostics={}", diagnostics);
    }
}
