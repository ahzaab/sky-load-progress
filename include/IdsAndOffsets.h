// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

#include <cstddef>

#include <REL/Relocation.h>

namespace load_progress
{
    namespace Runtimes
    {
        inline constexpr REL::Version SkyrimSE{ 1, 5, 97, 0 };
        inline constexpr REL::Version SkyrimAE{ 1, 6, 1170, 0 };
        inline constexpr REL::Version SkyrimAEGOG{ 1, 7, 99, 0 };

        [[nodiscard]] inline bool IsSupported() noexcept
        {
            const auto version = REL::Module::get().version();
            return version == SkyrimSE || version == SkyrimAE || version == SkyrimAEGOG;
        }
    }

    // Address Library identifiers used as the base of each decoded hook location.
    namespace IDs
    {
        constexpr REL::RelocationID CriticalReferencesEnqueue{ 18675, 19155 };
        constexpr REL::RelocationID CriticalReferencesComplete{ 18676, 19156 };
        constexpr REL::RelocationID ReferencesEnqueue{ 18672, 19151 };
        constexpr REL::RelocationID ReferencesComplete{ 18673, 19152 };
        constexpr REL::RelocationID DistantReferencesEnqueue{ 18677, 19159 };
        constexpr REL::RelocationID DistantReferencesComplete{ 18678, 19160 };

        constexpr REL::RelocationID NormalWorldRenderCaller{ 35560, 36559 };
        constexpr REL::RelocationID ScaleformRenderCaller{ 79947, 82084 };
    }

    struct RuntimeOffset
    {
        std::ptrdiff_t se;
        std::ptrdiff_t ae;
        std::ptrdiff_t aeGog;

        [[nodiscard]] std::ptrdiff_t Get() const noexcept
        {
            const auto version = REL::Module::get().version();
            if (version == Runtimes::SkyrimSE) {
                return se;
            }
            if (version == Runtimes::SkyrimAE) {
                return ae;
            }
            if (version == Runtimes::SkyrimAEGOG) {
                return aeGog;
            }
            return 0;
        }
    };

    // Function-relative offsets decoded against 1.5.97, 1.6.1170, and 1.7.99.
    namespace Offsets
    {
        constexpr RuntimeOffset CriticalReferencesEnqueue{ 0x07, 0x07, 0x07 };
        constexpr RuntimeOffset CriticalReferencesComplete{ 0x0C, 0x0C, 0x0C };
        constexpr RuntimeOffset ReferencesEnqueue{ 0x07, 0x07, 0x07 };
        constexpr RuntimeOffset ReferencesComplete{ 0x0C, 0x0C, 0x0C };
        constexpr RuntimeOffset DistantReferencesEnqueue{ 0x4E, 0x4E, 0x4E };
        constexpr RuntimeOffset DistantReferencesComplete{ 0x69, 0x69, 0x69 };

        // Main::DrawWorld call to the normal world renderer.
        constexpr RuntimeOffset NormalWorldRenderCall{ 0x831, 0x841, 0x85E };

        // UI render call made after Scaleform binds its render target.
        constexpr RuntimeOffset ScaleformBeginCall{ 0x17F, 0x18A, 0x18A };
    }
}
