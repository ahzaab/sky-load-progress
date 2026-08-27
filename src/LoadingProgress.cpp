// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#include "PCH.h"
#include "CellTransitioner.h"
#include "IdsAndOffsets.h"
#include "LoadingProgress.h"

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

// Writes a numeric member to an ActionScript object.
void LoadingProgress::SetNumber(RE::GFxValue& a_object, const char* a_name, double a_value)
{
    RE::GFxValue value;
    value.SetNumber(a_value);
    a_object.SetMember(a_name, value);
}

// Reads a numeric member from an ActionScript object.
bool LoadingProgress::GetNumber(const RE::GFxValue& a_object, const char* a_name, double& a_value)
{
    RE::GFxValue value;
    if (!a_object.GetMember(a_name, &value) || !value.IsNumber()) {
        return false;
    }
    a_value = value.GetNumber();
    return true;
}

// Duplicates the loading screen's level meter for queue progress.
bool LoadingProgress::CreateProgressBar(RE::GFxMovieView* a_view)
{
    RE::GFxValue meterRect;
    RE::GFxValue source;
    if (!a_view->GetVariable(&meterRect, "_root.Menu_mc.LevelMeterRect") || !meterRect.IsObject() ||
        !meterRect.GetMember("LevelProgressBar", &source) || !source.IsObject()) {
        logger::warn("could not find _root.Menu_mc.LevelMeterRect.LevelProgressBar");
        return false;
    }

    // Duplicate the active skin instead of supplying our own meter artwork.
    RE::GFxValue args[2];
    args[0].SetString("SkyrimLoadProgress");
    meterRect.Invoke("getNextHighestDepth", &args[1], nullptr, 0);
    RE::GFxValue ignored;
    if (!source.Invoke("duplicateMovieClip", &ignored, args, 2)) {
        logger::warn("LevelProgressBar.duplicateMovieClip failed");
        return false;
    }

    RE::GFxValue progressBar;
    if (!meterRect.GetMember("SkyrimLoadProgress", &progressBar) || !progressBar.IsObject()) {
        logger::warn("duplicated level progress bar was not addressable");
        return false;
    }

    double x = 0.0;
    double y = 0.0;
    double height = 0.0;
    GetNumber(source, "_x", x);
    GetNumber(source, "_y", y);
    GetNumber(source, "_height", height);
    SetNumber(progressBar, "_x", x);
    SetNumber(progressBar, "_y", y + height + 6.0);

    RE::GFxValue frameArgument;
    frameArgument.SetString("Empty");
    progressBar.Invoke("gotoAndStop", &ignored, &frameArgument, 1);
    double emptyFrame = 1.0;
    GetNumber(progressBar, "_currentframe", emptyFrame);
    frameArgument.SetString("Full");
    progressBar.Invoke("gotoAndStop", &ignored, &frameArgument, 1);
    double fullFrame = emptyFrame;
    GetNumber(progressBar, "_currentframe", fullFrame);
    // Some loading menu skins place Full before Empty on the timeline.
    SetNumber(progressBar, "_slpEmptyFrame", emptyFrame);
    SetNumber(progressBar, "_slpFullFrame", fullFrame);

    logger::info(
        "duplicated Loading Menu level meter at ({:.1f}, {:.1f}); height={:.1f}, frames empty={:.0f} full={:.0f}",
        x, y + height + 6.0, height, emptyFrame, fullFrame);
    return true;
}

// Maps a percentage onto the duplicated meter's labeled timeline frames.
bool LoadingProgress::SetMeterPercent(RE::GFxValue& a_meter, double a_percent)
{
    double emptyFrame = 0.0;
    double fullFrame = 0.0;
    if (!GetNumber(a_meter, "_slpEmptyFrame", emptyFrame) ||
        !GetNumber(a_meter, "_slpFullFrame", fullFrame) || fullFrame == emptyFrame) {
        return false;
    }

    const auto percent = std::clamp(a_percent, 0.0, 100.0);
    const auto frame = std::floor(emptyFrame + (fullFrame - emptyFrame) * percent / 100.0);
    RE::GFxValue ignored;
    RE::GFxValue argument;
    argument.SetNumber(frame);
    return a_meter.Invoke("gotoAndStop", &ignored, &argument, 1);
}

