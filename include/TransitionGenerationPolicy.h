#pragma once

namespace load_progress
{
    // Render-thread state. A completed retained-image frame must never seed FG.
    class TransitionGenerationPolicy
    {
    public:
        bool Begin(bool enabled, bool ownsImage, bool freshWorld) noexcept
        {
            if (!enabled) {
                remaining = 0;
            } else if (ownsImage) {
                remaining = warmupFrames;
            } else if (remaining && !freshWorld) {
                remaining = warmupFrames;
            }
            countAfterPresent = enabled && !ownsImage && freshWorld && remaining;
            return enabled && (ownsImage || remaining);
        }

        void Finish(bool presented) noexcept
        {
            if (countAfterPresent && presented) {
                --remaining;
            } else if (countAfterPresent) {
                remaining = warmupFrames;
            }
            countAfterPresent = false;
        }

    private:
        static constexpr unsigned warmupFrames = 3;
        unsigned remaining{};
        bool countAfterPresent{};
    };
}
