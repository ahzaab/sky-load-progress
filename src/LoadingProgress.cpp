// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "CellTransitioner.h"
#include "IdsAndOffsets.h"
#include "LoadingProgress.h"
#include "ProgressMeter.h"
#include "Settings.h"

#include <xbyak/xbyak.h>

namespace load_progress
{
    // Returns the singleton that owns loading aggregation and event handling.
    LoadingProgress& LoadingProgress::GetSingleton()
    {
        static LoadingProgress singleton;
        return singleton;
    }

    // Sums the directly observed work that is still queued across all pools.
    std::uint64_t LoadingProgress::GetLiveRemaining()
    {
        std::uint64_t remaining = 0;
        for (const auto& queue : liveRemaining) {
            remaining += queue.load(std::memory_order_relaxed);
        }
        return remaining;
    }

    // Applies queue deltas outside the engine's queue-mutation call paths.
    void LoadingProgress::DrainQueueMutations()
    {
        if (!epochActive.load(std::memory_order_acquire)) {
            return;
        }

        std::scoped_lock lock(stateLock);
        for (std::size_t i = 0; i < queueCount; ++i) {
            const auto enqueued = pendingEnqueued[i].exchange(0, std::memory_order_acq_rel);
            const auto completed = pendingCompleted[i].exchange(0, std::memory_order_acq_rel);
            const auto queue = static_cast<Queue>(i);

            if (enqueued != 0) {
                aggregator.Enqueue(queue, enqueued);
            }
            if (completed != 0) {
                aggregator.Complete(queue, completed);
            }
        }
        LogProgress(aggregator.Current());
    }

