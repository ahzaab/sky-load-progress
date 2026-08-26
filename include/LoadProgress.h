// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

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
        void Enqueue(Queue a_queue);
        void Complete(Queue a_queue);
        [[nodiscard]] Progress Current() const;
        void End();

    private:
        void Recalculate();

        std::array<std::uint64_t, static_cast<std::size_t>(Queue::count)> remaining_{};
        std::array<std::uint64_t, static_cast<std::size_t>(Queue::count)> total_{};
        Progress progress_{};
        bool active_{};
    };

    void Install();
}
