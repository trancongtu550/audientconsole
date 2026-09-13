#pragma once

#include "engine/GraphConfig.h"

#include <atomic>
#include <cstdint>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace audient::engine
{

template <typename T>
class SeqLockSnapshot
{
public:
    void publish(const T& value)
    {
        m_seq.fetch_add(1, std::memory_order_acq_rel);
        m_value = value;
        m_seq.fetch_add(1, std::memory_order_acq_rel);
    }

    bool tryRead(T& out) const
    {
        const std::uint32_t seq0 = m_seq.load(std::memory_order_acquire);
        if ((seq0 & 1u) != 0)
        {
            return false;
        }
        out = m_value;
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint32_t seq1 = m_seq.load(std::memory_order_acquire);
        return seq0 == seq1;
    }

    T read() const
    {
        T out{};
        while (!tryRead(out))
        {
        }
        return out;
    }

private:
    alignas(64) mutable std::atomic<std::uint32_t> m_seq{0};
    alignas(64) T m_value{};
};

using EngineConfigSnapshot = SeqLockSnapshot<EngineConfig>;

} // namespace audient::engine

#if defined(_MSC_VER)
#pragma warning(pop)
#endif