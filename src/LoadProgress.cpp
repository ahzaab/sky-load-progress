#include "PCH.h"
#include "LoadProgress.h"

namespace load_progress
{
    namespace
    {
        constexpr auto queueNames = std::to_array<std::string_view>({
            "critical-refs", "refs", "distant-refs", "background", "tasks", "post-processing"
        });

        constexpr std::size_t queueCount = static_cast<std::size_t>(Queue::count);
        std::array<std::atomic_uint64_t, queueCount> liveRemaining{};
        Aggregator aggregator;
        std::atomic_bool epochActive{ false };
        std::atomic_uint32_t displayedBasisPoints{};
        std::mutex stateLock;
        Progress lastLogged{};

        using AdvanceMovie_t = void (*)(RE::IMenu*, float, std::uint32_t);
        AdvanceMovie_t originalAdvanceMovie{};

        void SetNumber(RE::GFxValue& a_object, const char* a_name, double a_value)
        {
            RE::GFxValue value;
            value.SetNumber(a_value);
            a_object.SetMember(a_name, value);
        }

        bool DrawRectangle(RE::GFxValue& a_clip, double a_width, double a_height,
            std::uint32_t a_color, double a_alpha)
        {
            RE::GFxValue ignored;
            RE::GFxValue fillArgs[2];
            fillArgs[0].SetNumber(a_color);
            fillArgs[1].SetNumber(a_alpha);
            if (!a_clip.Invoke("beginFill", &ignored, fillArgs, 2)) {
                return false;
            }

            const std::array<std::pair<double, double>, 5> points{{
                { 0.0, 0.0 }, { a_width, 0.0 }, { a_width, a_height }, { 0.0, a_height }, { 0.0, 0.0 }
            }};
            for (std::size_t i = 0; i < points.size(); ++i) {
                RE::GFxValue args[2];
                args[0].SetNumber(points[i].first);
                args[1].SetNumber(points[i].second);
                if (!a_clip.Invoke(i == 0 ? "moveTo" : "lineTo", &ignored, args, 2)) {
                    return false;
                }
            }
            return a_clip.Invoke("endFill", &ignored, nullptr, 0);
        }

        bool CreateProgressBar(RE::GFxMovieView* a_view)
        {
            RE::GFxValue container;
            RE::GFxValue args[2];
            args[0].SetString("SkyrimLoadProgress");
            a_view->Invoke("_root.getNextHighestDepth", &args[1], nullptr, 0);
            if (!a_view->Invoke("_root.createEmptyMovieClip", &container, args, 2) || !container.IsObject()) {
                return false;
            }

            constexpr double width = 600.0;
            constexpr double height = 12.0;
            SetNumber(container, "_x", 340.0);
            SetNumber(container, "_y", 650.0);

            RE::GFxValue background;
            args[0].SetString("Background");
            container.Invoke("getNextHighestDepth", &args[1], nullptr, 0);
            if (!container.Invoke("createEmptyMovieClip", &background, args, 2) || !background.IsObject() ||
                !DrawRectangle(background, width, height, 0x101010, 75.0)) {
                return false;
            }

            RE::GFxValue fill;
            args[0].SetString("Fill");
            container.Invoke("getNextHighestDepth", &args[1], nullptr, 0);
            if (!container.Invoke("createEmptyMovieClip", &fill, args, 2) || !fill.IsObject() ||
                !DrawRectangle(fill, width, height, 0x8BAF50, 100.0)) {
                return false;
            }
            SetNumber(fill, "_xscale", 0.0);
            logger::info("injected loading progress bar into Loading Menu");
            return true;
        }

        void UpdateProgressBar(RE::IMenu* a_menu)
        {
            if (!a_menu || !a_menu->uiMovie) {
                return;
            }

            RE::GFxValue container;
            if (!a_menu->uiMovie->GetVariable(&container, "_root.SkyrimLoadProgress") || !container.IsObject()) {
                if (!CreateProgressBar(a_menu->uiMovie.get())) {
                    return;
                }
                if (!a_menu->uiMovie->GetVariable(&container, "_root.SkyrimLoadProgress") || !container.IsObject()) {
                    return;
                }
            }

            RE::GFxValue fill;
            if (container.GetMember("Fill", &fill) && fill.IsObject()) {
                const auto basisPoints = displayedBasisPoints.load(std::memory_order_acquire);
                SetNumber(fill, "_xscale", static_cast<double>(basisPoints) / 100.0);
            }
        }

        void LoadingMenuAdvanceMovie(RE::IMenu* a_menu, float a_interval, std::uint32_t a_currentTime)
        {
            originalAdvanceMovie(a_menu, a_interval, a_currentTime);
            UpdateProgressBar(a_menu);
        }

        void InstallLoadingMenuHook()
        {
            constexpr std::size_t advanceMovieIndex = 0x05;
            REL::Relocation<std::uintptr_t> vtable{ RE::LoadingMenu::VTABLE[0] };
            const auto original = vtable.write_vfunc(advanceMovieIndex, LoadingMenuAdvanceMovie);
            originalAdvanceMovie = reinterpret_cast<AdvanceMovie_t>(original);
            logger::info("installed LoadingMenu::AdvanceMovie presentation hook");
        }