    // Resolves captured numeric IDs on the loading-menu thread and releases their ring slots.
    void LoadingProgress::DrainLoadedEntries(bool a_writeLog)
    {
        static constexpr auto typeNames = std::to_array<std::string_view>(
            { "object-reference", "transferred-reference", "distant-reference" });

        for (auto& slot : loadedEntries) {
            if (slot.state.load(std::memory_order_acquire) != 2) {
                continue;
            }

            const auto entry = slot.entry;
            // Release returns ownership only after this thread has copied the published record.
            slot.state.store(0, std::memory_order_release);

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

    // Clears stale details and enables the lock-free producer path for a new Loading Menu lifetime.
    void LoadingProgress::BeginLoadedEntryCapture()
    {
        const auto& settings = Settings::GetSingleton();
        if (!settings.IsLoadedEntryLoggingEnabled() ||
            loadedEntryCaptureActive.load(std::memory_order_acquire)) {
            return;
        }

        // Capture is still disabled, so no new producer can enter while stale slots are reclaimed.
        WaitForLoadedEntryWriters();

        DrainLoadedEntries(false);
        for (auto& tally : loadedEntryTallies) {
            tally.store(0, std::memory_order_relaxed);
        }
        loadedEntryWriteCursor.store(0, std::memory_order_relaxed);
        droppedLoadedEntryDetails.store(0, std::memory_order_relaxed);
        loadedEntryCaptureActive.store(true, std::memory_order_release);
    }

    // Stops producers, drains their final details, and writes exact per-category enqueue totals.
    void LoadingProgress::EndLoadedEntryCapture()
    {
        if (!loadedEntryCaptureActive.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        WaitForLoadedEntryWriters();

        DrainLoadedEntries(true);
        logger::info(
            "loaded-entry tally: object-references={} transferred-references={} distant-references={} dropped-details={}",
            loadedEntryTallies[static_cast<std::size_t>(LoadedEntryType::objectReference)].load(
                std::memory_order_relaxed),
            loadedEntryTallies[static_cast<std::size_t>(LoadedEntryType::transferredReference)].load(
                std::memory_order_relaxed),
            loadedEntryTallies[static_cast<std::size_t>(LoadedEntryType::distantReference)].load(
                std::memory_order_relaxed),
            droppedLoadedEntryDetails.load(std::memory_order_relaxed));
    }

    // Waits only after capture has been disabled. Producers never wait on the loading-menu thread.
    void LoadingProgress::WaitForLoadedEntryWriters()
    {
        while (loadedEntryWriters.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
    }

    // Converts the entry type into its independently configurable diagnostic switch.
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

    // Reserves one ring slot and publishes the completed record to the loading-menu consumer.
    bool LoadingProgress::TryStoreLoadedEntry(const LoadedEntry& a_entry) noexcept
    {
        // A few retries avoid dropping detail when the cursor meets a slot the consumer has not
        // released yet. The separate tallies remain exact even when this detail buffer is saturated.
        constexpr std::size_t reservationAttempts = 8;

        for (std::size_t attempt = 0; attempt < reservationAttempts; ++attempt) {
            const auto index = loadedEntryWriteCursor.fetch_add(1, std::memory_order_relaxed) % loadedEntryCapacity;
            auto       expectedState = std::uint8_t{ 0 };
            auto&      slot = loadedEntries[index];

            // State 0 is free, state 1 belongs to a producer, and state 2 is ready for the consumer.
            // The acquire/release pair ensures the consumer cannot see a partially written entry.
            if (!slot.state.compare_exchange_strong(
                    expectedState, std::uint8_t{ 1 }, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                continue;
            }

            slot.entry = a_entry;
            slot.state.store(2, std::memory_order_release);
            return true;
        }

        return false;
    }

    // Copies only stable numeric identifiers from a loading worker into the diagnostic ring.
    void LoadingProgress::CaptureLoadedEntry(
        LoadedEntryType a_type, RE::TESObjectREFR* a_reference, RE::TESObjectCELL* a_cell) noexcept
    {
        const auto typeIndex = static_cast<std::size_t>(a_type);
        if (!a_reference || !a_cell || typeIndex >= loadedEntryTypeCount ||
            !IsLoadedEntryTypeEnabled(a_type) ||
            !loadedEntryCaptureActive.load(std::memory_order_acquire)) {
            return;
        }

        // Count active producers so EndLoadedEntryCapture cannot reclaim a slot mid-write.
        loadedEntryWriters.fetch_add(1, std::memory_order_acq_rel);
        if (!loadedEntryCaptureActive.load(std::memory_order_acquire)) {
            loadedEntryWriters.fetch_sub(1, std::memory_order_release);
            return;
        }

        // Do not resolve Editor IDs here. This callback can run on an engine loading worker, where
        // form lookups and the logger are unsafe. DrainLoadedEntries performs that work later.
        const auto* base = a_reference->GetBaseObject();
        const LoadedEntry entry{
            a_type, a_reference->GetFormID(), base ? base->GetFormID() : 0, a_cell->GetFormID()
        };
        loadedEntryTallies[typeIndex].fetch_add(1, std::memory_order_relaxed);

        if (!TryStoreLoadedEntry(entry)) {
            droppedLoadedEntryDetails.fetch_add(1, std::memory_order_relaxed);
        }
        loadedEntryWriters.fetch_sub(1, std::memory_order_release);
    }

    // The ordinary object path keeps the reference in RDI and its cell in RCX at the enqueue call.
    void LoadingProgress::ObjectReferenceQueued(CONTEXT& a_context) noexcept
    {
        CaptureLoadedEntry(LoadedEntryType::objectReference,
            reinterpret_cast<RE::TESObjectREFR*>(a_context.Rdi),
            reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx));
        if (originalReferenceEnqueue) {
            a_context.Rax = originalReferenceEnqueue(reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx));
        }
    }

    // The transfer path keeps the moved reference in RBX and its destination cell in RCX.
    void LoadingProgress::TransferredReferenceQueued(CONTEXT& a_context) noexcept
    {
        CaptureLoadedEntry(LoadedEntryType::transferredReference,
            reinterpret_cast<RE::TESObjectREFR*>(a_context.Rbx),
            reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx));
        if (originalReferenceEnqueue) {
            a_context.Rax = originalReferenceEnqueue(reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx));
        }
    }

    // The distant path passes the reference to its counter helper in RDX and the cell in RCX.
    void LoadingProgress::DistantReferenceQueued(CONTEXT& a_context) noexcept
    {
        CaptureLoadedEntry(LoadedEntryType::distantReference,
            reinterpret_cast<RE::TESObjectREFR*>(a_context.Rdx),
            reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx));
        if (originalDistantReferenceEnqueue) {
            a_context.Rax = originalDistantReferenceEnqueue(
                reinterpret_cast<RE::TESObjectCELL*>(a_context.Rcx),
                reinterpret_cast<RE::TESObjectREFR*>(a_context.Rdx));
        }
    }

