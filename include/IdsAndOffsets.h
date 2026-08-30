// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

#include <cstddef>

#include <REL/Relocation.h>

namespace load_progress
{
    namespace Runtimes
    {
        inline constexpr REL::Version SkyrimAEStart{ 1, 6, 0, 0 };
        inline constexpr REL::Version Skyrim17Start{ 1, 7, 99, 0 };
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

        // These callers own the semantic enqueue operations used by loaded-entry diagnostics. The
        // hook installer finds their unique calls to the counter helpers above, so these sites do not
        // depend on fragile function-relative offsets.
        constexpr REL::RelocationID ObjectReferenceQueueCaller{ 12910, 13057 };
        constexpr REL::RelocationID TransferredReferenceQueueCaller{ 19391, 19818 };
        constexpr REL::RelocationID DistantReferenceQueueCaller{ 17811, 18223 };

        // Main::DrawWorld owns the call that tells Skyrim to render the normal world scene. The
        // caller/callee pair lets the hook locate that call without a runtime-specific byte offset.
        constexpr REL::RelocationID NormalWorldRenderCaller{ 35560, 36559 };
        constexpr REL::RelocationID NormalWorldRenderer{ 100424, 107142 };

        // The UI renderer owns one call to this helper after binding the Scaleform render target and
        // before drawing any movies. Hooking that specific call preserves the world-only capture point.
        constexpr REL::RelocationID ScaleformRenderCaller{ 79947, 82084 };
        constexpr REL::RelocationID ScaleformBeginHelper{ 80605, 82732 };
    }

    struct RuntimeOffset
    {
        std::ptrdiff_t se;
        std::ptrdiff_t ae;
        std::ptrdiff_t aeGog;

        [[nodiscard]] std::ptrdiff_t Get() const noexcept
        {
            const auto version = REL::Module::get().version();
            if (version < Runtimes::SkyrimAEStart) {
                return se;
            }
            if (version >= Runtimes::Skyrim17Start) {
                return aeGog;
            }
            return ae;
        }
    };

    // Function-relative offsets decoded against 1.5.97, 1.6.1170, and 1.7.99. Other
    // Address Library-supported runtimes use the closest matching runtime family as a
    // best-effort fallback; hook-site validation remains responsible for rejecting bad sites.
    namespace Offsets
    {
        // These queue offsets are identical in all three decoded runtimes and are considered stable.
        constexpr RuntimeOffset CriticalReferencesEnqueue{ 0x07, 0x07, 0x07 };
        constexpr RuntimeOffset CriticalReferencesComplete{ 0x0C, 0x0C, 0x0C };
        constexpr RuntimeOffset ReferencesEnqueue{ 0x07, 0x07, 0x07 };
        constexpr RuntimeOffset ReferencesComplete{ 0x0C, 0x0C, 0x0C };
        constexpr RuntimeOffset DistantReferencesEnqueue{ 0x4E, 0x4E, 0x4E };
        constexpr RuntimeOffset DistantReferencesComplete{ 0x69, 0x69, 0x69 };

        // Verified fallback locations for chaining render hooks that another plugin has already
        // redirected. Vanilla installs still use semantic caller/callee discovery first.
        constexpr RuntimeOffset NormalWorldRenderCall{ 0x831, 0x841, 0x85E };
        constexpr RuntimeOffset ScaleformBeginCall{ 0x17F, 0x18A, 0x18A };
    }
}
