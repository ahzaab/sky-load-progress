// Skyrim Load Progress
// Copyright (c) 2026 ahzaab

#pragma once

#include <cstddef>

#include <REL/Relocation.h>

namespace load_progress
{
    // Address Library identifiers used as the base of each decoded hook location.
    namespace IDs
    {
        constexpr REL::ID CriticalReferencesEnqueue{ 19155 };
        constexpr REL::ID CriticalReferencesComplete{ 19156 };
        constexpr REL::ID ReferencesEnqueue{ 19151 };
        constexpr REL::ID ReferencesComplete{ 19152 };
        constexpr REL::ID DistantReferencesEnqueue{ 19159 };
        constexpr REL::ID DistantReferencesComplete{ 19160 };

        constexpr REL::ID NormalWorldRenderCaller{ 36559 };
        constexpr REL::ID ScaleformRenderCaller{ 82084 };
    }

    // Function-relative offsets decoded and verified against Skyrim AE 1.7.99.
    namespace Offsets
    {
        constexpr std::ptrdiff_t CriticalReferencesEnqueue{ 0x07 };
        constexpr std::ptrdiff_t CriticalReferencesComplete{ 0x0C };
        constexpr std::ptrdiff_t ReferencesEnqueue{ 0x07 };
        constexpr std::ptrdiff_t ReferencesComplete{ 0x0C };
        constexpr std::ptrdiff_t DistantReferencesEnqueue{ 0x4E };
        constexpr std::ptrdiff_t DistantReferencesComplete{ 0x69 };

        // Main::DrawWorld call to the normal world renderer.
        constexpr std::ptrdiff_t NormalWorldRenderCall{ 0x85E };

        // UI render call made after Scaleform binds its render target.
        constexpr std::ptrdiff_t ScaleformBeginCall{ 0x18A };
    }
}
