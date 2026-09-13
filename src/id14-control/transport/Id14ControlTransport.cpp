#include "id14-control/transport/Id14ControlTransport.h"

namespace audient::id14
{

const char* transportResultName(TransportResult result)
{
    switch (result)
    {
    case TransportResult::Ok:              return "ok";
    case TransportResult::NotSupported:    return "not-supported";
    case TransportResult::InvalidValue:    return "invalid-value";
    case TransportResult::Busy:            return "busy";
    case TransportResult::Timeout:         return "timeout";
    case TransportResult::DeviceMismatch:  return "device-mismatch";
    case TransportResult::Disconnected:    return "disconnected";
    case TransportResult::NotImplemented:  return "not-implemented";
    case TransportResult::Other:           return "other";
    }
    return "unknown";
}

} // namespace audient::id14