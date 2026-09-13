#include "id14-control/protocol/Id14CommandWhitelist.h"

namespace audient::id14
{

// No MK1 control command is verified yet (no read-only proof, no safe reversible
// write proof, no control-inventory evidence). The whitelist is intentionally empty:
// every request is rejected with NotSupported, and the UI must render the control as
// non-invokable (project guidelines §10.3).
bool Id14CommandWhitelist::isAllowed(Id14ControlKind kind, std::uint32_t valueBits,
                                     std::uint32_t payloadBytes) const
{
    (void)kind;
    (void)valueBits;
    (void)payloadBytes;
    return false;
}

bool Id14CommandWhitelist::isWritable(Id14ControlKind kind) const
{
    (void)kind;
    return false;
}

std::size_t Id14CommandWhitelist::size() const
{
    return 0;
}

} // namespace audient::id14