// Creates the progress meter on demand and advances it monotonically.
void LoadingProgress::UpdateProgressBar(RE::IMenu* a_menu)
{
    if (!a_menu || !a_menu->uiMovie) {
        return;
    }

    RE::GFxValue progressBar;
    if (!a_menu->uiMovie->GetVariable(
            &progressBar, "_root.Menu_mc.LevelMeterRect.SkyrimLoadProgress") ||
        !progressBar.IsObject()) {
        if (!CreateProgressBar(a_menu->uiMovie.get())) {
            return;
        }
        if (!a_menu->uiMovie->GetVariable(
                &progressBar, "_root.Menu_mc.LevelMeterRect.SkyrimLoadProgress") ||
            !progressBar.IsObject()) {
            return;
        }
    }

    const auto basisPoints = displayedBasisPoints.load(std::memory_order_acquire);
    if (!SetMeterPercent(progressBar, static_cast<double>(basisPoints) / 100.0)) {
        static bool warned = false;
        if (!warned) {
            logger::warn("duplicated level meter has invalid Empty/Full frame labels");
            warned = true;
        }
    }
}

// Updates the progress widget and hides Scaleform for warm transitions.
void LoadingProgress::LoadingMenuAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
{
    originalAdvanceMovie(a_menu, a_interval, a_currentTime);
    if (a_menu && a_menu->uiMovie) {
        const bool seamless = CellTransitioner::IsSeamless();
        a_menu->uiMovie->SetBackgroundAlpha(0.0F);
        a_menu->uiMovie->SetVisible(!seamless);
        if (!seamless) {
            UpdateProgressBar(a_menu);
        }
    }
}

