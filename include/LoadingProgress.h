// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

namespace load_progress
{
    // LoadingProgress owns the queue hooks, loading epochs, loaded-reference diagnostics, and aggregate
    // progress calculation. Its LoadingMenu update passes the monotonic result to ProgressMeter, keeping
    // engine instrumentation independent from the Scaleform implementation.
    class LoadingProgress final :
        public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
        public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
    {
    public:
        enum class Queue : std::size_t
        {
            criticalReferences,
            references,
            distantReferences,
            backgroundProcessing,
            tasks,
            postProcessing,
            count
        };

        enum class LoadedEntryType : std::size_t
        {
            objectReference,
            transferredReference,
            distantReference,
            count
        };

        struct LoadedEntry
        {
            LoadedEntryType type{};
            RE::FormID       referenceID{};
            RE::FormID       baseID{};
            RE::FormID       cellID{};
        };

        struct LoadedEntrySlot
        {
            std::atomic_uint8_t state{};
            LoadedEntry        entry{};
        };

        struct Progress
        {
            std::uint64_t total{};
            std::uint64_t remaining{};
            std::uint64_t completed{};
            double        fraction{};
        };

        class Aggregator
        {
        public:
            void                   Begin();
            void                   Enqueue(Queue, std::uint64_t = 1);
            void                   Complete(Queue, std::uint64_t = 1);
            [[nodiscard]] Progress Current() const;
            void                   End();

        private:
            void Recalculate();

            std::array<std::uint64_t, static_cast<std::size_t>(Queue::count)> remaining_{};
            std::array<std::uint64_t, static_cast<std::size_t>(Queue::count)> total_{};
            Progress                                                          progress_{};
            bool                                                              active_{};
        };

        using AdvanceMovie_t = void (*)(RE::IMenu*, float, std::uint32_t);
        using ProcessMessage_t = RE::UI_MESSAGE_RESULTS (*)(RE::IMenu*, RE::UIMessage&);
        using ReferenceEnqueue_t = std::uintptr_t (*)(RE::TESObjectCELL*);
        using DistantReferenceEnqueue_t = std::uintptr_t (*)(RE::TESObjectCELL*, RE::TESObjectREFR*);

        static LoadingProgress& GetSingleton();
        static std::uint64_t    GetLiveRemaining();

        static void                   DrainQueueMutations();
        static void                   DrainLoadedEntries(bool);
        static void                   BeginLoadedEntryCapture();
        static void                   EndLoadedEntryCapture();
        static void                   WaitForLoadedEntryWriters();
        static bool                   IsLoadedEntryTypeEnabled(LoadedEntryType) noexcept;
        static bool                   TryStoreLoadedEntry(const LoadedEntry&) noexcept;
        static void                   CaptureLoadedEntry(LoadedEntryType, RE::TESObjectREFR*, RE::TESObjectCELL*) noexcept;
        static void                   ObjectReferenceQueued(CONTEXT&) noexcept;
        static void                   TransferredReferenceQueued(CONTEXT&) noexcept;
        static void                   DistantReferenceQueued(CONTEXT&) noexcept;
        static void                   LoadingMenuAdvanceMovie(RE::IMenu*, float, std::uint32_t);
        static RE::UI_MESSAGE_RESULTS LoadingMenuProcessMessage(RE::IMenu*, RE::UIMessage&);
        static void                   LogProgress(const Progress&);
        static void                   DisableHooks(std::string_view) noexcept;
        static void                   OnEnqueue(Queue) noexcept;
        static void                   OnComplete(Queue) noexcept;
        static void                   CriticalEnqueue(CONTEXT&) noexcept;
        static void                   CriticalComplete(CONTEXT&) noexcept;
        static void                   ReferenceEnqueue(CONTEXT&) noexcept;
        static void                   ReferenceComplete(CONTEXT&) noexcept;
        static void                   DistantEnqueue(CONTEXT&) noexcept;
        static void                   DistantComplete(CONTEXT&) noexcept;
        static void                   SeedQueuedWork();
        static void                   BeginLoadingEpoch();
        static void                   EndLoadingEpoch();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent*, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESCellFullyLoadedEvent*, RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override;

        static constexpr std::size_t queueCount = static_cast<std::size_t>(Queue::count);
        static constexpr std::size_t loadedEntryTypeCount = static_cast<std::size_t>(LoadedEntryType::count);
        static constexpr std::size_t loadedEntryCapacity = 16384;
        inline static constexpr auto queueNames = std::to_array<std::string_view>(
            { "critical-refs", "refs", "distant-refs", "background", "tasks", "post-processing" });
        inline static std::array<std::atomic_uint64_t, queueCount> liveRemaining{};
        inline static std::array<std::atomic_uint64_t, queueCount> pendingEnqueued{};
        inline static std::array<std::atomic_uint64_t, queueCount> pendingCompleted{};
        inline static Aggregator                                   aggregator;
        inline static std::atomic_bool                             epochActive{ false };
        inline static std::atomic_bool                             hooksEnabled{ false };
        inline static std::atomic_bool                             failureLogged{ false };
        inline static std::atomic_uint32_t                         displayedBasisPoints{};
        inline static std::array<LoadedEntrySlot, loadedEntryCapacity> loadedEntries{};
        inline static std::array<std::atomic_uint64_t, loadedEntryTypeCount> loadedEntryTallies{};
        inline static std::atomic_uint64_t                         loadedEntryWriteCursor{};
        inline static std::atomic_uint64_t                         droppedLoadedEntryDetails{};
        inline static std::atomic_uint32_t                         loadedEntryWriters{};
        inline static std::atomic_bool                             loadedEntryCaptureActive{};
        inline static std::mutex                                   stateLock;
        inline static Progress                                     lastLogged{};
        inline static ProcessMessage_t                             originalLoadingProcessMessage{};
        inline static AdvanceMovie_t                               originalAdvanceMovie{};
        inline static ReferenceEnqueue_t                           originalReferenceEnqueue{};
        inline static DistantReferenceEnqueue_t                    originalDistantReferenceEnqueue{};

    private:
        LoadingProgress() = default;
        LoadingProgress(const LoadingProgress&) = delete;
        LoadingProgress(LoadingProgress&&) = delete;
        LoadingProgress& operator=(const LoadingProgress&) = delete;
        LoadingProgress& operator=(LoadingProgress&&) = delete;
    };

    // Installs loading progress hooks and event sinks.
    void InstallHooks();
}
