#include "engine/SanitizeAudio.h"

#include <cmath>

#if defined(_M_X64) || defined(_M_IX86)
#include <float.h>
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace audient::engine
{

float sanitizeValue(float value)
{
    if (value != value)
    {
        return 0.0f;
    }
    if (std::isinf(value))
    {
        return value < 0.0f ? -1.0f : 1.0f;
    }
    return value;
}

void sanitizeBlock(float* data, std::size_t frames, SanitizeCounters& counters)
{
    for (std::size_t i = 0; i < frames; ++i)
    {
        const float value = data[i];
        if (value != value)
        {
            data[i] = 0.0f;
            ++counters.nanReplaced;
        }
        else if (std::isinf(value))
        {
            data[i] = value < 0.0f ? -1.0f : 1.0f;
            ++counters.infClamped;
        }
    }
}

void enableFlushToZero()
{
#if defined(_M_X64) || defined(_M_IX86)
    unsigned int control = 0;
    ::_controlfp_s(&control, _DN_FLUSH, _MCW_DN);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

} // namespace audient::engine