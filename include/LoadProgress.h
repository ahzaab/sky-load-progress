#pragma once

namespace load_progress
{
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

    struct Snapshot
    {
        std::array<std::uint64_t, static_cast<std::size_t>(Queue::count)> remaining{};
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
        [[nodiscard]] Progress Observe(const Snapshot& a_snapshot);
        [[nodiscard]] Progress Current() const;
        void End();

    private:
        std::array<std::uint64_t, static_cast<std::size_t>(Queue::count)> previous_{};
        std::array<std::uint64_t, static_cast<std::size_t>(Queue::count)> discovered_{};
        Progress progress_{};
        bool active_{};
    };

    void Install();
}