// Locks the source frame and configures the selected presentation when the menu opens.
RE::UI_MESSAGE_RESULTS LoadingProgress::LoadingMenuProcessMessage(RE::IMenu* a_menu, RE::UIMessage& a_message)
{
    if (a_message.type == RE::UI_MESSAGE_TYPE::kShow) {
        CellTransitioner::PrepareForLoad(a_menu);
    }
    return originalLoadingProcessMessage(a_menu, a_message);
}


    void LoadingProgress::LogProgress(const Progress& a_progress)
    {
        const auto candidate = a_progress.total ?
            static_cast<std::uint32_t>(std::clamp(a_progress.fraction * 10000.0, 0.0, 10000.0)) : 0u;
        auto displayed = displayedBasisPoints.load(std::memory_order_relaxed);
        // Newly queued work can lower the raw fraction, but the meter should not move backward.
        while (candidate > displayed &&
            !displayedBasisPoints.compare_exchange_weak(
                displayed, candidate, std::memory_order_release, std::memory_order_relaxed)) {}
        if (a_progress.total == lastLogged.total && a_progress.completed == lastLogged.completed &&
            a_progress.remaining == lastLogged.remaining) {
            return;
        }
        logger::info("load progress completed={} remaining={} total={} percent={:.1f}",
            a_progress.completed, a_progress.remaining, a_progress.total, a_progress.fraction * 100.0);
        lastLogged = a_progress;
    }
    
    // Records a direct queue increment and adds it to the active epoch.
    void LoadingProgress::OnEnqueue(Queue a_queue)
    {
        liveRemaining[static_cast<std::size_t>(a_queue)].fetch_add(1, std::memory_order_relaxed);
        if (!epochActive.load(std::memory_order_acquire)) {
            return;
        }
        std::scoped_lock lock(stateLock);
        aggregator.Enqueue(a_queue);
        LogProgress(aggregator.Current());
    }
    
    // Records a direct queue decrement and completes it in the active epoch.
    void LoadingProgress::OnComplete(Queue a_queue)
    {
        auto& live = liveRemaining[static_cast<std::size_t>(a_queue)];
        auto value = live.load(std::memory_order_relaxed);
        while (value != 0 && !live.compare_exchange_weak(value, value - 1, std::memory_order_relaxed)) {}
        if (!epochActive.load(std::memory_order_acquire)) {
            return;
        }
        std::scoped_lock lock(stateLock);
        aggregator.Complete(a_queue);
        LogProgress(aggregator.Current());
    }
    
    // Context-hook adapters identify which engine queue changed.
    void LoadingProgress::CriticalEnqueue(CONTEXT&) { OnEnqueue(Queue::criticalReferences); }
    void LoadingProgress::CriticalComplete(CONTEXT&) { OnComplete(Queue::criticalReferences); }
    void LoadingProgress::ReferenceEnqueue(CONTEXT&) { OnEnqueue(Queue::references); }
    void LoadingProgress::ReferenceComplete(CONTEXT&) { OnComplete(Queue::references); }
    void LoadingProgress::DistantEnqueue(CONTEXT&) { OnEnqueue(Queue::distantReferences); }
    void LoadingProgress::DistantComplete(CONTEXT&) { OnComplete(Queue::distantReferences); }
    
    // Installs direct hooks on the three decoded reference queue mutations.
    void InstallMutationHooks()
    {
        struct MutationHook
        {
            REL::ID id;
            std::ptrdiff_t offset;
            void (*callback)(CONTEXT&);
            std::string_view name;
        };

        const auto mutationHooks = std::to_array<MutationHook>({
            { IDs::CriticalReferencesEnqueue, Offsets::CriticalReferencesEnqueue,
                LoadingProgress::CriticalEnqueue, "critical enqueue" },
            { IDs::CriticalReferencesComplete, Offsets::CriticalReferencesComplete,
                LoadingProgress::CriticalComplete, "critical completion" },
            { IDs::ReferencesEnqueue, Offsets::ReferencesEnqueue,
                LoadingProgress::ReferenceEnqueue, "reference enqueue" },
            { IDs::ReferencesComplete, Offsets::ReferencesComplete,
                LoadingProgress::ReferenceComplete, "reference completion" },
            { IDs::DistantReferencesEnqueue, Offsets::DistantReferencesEnqueue,
                LoadingProgress::DistantEnqueue, "distant enqueue" },
            { IDs::DistantReferencesComplete, Offsets::DistantReferencesComplete,
                LoadingProgress::DistantComplete, "distant completion" }
        });

        // Context hooks need enough trampoline storage for six generated stubs.
        SKSE::AllocTrampoline(4096);
        constexpr std::size_t counterInstructionSize = 7;
    
        for (const auto& hook : mutationHooks) {
            const auto address = REL::Relocation<std::uintptr_t>(hook.id).address() + hook.offset;
            // Each target is a seven-byte lock inc/dec instruction.
            if (!SKSE::stl::install_context_hook(
                    address, counterInstructionSize, hook.callback, counterInstructionSize)) {
                throw std::runtime_error(fmt::format("could not install {} hook at {:X}", hook.name, address));
            }
            logger::info("installed direct {} hook at {:X}", hook.name, address);
        }
    }

    // Installs the LoadingMenu message and movie-advance hooks.
    void InstallLoadingMenuHook()
    {
        // CommonLib's IMenu vtable maps slot 4 to ProcessMessage and slot 5 to AdvanceMovie.
        constexpr std::size_t processMessageIndex = 0x04;
        constexpr std::size_t advanceMovieIndex = 0x05;

        REL::Relocation<std::uintptr_t> vtable{ RE::LoadingMenu::VTABLE[0] };
        const auto originalProcess =
            vtable.write_vfunc(processMessageIndex, LoadingProgress::LoadingMenuProcessMessage);
        LoadingProgress::originalLoadingProcessMessage =
            reinterpret_cast<LoadingProgress::ProcessMessage_t>(originalProcess);

        const auto original = vtable.write_vfunc(advanceMovieIndex, LoadingProgress::LoadingMenuAdvanceMovie);
        LoadingProgress::originalAdvanceMovie = reinterpret_cast<LoadingProgress::AdvanceMovie_t>(original);

        logger::info("installed hidden LoadingMenu::AdvanceMovie experiment hook");
    }

