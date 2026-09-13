#pragma once

#include <atomic>
#include <cstdint>

namespace audient::engine
{

inline std::atomic<std::uint64_t>& callbackExceptionCounter()
{
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

template <typename Callback>
bool invokeInCallback(Callback&& callback) noexcept
{
    try
    {
        callback();
        return true;
    }
    catch (...)
    {
        callbackExceptionCounter().fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

inline std::uint64_t callbackExceptionCount()
{
    return callbackExceptionCounter().load(std::memory_order_relaxed);
}

} // namespace audient::engine