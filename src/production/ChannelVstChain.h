#pragma once

#include "channel/InputChannel.h"
#include "channel/ChannelIdentity.h"
#include "channel/ChannelRack.h"
#include "channel/ChannelTypes.h"
#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Processor.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audient::vst3
{
class Vst3ModuleLoader;
class Vst3Host;
class Vst3Editor;
class Vst3Controller;
}

namespace audient::production
{

// Stable, durable identity of ONE live plug-in slot inside one channel. A
// SlotId is allocated once when the plug-in is added and NEVER changes,
// including across reorder. Position (the vector index in the rack / live list)
// is only ordering. Editor ownership, controller ownership, parameter-edit
// routing, retirement tracking and every future UI handle bind to the SlotId /
// the stable heap-backed SlotBundle, never to the current index. SlotId is
// channel-local: two ChannelVstChain instances may both contain SlotId 1. Any
// future cross-channel identity must use ChannelIdentity + SlotId; there is no
// global SlotId allocator.
using SlotId = std::uint64_t;

enum class ChainResult
{
    Ok,
    Full,     // product rack is full (v1 cap = 4)
    NotFound, // no live slot with that id / out-of-range index
    Invalid,  // bad argument or operation not supported for this slot
};

struct SlotInfo
{
    SlotId id = 0;
    std::string name;
    bool bypass = false;
};

// Reusable, control-thread VST owner for ONE physical input channel
// (Phase B B3). Holds:
//   - an owned channel::InputChannel (the product control model: identity,
//     per-channel runtime config, and the ChannelRack whose product cap is 4),
//   - one independent immutable-snapshot vst3::Vst3Chain (stable address), and
//   - independent live SlotBundles (loader/host/processor/editor per stable
//     SlotId) plus an independent retirement queue.
//
// Invariants (maintained transactionally by every structural mutation):
//   product rack size      == live SlotBundle count
//   product rack order     == published VST processor order
//   rack[i].id             == m_live[i]->id   (same stable SlotId)
//   rack[i].bypass         == m_live[i]->bypass
//   product cap            == channel::kMaxSlotsPerChannel (4)
// A failed load/prepare leaves rack, heavy slot order and the published
// snapshot unchanged (no partial mutation is ever published).
//
// RT contract: the audio callback reads ONLY the published immutable snapshot
// through chain() (a stable vst3::Vst3Chain). The callback never touches this
// object, never allocates, never destroys anything. All bundles are destroyed
// on the control thread via serviceRetirements() after the chain grace elapses,
// or via quiescentShutdown()/the destructor once the host has guaranteed the RT
// callback is detached (no blocking wait is ever added to the audio thread).
//
// Stable-address contract (Phase B B3 refinement 4): a ChannelVstChain that is
// wired to RT (RoutingCore/adapter hold &chain() as the hook context) must
// never be relocated. This class is therefore non-copyable and non-movable;
// higher layers must own live wired owners through stable heap ownership (e.g.
// std::unique_ptr) and must never store them in a container whose reallocation
// moves them. The chain context address is constant from attach until detach/
// quiescence.
class ChannelVstChain
{
public:
    static constexpr std::size_t kMaxSlots = channel::kMaxSlotsPerChannel; // v1 product cap = 4

    explicit ChannelVstChain(channel::ChannelIdentity identity);
    ~ChannelVstChain();

    ChannelVstChain(const ChannelVstChain&) = delete;
    ChannelVstChain& operator=(const ChannelVstChain&) = delete;
    ChannelVstChain(ChannelVstChain&&) = delete;
    ChannelVstChain& operator=(ChannelVstChain&&) = delete;

    // Sizes the chain's mono<->stereo adapter staging. Control thread, once per
    // engine block size BEFORE streaming and BEFORE loading processors.
    void configure(std::size_t maxBlockSamples);

    const channel::ChannelIdentity& identity() const;
    channel::InputChannel& controlChannel() { return m_input; }
    const channel::InputChannel& controlChannel() const { return m_input; }

    // --- RT-facing access ---------------------------------------------------
    // Stable object address for the whole lifetime of the owner. The callback
    // reads only the immutable snapshots inside this chain.
    audient::vst3::Vst3Chain& chain() { return m_chain; }
    const audient::vst3::Vst3Chain& chain() const { return m_chain; }
    std::uint32_t latencySamples() const; // total live processing path latency

    // --- Structural mutation (control thread; transactional) -----------------
    // All fallible preparation and capacity checks happen before the commit
    // phase. The commit phase only uses pre-reserved control-side capacity and
    // publishes rack, bundle order, bypass state, and the immutable snapshot as
    // one consistent state transition. Nothing changes when preparation fails.
    // Loads one real .vst3 module and prepares a MONO processor.
    ChainResult loadSlot(const std::string& vst3ModulePath);                    // append
    ChainResult loadSlotAt(const std::string& vst3ModulePath, std::size_t index);

