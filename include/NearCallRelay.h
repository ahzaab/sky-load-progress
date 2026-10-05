#pragma once

#include <Windows.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>

namespace load_progress
{
    inline bool CallRelayInRange(std::uintptr_t site, std::uintptr_t relay) noexcept
    {
        const auto next = site + 5;
        return relay >= next ? relay - next <= 0x7FFFFFFFULL : next - relay <= 0x80000000ULL;
    }

    // Intersect all rel32 windows. VirtualQuery can return a free region whose
    // base lies BEFORE the queried address: never allocate at that unbounded base.
    inline void* AllocateNearCallRelay(std::span<const std::uintptr_t> sites, std::size_t size) noexcept
    {
        if (sites.empty() || !size) {
            return nullptr;
        }
        SYSTEM_INFO info{};
        ::GetSystemInfo(&info);
        const auto maximum = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
        auto lower = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
        auto upper = maximum;
        for (const auto site : sites) {
            if (site > maximum - 5) {
                return nullptr;
            }
            const auto next = site + 5;
            lower = (std::max)(lower, next >= 0x80000000ULL ? next - 0x80000000ULL : 0ULL);
            upper = (std::min)(upper, next <= maximum - 0x7FFFFFFFULL ? next + 0x7FFFFFFFULL : maximum);
        }
        const std::uintptr_t granularity = info.dwAllocationGranularity;
        const std::uintptr_t allocation = (size + info.dwPageSize - 1) / info.dwPageSize * info.dwPageSize;
        if (upper < lower || allocation > upper - lower) {
            return nullptr;
        }
        auto cursor = lower;
        while (cursor <= upper - allocation) {
            MEMORY_BASIC_INFORMATION region{};
            if (!::VirtualQuery(reinterpret_cast<void*>(cursor), &region, sizeof(region))) {
                break;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            if (region.RegionSize > maximum - base) {
                break;
            }
            const auto end = base + region.RegionSize;
            if (region.State == MEM_FREE) {
                const auto start = (std::max)(cursor, base);
                const auto candidate = (start + granularity - 1) / granularity * granularity;
                if (candidate >= lower && candidate <= upper - allocation &&
                    candidate < end && allocation <= end - candidate) {
                    if (auto* memory = ::VirtualAlloc(reinterpret_cast<void*>(candidate), size,
                            MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
                        return memory;
                    }
                }
            }
            if (end <= cursor) {
                break;
            }
            cursor = end;
        }
        return nullptr;
    }
}
