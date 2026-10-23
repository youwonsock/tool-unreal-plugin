#pragma once

// Instance identities live only in the loaded runtime, never in job records or
// external API replies. This allocator is also tested without Unreal.
#include <atomic>
#include <cstdint>

namespace UnrealToolsRuntime
{
inline std::uint64_t NextServiceInstanceGeneration()
{
    static std::atomic<std::uint64_t> Generation{0};
    return Generation.fetch_add(1, std::memory_order_relaxed) + 1;
}
} // namespace UnrealToolsRuntime