void LoadingProgress::SeedQueuedWork()
{
    for (std::size_t i = 0; i < queueCount; ++i) {
        const auto baseline = liveRemaining[i].load(std::memory_order_relaxed);

        for (std::uint64_t n = 0; n < baseline; ++n) {
            aggregator.Enqueue(static_cast<Queue>(i));
        }
    }
}

// Starts aggregation when Skyrim opens LoadingMenu.
void LoadingProgress::BeginLoadingEpoch()
{
    std::scoped_lock lock(stateLock);

    displayedBasisPoints.store(0, std::memory_order_release);

    aggregator.Begin();
    SeedQueuedWork();

    lastLogged = {};
    epochActive.store(true, std::memory_order_release);
    CellTransitioner::BeginLoad();

    logger::info(
        "loading epoch began: Loading Menu opened; baseline remaining={}", aggregator.Current().remaining);
    LogProgress(aggregator.Current());
}

// Ends aggregation and starts the retained-frame transition into gameplay.
void LoadingProgress::EndLoadingEpoch()
{
    std::scoped_lock lock(stateLock);

    epochActive.store(false, std::memory_order_release);
    CellTransitioner::EndLoad();

    const auto final = aggregator.Current();
    logger::info("loading epoch ended: Loading Menu closed; completed={} remaining={} total={}",
        final.completed, final.remaining, final.total);
    aggregator.End();

}

// Starts or ends an aggregation epoch with LoadingMenu's lifetime.
RE::BSEventNotifyControl LoadingProgress::ProcessEvent(
    const RE::MenuOpenCloseEvent* a_event,
    RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
    if (!a_event || a_event->menuName != RE::LoadingMenu::MENU_NAME) {
        return RE::BSEventNotifyControl::kContinue;
    }

    a_event->opening ? BeginLoadingEpoch() : EndLoadingEpoch();

    return RE::BSEventNotifyControl::kContinue;
}

// Logs the fully-loaded milestone while a transition is being observed.
RE::BSEventNotifyControl LoadingProgress::ProcessEvent(
    const RE::TESCellFullyLoadedEvent* a_event,
    RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*)
{
    if (CellTransitioner::renderObservationState.load(std::memory_order_acquire) != 0 && a_event &&
        a_event->cell) {
        logger::info("cell fully loaded: formID={:08X} editorID='{}' menuOpen={} liveRemaining={}",
            a_event->cell->GetFormID(), a_event->cell->GetFormEditorID(),
            epochActive.load(std::memory_order_acquire), GetLiveRemaining());
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
void LoadingProgress::Aggregator::Enqueue(LoadingProgress::Queue a_queue)
{
    if (!active_) {
        return;
    }
    const auto index = static_cast<std::size_t>(a_queue);
    ++remaining_[index];
    ++total_[index];
    logger::debug("queue '{}' enqueued one item", queueNames[index]);
    Recalculate();
}

// Completes one unit of work, including work first seen at completion.
void LoadingProgress::Aggregator::Complete(LoadingProgress::Queue a_queue)
{
    if (!active_) {
        return;
    }
    const auto index = static_cast<std::size_t>(a_queue);
    if (remaining_[index] == 0) {
        ++total_[index];
        logger::debug("queue '{}' completed an unobserved item", queueNames[index]);
    } else {
        --remaining_[index];
        logger::debug("queue '{}' completed one item", queueNames[index]);
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
        static_cast<double>(progress_.completed) / static_cast<double>(progress_.total) : 0.0;
}

// Returns the most recently calculated aggregate progress.
LoadingProgress::Progress LoadingProgress::Aggregator::Current() const { return progress_; }

// Stops accepting queue mutations for the current epoch.
void LoadingProgress::Aggregator::End() { active_ = false; }

// Installs loading progress hooks and registers the singleton event sink.
void InstallHooks()
{
    InstallMutationHooks();
    InstallLoadingMenuHook();

    auto& events = LoadingProgress::GetSingleton();
    RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&events);
    RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESCellFullyLoadedEvent>(&events);

    logger::info("installed loading-menu and cell-fully-loaded event sinks");
}
}
