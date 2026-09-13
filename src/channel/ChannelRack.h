#pragma once

#include "channel/ChannelTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace audient::channel
{

enum class ChannelRackResult
{
    Ok,
    Full,
    NotFound,
    Invalid,
};

// Ordered insert rack for one input channel (control/UI thread only; nothing
// here runs on the audio path). Enforces the v1 product ceiling
// (kMaxSlotsPerChannel = 4): a fifth add is rejected cleanly. Slot contents are
// control-side tags (name/bypass/missing) plus a stable slot id; the heavy
// plug-in lifecycle (module/host/processor/editor) is attached per stable id by
// the production ChannelVstChain owner (Phase B B3), never stored here.
class ChannelRack
{
public:
    struct Slot
    {
        std::string name;     // human label, e.g. plug-in name
        bool bypass = false;  // per-slot host-side bypass
        bool missing = false; // placeholder when the plug-in is not loadable
        std::uint64_t id = 0; // STABLE slot id (unchanged across reorder); issued by the owner

        bool operator==(const Slot&) const = default;
        bool operator!=(const Slot&) const = default;
    };

    static constexpr std::size_t kMaxSlots = kMaxSlotsPerChannel;

    // The product cap is reserved up front so a B3 rack commit cannot fail
    // after another owner has made its state observable.
    ChannelRack();

    ChannelRackResult add(std::string name);
    ChannelRackResult add(Slot slot);
    ChannelRackResult removeAt(std::size_t index);
    ChannelRackResult move(std::size_t from, std::size_t to);
    ChannelRackResult setBypass(std::size_t index, bool bypass);
    ChannelRackResult setMissing(std::size_t index, bool missing);

    void setWholeChainBypass(bool bypass);
    bool wholeChainBypass() const { return m_wholeChainBypass; }

    std::size_t size() const { return m_slots.size(); }
    bool empty() const { return m_slots.empty(); }
    bool full() const { return m_slots.size() >= kMaxSlots; }

    const Slot* slot(std::size_t index) const;
    Slot* slot(std::size_t index);

    void clear();

private:
    bool validIndex(std::size_t index) const { return index < m_slots.size(); }

    std::vector<Slot> m_slots;
    bool m_wholeChainBypass = false;
};

} // namespace audient::channel
