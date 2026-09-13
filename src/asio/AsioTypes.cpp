#include "asio/AsioTypes.h"

#include <algorithm>

namespace audient::asio
{

bool DriverCapabilities::supportsSampleRate(long sampleRate) const
{
    return std::find(supportedSampleRates.begin(), supportedSampleRates.end(), sampleRate) != supportedSampleRates.end();
}

} // namespace audient::asio