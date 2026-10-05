#pragma once

namespace load_progress::CommunityShadersTransition
{
    // Supports the exact CS 1.8.4 HDR/FFX build investigated in IDA; otherwise no patches.
    void Install();
    void BeginPresent(bool enabled, bool ownsImage, bool freshWorld);
    void EndPresent(bool presented) noexcept;
}
