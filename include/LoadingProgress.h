// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

namespace load_progress
{
// LoadingProgress injects the queue meter directly into LoadingMenu's existing Scaleform movie.
// LoadingMenu::AdvanceMovie first looks for _root.Menu_mc.LevelMeterRect.SkyrimLoadProgress.
// When it is missing, CreateProgressBar duplicates LevelProgressBar at the next available depth so
// the new meter inherits whichever level-bar skin is currently installed. The copy is positioned
// below the original level meter, and its Empty and Full timeline frames are recorded on the clip.
// Each update converts the monotonic aggregate percentage into a frame between those two labels and
// calls gotoAndStop. Nothing is added to the SWF on disk; the clip exists only in the active movie.
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

    struct Progress
    {
        std::uint64_t total{};
        std::uint64_t remaining{};
        std::uint64_t completed{};
        double fraction{};
    };

    class Aggregator
    {
    public:
        void Begin();
        void Enqueue(Queue);
        void Complete(Queue);
        [[nodiscard]] Progress Current() const;
        void End();

    private:
        void Recalculate();

        std::array<std::uint64_t, static_cast<std::size_t>(Queue::count)> remaining_{};
        std::array<std::uint64_t, static_cast<std::size_t>(Queue::count)> total_{};
        Progress progress_{};
        bool active_{};
    };

    using AdvanceMovie_t = void (*)(RE::IMenu*, float, std::uint32_t);
    using ProcessMessage_t = RE::UI_MESSAGE_RESULTS (*)(RE::IMenu*, RE::UIMessage&);

    static LoadingProgress& GetSingleton();
    static std::uint64_t GetLiveRemaining();
    static void SetNumber(RE::GFxValue&, const char*, double);
    static bool GetNumber(const RE::GFxValue&, const char*, double&);
    static bool CreateProgressBar(RE::GFxMovieView*);
    static bool SetMeterPercent(RE::GFxValue&, double);
    static void UpdateProgressBar(RE::IMenu*);
    static void LoadingMenuAdvanceMovie(RE::IMenu*, float, std::uint32_t);
    static RE::UI_MESSAGE_RESULTS LoadingMenuProcessMessage(RE::IMenu*, RE::UIMessage&);
    static void LogProgress(const Progress&);
    static void OnEnqueue(Queue);
    static void OnComplete(Queue);
    static void CriticalEnqueue(CONTEXT&);
    static void CriticalComplete(CONTEXT&);
    static void ReferenceEnqueue(CONTEXT&);
    static void ReferenceComplete(CONTEXT&);
    static void DistantEnqueue(CONTEXT&);
    static void DistantComplete(CONTEXT&);
    static void SeedQueuedWork();
    static void BeginLoadingEpoch();
    static void EndLoadingEpoch();

    RE::BSEventNotifyControl ProcessEvent(
        const RE::MenuOpenCloseEvent*, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESCellFullyLoadedEvent*, RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override;

    static constexpr std::size_t queueCount = static_cast<std::size_t>(Queue::count);
    inline static constexpr auto queueNames = std::to_array<std::string_view>(
        { "critical-refs", "refs", "distant-refs", "background", "tasks", "post-processing" });
    inline static std::array<std::atomic_uint64_t, queueCount> liveRemaining{};
    inline static Aggregator aggregator;
    inline static std::atomic_bool epochActive{ false };
    inline static std::atomic_uint32_t displayedBasisPoints{};
    inline static std::mutex stateLock;
    inline static Progress lastLogged{};
    inline static ProcessMessage_t originalLoadingProcessMessage{};
    inline static AdvanceMovie_t originalAdvanceMovie{};

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
