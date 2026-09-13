#pragma once

#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audient::vst3
{

// Realtime-safe host-side IParameterChanges / IParamValueQueue implementation.
// Preallocated at maximum capacity; the callback only fills it from the
// ParameterEditQueue and hands it to IAudioProcessor::process as
// ProcessData::inputParameterChanges. Nothing here allocates, locks, or blocks.
//
// Design:
//  - kMaxQueues parameter queues, each holding kMaxPoints values.
//  - The queue for a repeated ParamID is reused in insertion order (no lookup
//    table), so duplicate edits to the same parameter in a block are coalesced
//    into one queue with multiple points rather than separate queues. The
//    plug-in sees them in order and typically applies the last point.
//  - addParameterData returns a pointer into a fixed pool. If the pool is
//    exhausted it returns nullptr instead of growing (defined overflow).
class StreamingParameterChanges : public Steinberg::Vst::IParameterChanges
{
public:
    static constexpr Steinberg::int32 kMaxQueues = 8;
    static constexpr Steinberg::int32 kMaxPoints = 4;

    StreamingParameterChanges();
    ~StreamingParameterChanges();

    // IParameterChanges
    Steinberg::int32 PLUGIN_API getParameterCount() override;
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override;
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID& id, Steinberg::int32& index) override;

    // FUnknown
    Steinberg::tresult queryInterface(const Steinberg::TUID requestId, void** obj) override;
    Steinberg::uint32 addRef() override;
    Steinberg::uint32 release() override;

    // Resets all queues to empty. Audio-thread only.
    void reset();

    // Appends a value to the first queue whose id matches `id` (in insertion
    // order) or to the first free queue. Returns false if the pool is full or
    // a matching queue already holds kMaxPoints. Audio-thread only.
    bool addPoint(const Steinberg::Vst::ParamID& id, Steinberg::int32 sampleOffset, Steinberg::Vst::ParamValue value);

    Steinberg::int32 pointCount(const Steinberg::Vst::ParamID& id) const;

private:
    class Queue : public Steinberg::Vst::IParamValueQueue
    {
    public:
        Steinberg::Vst::ParamID PLUGIN_API getParameterId() override;
        Steinberg::int32 PLUGIN_API getPointCount() override;
        Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32& sampleOffset,
                                               Steinberg::Vst::ParamValue& value) override;
        Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset, Steinberg::Vst::ParamValue value,
                                               Steinberg::int32& index) override;
        Steinberg::tresult queryInterface(const Steinberg::TUID requestId, void** obj) override;
        Steinberg::uint32 addRef() override;
        Steinberg::uint32 release() override;

        bool init(const Steinberg::Vst::ParamID& id);
        bool add(Steinberg::int32 sampleOffset, Steinberg::Vst::ParamValue value);
        void clear();

        Steinberg::Vst::ParamID id() const { return m_id; }
        Steinberg::int32 count() const { return m_count; }

    private:
        Steinberg::Vst::ParamID m_id = Steinberg::Vst::kNoParamId;
        Steinberg::int32 m_count = 0;
        Steinberg::int32 m_offsets[kMaxPoints]{};
        Steinberg::Vst::ParamValue m_values[kMaxPoints]{};
        std::atomic<std::uint32_t> m_refs{1};
    };

    Queue m_queues[kMaxQueues];
    Steinberg::int32 m_count = 0;
    std::atomic<std::uint32_t> m_refs{1};
};

} // namespace audient::vst3