    // Updates the progress widget and hides Scaleform for warm transitions.
    void LoadingProgress::LoadingMenuAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
    {
        if (originalAdvanceMovie) {
            originalAdvanceMovie(a_menu, a_interval, a_currentTime);
        }

        if (!hooksEnabled.load(std::memory_order_acquire)) {
            return;
        }

        try {
            DrainQueueMutations();
            if (loadedEntryCaptureActive.load(std::memory_order_acquire)) {
                DrainLoadedEntries(true);
            }

            // HUDMenu can be created or made visible after LoadingMenu opens on a Main Menu save load.
            CellTransitioner::HideHUDForLoad();

            if (a_menu && a_menu->uiMovie) {
                const bool seamless = CellTransitioner::IsSeamless();
                const bool vanilla = CellTransitioner::IsVanilla();
                const auto& progressBar = Settings::GetSingleton().GetProgressBar();
                if (!vanilla) {
                    a_menu->uiMovie->SetBackgroundAlpha(0.0F);
                    a_menu->uiMovie->SetVisible(!seamless);
                }
                const bool showProgress =
                    progressBar.mode != Settings::ProgressBar::Mode::disabled &&
                    (!vanilla || progressBar.mode == Settings::ProgressBar::Mode::all);
                ProgressMeter::GetSingleton().SetVisible(a_menu, showProgress);
                if (!seamless && showProgress) {
                    const auto basisPoints = displayedBasisPoints.load(std::memory_order_acquire);
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

    // Locks the source frame and configures the selected presentation when the menu opens.
    RE::UI_MESSAGE_RESULTS LoadingProgress::LoadingMenuProcessMessage(RE::IMenu* a_menu, RE::UIMessage& a_message)
    {
        if (hooksEnabled.load(std::memory_order_acquire) &&
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

    void LoadingProgress::LogProgress(const Progress& a_progress)
    {
        const auto candidate = a_progress.total ?
                                   static_cast<std::uint32_t>(std::clamp(a_progress.fraction * 10000.0, 0.0, 10000.0)) :
                                   0u;
        auto       displayed = displayedBasisPoints.load(std::memory_order_relaxed);
        // Newly queued work can lower the raw fraction, but the meter should not move backward.
        while (candidate > displayed &&
               !displayedBasisPoints.compare_exchange_weak(
                   displayed, candidate, std::memory_order_release, std::memory_order_relaxed)) {}
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

    // Disables plugin behavior while leaving every installed hook as a pass-through.
    void LoadingProgress::DisableHooks(std::string_view a_reason) noexcept
    {
        hooksEnabled.store(false, std::memory_order_release);
        epochActive.store(false, std::memory_order_release);
        loadedEntryCaptureActive.store(false, std::memory_order_release);

        if (!failureLogged.exchange(true, std::memory_order_acq_rel)) {
            try {
                logger::critical("loading-progress hooks disabled: {}", a_reason);
            } catch (...) {
                REX::W32::OutputDebugStringA("Skyrim Load Progress: loading-progress hooks disabled\n");
            }
        }
    }

    // Records a direct queue increment without locking or logging on the engine worker thread.
    void LoadingProgress::OnEnqueue(Queue a_queue) noexcept
    {
        const auto index = static_cast<std::size_t>(a_queue);
        if (index >= queueCount || !hooksEnabled.load(std::memory_order_relaxed)) {
            return;
        }

        liveRemaining[index].fetch_add(1, std::memory_order_relaxed);
        if (epochActive.load(std::memory_order_relaxed)) {
            pendingEnqueued[index].fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Records a direct queue decrement without locking or logging on the engine worker thread.
    void LoadingProgress::OnComplete(Queue a_queue) noexcept
    {
        const auto index = static_cast<std::size_t>(a_queue);
        if (index >= queueCount || !hooksEnabled.load(std::memory_order_relaxed)) {
            return;
        }

        auto& live = liveRemaining[index];
        auto  value = live.load(std::memory_order_relaxed);
        // Saturate at zero because the plugin may begin observing after Skyrim queued the work.
        while (value != 0 && !live.compare_exchange_weak(value, value - 1, std::memory_order_relaxed)) {}
        if (epochActive.load(std::memory_order_relaxed)) {
            pendingCompleted[index].fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Context-hook adapters identify which engine queue changed.
    void LoadingProgress::CriticalEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::criticalReferences); }
    void LoadingProgress::CriticalComplete(CONTEXT&) noexcept { OnComplete(Queue::criticalReferences); }
    void LoadingProgress::ReferenceEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::references); }
    void LoadingProgress::ReferenceComplete(CONTEXT&) noexcept { OnComplete(Queue::references); }
    void LoadingProgress::DistantEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::distantReferences); }
    void LoadingProgress::DistantComplete(CONTEXT&) noexcept { OnComplete(Queue::distantReferences); }
    void LoadingProgress::BackgroundEnqueue(CONTEXT&) noexcept { OnEnqueue(Queue::backgroundProcessing); }
    void LoadingProgress::BackgroundComplete(CONTEXT&) noexcept { OnComplete(Queue::backgroundProcessing); }
    void LoadingProgress::IOTaskEnqueue(RE::IOManager* a_manager) noexcept
    {
        if (originalIOTaskEnqueue) {
            originalIOTaskEnqueue(a_manager);
        }
        OnEnqueue(Queue::tasks);
    }

    void LoadingProgress::IOTaskComplete(RE::IOManager* a_manager) noexcept
    {
        if (originalIOTaskComplete) {
            originalIOTaskComplete(a_manager);
        }
        OnComplete(Queue::tasks);
    }

    void LoadingProgress::PostProcessingEnqueue(void* a_queue) noexcept
    {
        if (originalPostProcessingEnqueue) {
            originalPostProcessingEnqueue(a_queue);
        }
        const auto* manager = RE::IOManager::GetSingleton();
        const auto* postProcessing = manager ?
                                         *reinterpret_cast<void* const*>(
                                             reinterpret_cast<std::uintptr_t>(manager) + 0xE0) :
                                         nullptr;
        if (a_queue == postProcessing) {
            OnEnqueue(Queue::postProcessing);
        }
    }

    void LoadingProgress::PostProcessingComplete(void* a_queue) noexcept
    {
        if (originalPostProcessingComplete) {
            originalPostProcessingComplete(a_queue);
        }
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
            REL::RelocationID id;
            RuntimeOffset     offset;
            ContextCallback   callback;
            std::string_view  name;
            CounterSignature  signature;
        };

        struct ResolvedMutationHook
        {
            const MutationHookDefinition* definition;
            std::uintptr_t                 address;
        };

        struct LoadedEntryHookDefinition
        {
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
            const auto           counter = assembler.dword[a_owner + a_counterOffset];

            // LOCK is required because several loading workers can mutate the same counter.
            assembler.lock();
            if (a_operation == CounterOperation::increment) {
                assembler.inc(counter);
            } else {
                assembler.dec(counter);
            }
            assembler.ready();

            const auto* bytes = assembler.getCode();
            return { bytes, bytes + assembler.getSize() };
        }

        // Returns the six queue-counter mutations: enqueue and completion for each observed queue.
        auto GetMutationHookDefinitions()
        {
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

            return { std::addressof(a_hook), address };
        }

        // Resolves all counter sites first so a bad runtime cannot leave a partially installed set.
        auto ResolveMutationHooks(const std::span<const MutationHookDefinition> a_hooks)
        {
            std::vector<ResolvedMutationHook> resolved;
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
            const auto  instructionSize = definition.signature.size();
            if (!SKSE::stl::install_context_hook(
                    a_hook.address, static_cast<int>(instructionSize), definition.callback,
                    static_cast<int>(instructionSize))) {
                throw std::runtime_error(
                    fmt::format("could not install {} hook at {:X}", definition.name, a_hook.address));
            }

            logger::info("installed direct {} hook at {:X}", definition.name, a_hook.address);
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

            const auto enqueue = REL::Relocation<std::uintptr_t>(IDs::IOTasksEnqueue).address();
            const auto complete = REL::Relocation<std::uintptr_t>(IDs::IOTasksComplete).address();
            const auto enqueueSignature = BuildCounterSignature(CounterOperation::increment, rcx, 0x30);
            const auto completeSignature = BuildCounterSignature(CounterOperation::decrement, rcx, 0x30);
            if (!enqueue || !complete ||
                std::memcmp(reinterpret_cast<const void*>(enqueue), enqueueSignature.data(), enqueueSignature.size()) != 0 ||
                std::memcmp(reinterpret_cast<const void*>(complete), completeSignature.data(), completeSignature.size()) != 0 ||
                *reinterpret_cast<const std::uint8_t*>(enqueue + enqueueSignature.size()) != 0xC3 ||
                *reinterpret_cast<const std::uint8_t*>(complete + completeSignature.size()) != 0xC3) {
                throw std::runtime_error("IOManager task-counter hook bytes did not match this runtime");
            }

            constexpr std::size_t enqueueIndex = 14;
            constexpr std::size_t completeIndex = 15;
            REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_IOManager[0] };
            const auto currentEnqueue = *reinterpret_cast<const std::uintptr_t*>(
                vtable.address() + enqueueIndex * sizeof(std::uintptr_t));
            const auto currentComplete = *reinterpret_cast<const std::uintptr_t*>(
                vtable.address() + completeIndex * sizeof(std::uintptr_t));
            if (currentEnqueue != enqueue || currentComplete != complete) {
                throw std::runtime_error("IOManager task-counter vtable did not reference the validated methods");
            }

            LoadingProgress::originalIOTaskEnqueue =
                reinterpret_cast<LoadingProgress::IOTaskMutation_t>(currentEnqueue);
            LoadingProgress::originalIOTaskComplete =
                reinterpret_cast<LoadingProgress::IOTaskMutation_t>(currentComplete);
            vtable.write_vfunc(enqueueIndex, LoadingProgress::IOTaskEnqueue);
            vtable.write_vfunc(completeIndex, LoadingProgress::IOTaskComplete);
            logger::info("installed IOManager task enqueue/completion vtable hooks");
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
            if (currentEnqueue != enqueue || currentComplete != complete || currentCount != count) {
                throw std::runtime_error(
                    "post-processing vtable did not reference the validated counter methods");
            }

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

    // Installs the LoadingMenu message and movie-advance hooks.
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

        LoadingProgress::originalLoadingProcessMessage =
            reinterpret_cast<LoadingProgress::ProcessMessage_t>(processAddress);
        LoadingProgress::originalAdvanceMovie =
            reinterpret_cast<LoadingProgress::AdvanceMovie_t>(advanceAddress);

        vtable.write_vfunc(processMessageIndex, LoadingProgress::LoadingMenuProcessMessage);
        vtable.write_vfunc(advanceMovieIndex, LoadingProgress::LoadingMenuAdvanceMovie);

        logger::info("installed hidden LoadingMenu::AdvanceMovie experiment hook");
    }

    void LoadingProgress::SeedQueuedWork()
    {
        for (std::size_t i = 0; i < queueCount; ++i) {
            const auto baseline = liveRemaining[i].load(std::memory_order_relaxed);
            aggregator.Enqueue(static_cast<Queue>(i), baseline);
        }
    }

    // Starts aggregation when Skyrim opens LoadingMenu.
    void LoadingProgress::BeginLoadingEpoch()
    {
        std::scoped_lock lock(stateLock);

        // ProcessMessage normally arms this capture first. This fallback also covers UI event-order
        // changes in which the menu-open notification arrives before the show message.
        BeginLoadedEntryCapture();

        displayedBasisPoints.store(0, std::memory_order_release);
        for (std::size_t i = 0; i < queueCount; ++i) {
            pendingEnqueued[i].store(0, std::memory_order_relaxed);
            pendingCompleted[i].store(0, std::memory_order_relaxed);
        }

        aggregator.Begin();
        SeedQueuedWork();

        lastLogged = {};
        epochActive.store(true, std::memory_order_release);
        CellTransitioner::BeginLoad();

        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::info(
                "loading epoch began: Loading Menu opened; baseline remaining={}", aggregator.Current().remaining);
        }
        LogProgress(aggregator.Current());
    }

    // Ends aggregation and starts the retained-frame transition into gameplay.
    void LoadingProgress::EndLoadingEpoch()
    {
        std::scoped_lock lock(stateLock);

        // Skyrim can broadcast a LoadingMenu close notification for a queued hide even when no
        // corresponding menu-open event established a loading epoch. Treating that notification as a
        // completed load starts CellTransitioner's post-load compositor with a stale retained frame.
        // DA09's scripted same-cell MoveTo sequence exercises this path around its white IMODs.
        if (!epochActive.exchange(false, std::memory_order_acq_rel)) {
            if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                logger::info("ignored Loading Menu close without an active loading epoch");
            }
            return;
        }

        for (std::size_t i = 0; i < queueCount; ++i) {
            pendingEnqueued[i].store(0, std::memory_order_relaxed);
            pendingCompleted[i].store(0, std::memory_order_relaxed);
        }
        EndLoadedEntryCapture();
        CellTransitioner::EndLoad();

        const auto final = aggregator.Current();
        if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
            logger::info("loading epoch ended: Loading Menu closed; completed={} remaining={} total={}",
                final.completed, final.remaining, final.total);
        }
        aggregator.End();
    }

    // Starts or ends an aggregation epoch with LoadingMenu's lifetime.
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
            if (hooksEnabled.load(std::memory_order_acquire)) {
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

        if (!hooksEnabled.load(std::memory_order_acquire)) {
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

    // Logs the fully-loaded milestone while a transition is being observed.
    RE::BSEventNotifyControl LoadingProgress::ProcessEvent(
        const RE::TESCellFullyLoadedEvent* a_event,
        RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*)
    {
        if (!hooksEnabled.load(std::memory_order_acquire)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        try {
            if (CellTransitioner::renderObservationState.load(std::memory_order_acquire) != 0 && a_event &&
                a_event->cell) {
                const auto* editorID = a_event->cell->GetFormEditorID();
                if (Settings::GetSingleton().IsLoadingLoggingEnabled()) {
                    logger::info(
                        "cell fully loaded: formID={:08X} editorID='{}' menuOpen={} liveRemaining={}",
                        a_event->cell->GetFormID(), editorID ? editorID : "",
                        epochActive.load(std::memory_order_acquire), GetLiveRemaining());
                }
            }
        } catch (const std::exception& error) {
            DisableHooks(error.what());
        } catch (...) {
            DisableHooks("unknown exception in cell-loaded event sink");
        }

        return RE::BSEventNotifyControl::kContinue;
    }
    // Resets all counters and starts a new loading epoch.
    void LoadingProgress::Aggregator::Begin()
    {
        remaining_.fill(0);
        total_.fill(0);
        progress_ = {};
        active_ = true;
    }

    // Adds one unit of work to a tracked queue.
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

    // Completes one unit of work, including work first seen at completion.
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

    // Returns the most recently calculated aggregate progress.
    LoadingProgress::Progress LoadingProgress::Aggregator::Current() const { return progress_; }

    // Stops accepting queue mutations for the current epoch.
    void LoadingProgress::Aggregator::End() { active_ = false; }

    // Installs loading progress hooks and registers the singleton event sink.
    void InstallHooks()
    {
        auto& events = LoadingProgress::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        auto* eventSources = RE::ScriptEventSourceHolder::GetSingleton();
        if (!ui || !eventSources) {
            throw std::runtime_error("could not find Skyrim's UI or script event source holder");
        }

        InstallMutationHooks();
        InstallIOTaskHooks();
        InstallPostProcessingHooks();
        InstallLoadedEntryHooks();
        InstallLoadingMenuHook();

        ui->AddEventSink<RE::MenuOpenCloseEvent>(&events);
        eventSources->AddEventSink<RE::TESCellFullyLoadedEvent>(&events);
        LoadingProgress::hooksEnabled.store(true, std::memory_order_release);

        logger::info("installed loading-menu and cell-fully-loaded event sinks");
    }
}
