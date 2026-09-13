#pragma once

#include <atomic>
#include <cstddef>

namespace audient::asio
{

class StreamFade
{
public:
    enum class State
    {
        Muted,
        FadingIn,
        FadingOut,
    };

    void configure(std::size_t fadeSamples);
    void requestMute();
    void requestFadeIn();
    void requestFadeOut();

    float next();
    bool isMuted() const;
    bool isFull() const;

private:
    void rearm(State next);
    void settleFromTarget();

    std::atomic<State> m_desired{State::Muted};
    State m_current = State::Muted;
    float m_value = 0.0f;
    float m_start = 0.0f;
    float m_target = 0.0f;
    std::size_t m_total = 0;
    std::size_t m_position = 0;

    std::atomic<bool> m_muted{true};
    std::atomic<bool> m_full{false};
};

} // namespace audient::asio