    // Commits an already-prepared processor (DSP-only: no editor/controller
    // support unless the slot was created via loadSlot*).
    ChainResult addPrepared(std::unique_ptr<audient::vst3::Vst3Processor> processor, std::string name);

    ChainResult removeSlot(SlotId id);
    ChainResult moveSlot(SlotId id, std::size_t toIndex);
    ChainResult setSlotBypass(SlotId id, bool bypass);
    void setWholeChainBypass(bool bypass);
    bool wholeChainBypass() const;

    // --- Queries -------------------------------------------------------------
    std::size_t slotCount() const;              // == rack size (live)
    bool empty() const;
    bool full() const;
    bool hasSlot(SlotId id) const;              // live slot exists
    SlotId slotIdAt(std::size_t index) const;   // 0 when out of range
    std::vector<SlotId> slotIds() const;        // live order
    bool slotInfoAt(std::size_t index, SlotInfo& out) const;
    bool slotInfoById(SlotId id, SlotInfo& out) const;
    std::size_t pendingRetirementCount() const;
    bool hasPendingRetirements() const;
    std::size_t openEditorCount() const;

    // --- Component / controller state (per stable SlotId) --------------------
    // Component blob: Vst3Processor::saveState/restoreState. Controller blob:
    // opened transiently through a Vst3Controller on the control thread. Both
    // require a live slot. Controller/editor-capable slots are those created by
    // loadSlot* (they own the loader + host needed for the controller).
    bool saveComponentState(SlotId id, std::vector<std::uint8_t>& out) const;
    bool restoreComponentState(SlotId id, const std::vector<std::uint8_t>& blob);
    bool saveControllerState(SlotId id, std::vector<std::uint8_t>& out);
    bool restoreControllerState(SlotId id, const std::vector<std::uint8_t>& blob);

    // --- Editor ownership (per stable SlotId) ---------------------------------
    // Native editors stay independent per slot; editor objects live inside the
    // stable SlotBundle so they never move with a reorder. One editor per slot.
    ChainResult openEditor(SlotId id, void* parentWindow);
    ChainResult closeEditor(SlotId id);
    bool isEditorOpen(SlotId id) const;

    // --- Off-RT retirement service -------------------------------------------
    // Explicit control-thread service: reaps expired snapshots and destroys
    // retired SlotBundles whose grace has elapsed. Does NOT depend on another
    // mutation happening. Never call from the audio callback.
    void serviceRetirements();

    // --- Quiescent shutdown ---------------------------------------------------
    // Control thread, ONLY once the host guarantees the RT callback is detached
    // (no further process()/processAsio calls) and no callback can dereference
    // chain(). ChannelVstChain does not own or detect callback lifetime. Closes
    // editors and releases all live and retired SlotBundles immediately (no
    // waiting for more audio blocks). Idempotent; safe to call from the
    // destructor. Never add a blocking wait to the audio thread.
    void quiescentShutdown();

    const std::string& lastError() const { return m_lastError; }

private:
    struct SlotBundle
    {
        // Destruction order (reverse of declaration) is intentionally
        // editor -> processor -> host -> loader.
        SlotId id = 0; // stable slot identity (== the rack tag id at the same index)
        std::unique_ptr<audient::vst3::Vst3ModuleLoader> loader;
        std::unique_ptr<audient::vst3::Vst3Host> host;
        std::unique_ptr<audient::vst3::Vst3Processor> processor;
        std::string name;
        std::string path;
        bool bypass = false;
        std::unique_ptr<audient::vst3::Vst3Editor> editor; // last: destroyed first
    };

    using BundlePtr = std::unique_ptr<SlotBundle>;

    SlotBundle* findLive(SlotId id);
    const SlotBundle* findLive(SlotId id) const;
    std::size_t liveIndex(SlotId id) const;

    ChainResult commitPrepared(BundlePtr bundle, std::string name, std::size_t index);
    bool publishSnapshot(std::vector<audient::vst3::Vst3Chain::Slot> snapshot, bool wholeChainBypass);
    void closeEditorOf(SlotBundle& bundle);
    void releaseAll();

    channel::InputChannel m_input;
    audient::vst3::Vst3Chain m_chain; // stable address; RT reads its snapshots

    std::vector<BundlePtr> m_live;     // live, ordered (position == rack order)
    std::vector<BundlePtr> m_retiring; // removed slots awaiting grace destruction

    bool m_wholeChainBypass = false;
    SlotId m_nextSlotId = 1;
    std::size_t m_maxBlock = 0;
    bool m_shutdown = false;
    mutable std::string m_lastError;
};

} // namespace audient::production
