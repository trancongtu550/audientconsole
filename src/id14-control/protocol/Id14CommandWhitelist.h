#pragma once

#include "id14-control/semantic-model/Id14SemanticControl.h"

#include <cstddef>
#include <cstdint>

namespace audient::id14
{

// Strict command whitelist between the semantic model and the transport (ADR-005).
// Every entry must carry evidence-bound metadata; unknown command IDs and payload
// shapes are rejected before any byte leaves the process. Until a command is proven
// on the exact MK1 tuple, the whitelist is empty — nothing is writable.

struct WhitelistEntry
{
    Id14ControlKind kind;
    Id14SupportClass supportClass;
    std::size_t payloadBytes;
    const char* evidenceRef; // ADR-005 / protocol-evidence doc section
};

class Id14CommandWhitelist
{
public:
    bool isAllowed(Id14ControlKind kind, std::uint32_t valueBits, std::uint32_t payloadBytes) const;
    bool isWritable(Id14ControlKind kind) const;
    std::size_t size() const;
};

} // namespace audient::id14