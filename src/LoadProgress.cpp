#include "PCH.h"
#include "LoadProgress.h"

namespace load_progress
{
    namespace
    {
        constexpr auto queueNames = std::to_array<std::string_view>({
            "critical-refs", "refs", "distant-refs", "background", "tasks", "post-processing"
        });

        Aggregator aggregator;
        std::atomic_bool epochActive{ false };
        std::atomic_bool pumpScheduled{ false };
        std::mutex stateLock;
        Progress lastLogged{};

        Snapshot ReadSnapshot()
        {
            Snapshot result;
            const auto* player = RE::PlayerCharacter::GetSingleton();
            const auto* cell = player ? player->GetParentCell() : nullptr;
            if (!cell) {
                return result;
            }

            const auto* loaded = cell->GetRuntimeData().loadedData;
            if (!loaded) {
                return result;
            }

            const auto nonnegative = [](std::int32_t a_value) {
                return static_cast<std::uint64_t>(std::max(a_value, 0));
            };
            result.remaining[static_cast<std::size_t>(Queue::criticalReferences)] = nonnegative(loaded->criticalQueuedRefCount);
            result.remaining[static_cast<std::size_t>(Queue::references)] = nonnegative(loaded->queuedRefCount);
            result.remaining[static_cast<std::size_t>(Queue::distantReferences)] = nonnegative(loaded->queuedDistantRefCount);
            return result;
        }

        void Pump()
        {
            pumpScheduled.store(false);
            if (!epochActive.load()) {
                return;
            }

            const auto snapshot = ReadSnapshot();
            Progress progress;
            {
                std::scoped_lock lock(stateLock);
                progress = aggregator.Observe(snapshot);
                if (progress.total != lastLogged.total || progress.completed != lastLogged.completed ||
                    progress.remaining != lastLogged.remaining) {
                    logger::info("load progress completed={} remaining={} total={} percent={:.1f}",
                        progress.completed, progress.remaining, progress.total, progress.fraction * 100.0);
                    lastLogged = progress;
                }
            }

            if (!pumpScheduled.exchange(true)) {
                SKSE::GetTaskInterface()->AddTask(Pump);
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
                    {
                        std::scoped_lock lock(stateLock);
                        aggregator.Begin();
                        lastLogged = {};
                    }
                    epochActive.store(true);
                    logger::info("loading epoch began: Loading Menu opened");
                    if (!pumpScheduled.exchange(true)) {
                        SKSE::GetTaskInterface()->AddTask(Pump);
                    }
                } else {
                    epochActive.store(false);
                    std::scoped_lock lock(stateLock);
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
                if (epochActive.load() && a_event && a_event->cell) {
                    logger::info("cell fully loaded: formID={:08X} editorID='{}'",
                        a_event->cell->GetFormID(), a_event->cell->GetFormEditorID());
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Aggregator::Begin()
    {
        previous_.fill(0);
        discovered_.fill(0);
        progress_ = {};
        active_ = true;
    }

    Progress Aggregator::Observe(const Snapshot& a_snapshot)
    {
        if (!active_) {
            return progress_;
        }

        std::uint64_t remaining = 0;
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < a_snapshot.remaining.size(); ++i) {
            const auto current = a_snapshot.remaining[i];
            if (current > previous_[i]) {
                const auto added = current - previous_[i];
                discovered_[i] += added;
                logger::debug("queue '{}' discovered {} work items", queueNames[i], added);
            }
            previous_[i] = current;
            remaining += current;
            total += discovered_[i];
        }

        progress_.total = total;
        progress_.remaining = std::min(remaining, total);
        progress_.completed = total - progress_.remaining;
        progress_.fraction = total ? static_cast<double>(progress_.completed) / static_cast<double>(total) : 0.0;
        return progress_;
    }

    Progress Aggregator::Current() const { return progress_; }
    void Aggregator::End() { active_ = false; }

    void Install()
    {
        auto& events = Events::GetSingleton();
        RE::UI::GetSingleton()->AddEventSink<RE::MenuOpenCloseEvent>(&events);
        RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink<RE::TESCellFullyLoadedEvent>(&events);
        logger::info("installed loading-menu and cell-fully-loaded event sinks");
    }
}

