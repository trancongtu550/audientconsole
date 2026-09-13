#include "asio/StreamFade.h"

namespace audient::asio
{

void StreamFade::configure(std::size_t fadeSamples)
{
    m_total = fadeSamples;
    m_position = 0;
    m_current = State::Muted;
    m_value = 0.0f;
    m_start = 0.0f;
    m_target = 0.0f;
    m_desired.store(State::Muted, std::memory_order_relaxed);
    m_muted.store(true, std::memory_order_relaxed);
    m_full.store(false, std::memory_order_relaxed);
}

void StreamFade::requestMute()
{
    m_desired.store(State::Muted, std::memory_order_release);
}

void StreamFade::requestFadeIn()
{
    m_desired.store(State::FadingIn, std::memory_order_release);
}

void StreamFade::requestFadeOut()
{
    m_desired.store(State::FadingOut, std::memory_order_release);
}

void StreamFade::rearm(State next)
{
    m_current = next;
    m_position = 0;
    m_start = m_value;
    switch (next)
    {
    case State::Muted:
        m_target = 0.0f;
        break;
    case State::FadingIn:
        m_target = 1.0f;
        break;
    case State::FadingOut:
        m_target = 0.0f;
        break;
    }
}

void StreamFade::settleFromTarget()
{
    m_value = m_target;
    if (m_value <= 0.0f)
    {
        m_muted.store(true, std::memory_order_relaxed);
        m_full.store(false, std::memory_order_relaxed);
        m_current = State::Muted;
    }
    else
    {
        m_muted.store(false, std::memory_order_relaxed);
        m_full.store(true, std::memory_order_relaxed);
        m_current = State::FadingIn;
    }
}

float StreamFade::next()
{
    const State desired = m_desired.load(std::memory_order_acquire);
    if (desired != m_current)
    {
        rearm(desired);
    }

    if (m_total == 0)
    {
        m_value = 0.0f;
        m_muted.store(true, std::memory_order_relaxed);
        m_full.store(false, std::memory_order_relaxed);
        m_current = State::Muted;
        return m_value;
    }

    if (m_position >= m_total)
    {
        settleFromTarget();
        return m_value;
    }

    const float t = static_cast<float>(m_position) / static_cast<float>(m_total);
    m_value = m_start + (m_target - m_start) * t;
    ++m_position;
    return m_value;
}

bool StreamFade::isMuted() const
{
    return m_muted.load(std::memory_order_relaxed);
}

bool StreamFade::isFull() const
{
    return m_full.load(std::memory_order_relaxed);
}

} // namespace audient::asio