        void LogProgress(const Progress& a_progress)
        {
            const auto candidate = a_progress.total ?
                static_cast<std::uint32_t>(std::clamp(a_progress.fraction * 10000.0, 0.0, 10000.0)) : 0u;
            auto displayed = displayedBasisPoints.load(std::memory_order_relaxed);
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

        void OnEnqueue(Queue a_queue)
        {
            liveRemaining[static_cast<std::size_t>(a_queue)].fetch_add(1, std::memory_order_relaxed);
            if (!epochActive.load(std::memory_order_acquire)) {
                return;
            }
            std::scoped_lock lock(stateLock);
            aggregator.Enqueue(a_queue);
            LogProgress(aggregator.Current());
        }

        void OnComplete(Queue a_queue)
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

        void CriticalEnqueue(CONTEXT&) { OnEnqueue(Queue::criticalReferences); }
        void CriticalComplete(CONTEXT&) { OnComplete(Queue::criticalReferences); }
        void ReferenceEnqueue(CONTEXT&) { OnEnqueue(Queue::references); }
        void ReferenceComplete(CONTEXT&) { OnComplete(Queue::references); }
        void DistantEnqueue(CONTEXT&) { OnEnqueue(Queue::distantReferences); }
        void DistantComplete(CONTEXT&) { OnComplete(Queue::distantReferences); }

        struct MutationHook
        {
            REL::ID id;
            std::ptrdiff_t offset;
            void (*callback)(CONTEXT&);
            std::string_view name;
        };

        const auto mutationHooks = std::to_array<MutationHook>({
            { REL::ID(19155), 0x07, CriticalEnqueue, "critical enqueue" },
            { REL::ID(19156), 0x0C, CriticalComplete, "critical completion" },
            { REL::ID(19151), 0x07, ReferenceEnqueue, "reference enqueue" },
            { REL::ID(19152), 0x0C, ReferenceComplete, "reference completion" },
            { REL::ID(19159), 0x4E, DistantEnqueue, "distant enqueue" },
            { REL::ID(19160), 0x69, DistantComplete, "distant completion" }
        });

        void InstallMutationHooks()
        {
            SKSE::AllocTrampoline(4096);
            for (const auto& hook : mutationHooks) {
                const auto address = REL::Relocation<std::uintptr_t>(hook.id).address() + hook.offset;
                if (!SKSE::stl::install_context_hook(address, 7, hook.callback, 7)) {
                    throw std::runtime_error(fmt::format("could not install {} hook at {:X}", hook.name, address));
                }
                logger::info("installed direct {} hook at {:X}", hook.name, address);
            }
        }

        class Events final :
            public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
            public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
        {
        public:
            static Events& GetSingleton()
            {
                static Events singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* a_event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!a_event || a_event->menuName != RE::LoadingMenu::MENU_NAME) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                if (a_event->opening) {
                    std::scoped_lock lock(stateLock);
                    displayedBasisPoints.store(0, std::memory_order_release);
                    aggregator.Begin();
                    for (std::size_t i = 0; i < queueCount; ++i) {
                        const auto baseline = liveRemaining[i].load(std::memory_order_relaxed);
                        for (std::uint64_t n = 0; n < baseline; ++n) {
                            aggregator.Enqueue(static_cast<Queue>(i));
                        }
                    }
                    lastLogged = {};
                    epochActive.store(true, std::memory_order_release);
                    logger::info("loading epoch began: Loading Menu opened; baseline remaining={}",
                        aggregator.Current().remaining);
                    LogProgress(aggregator.Current());
                } else {
                    std::scoped_lock lock(stateLock);
                    epochActive.store(false, std::memory_order_release);
                    const auto final = aggregator.Current();
                    logger::info("loading epoch ended: Loading Menu closed; completed={} remaining={} total={}",
                        final.completed, final.remaining, final.total);
                    aggregator.End();
                }
                return RE::BSEventNotifyControl::kContinue;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESCellFullyLoadedEvent* a_event,
                RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
            {
                if (epochActive.load(std::memory_order_acquire) && a_event && a_event->cell) {
                    logger::info("cell fully loaded: formID={:08X} editorID='{}'",
                        a_event->cell->GetFormID(), a_event->cell->GetFormEditorID());
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Aggregator::Begin()
    {
        remaining_.fill(0);
        total_.fill(0);
        progress_ = {};
        active_ = true;
    }

    void Aggregator::Enqueue(Queue a_queue)
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

    void Aggregator::Complete(Queue a_queue)
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

    void Aggregator::Recalculate()
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

    Progress Aggregator::Current() const { return progress_; }
    void Aggregator::End() { active_ = false; }

    void Install()
    {
        InstallMutationHooks();
        InstallLoadingMenuHook();
        auto& events = Events::GetSingleton();
        RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&events);
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESCellFullyLoadedEvent>(&events);
        logger::info("installed loading-menu and cell-fully-loaded event sinks");
    }
}
