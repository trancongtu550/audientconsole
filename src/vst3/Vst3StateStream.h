#pragma once

#include "pluginterfaces/base/ibstream.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audient::vst3
{

// Max bytes a plug-in may write as component state in v1. Bounded so a
// misbehaving or malicious getState can never cause unbounded allocation.
inline constexpr std::size_t kMaxStateBytes = 4u * 1024u * 1024u; // 4 MiB

// Host-side IBStream handed to IComponent::getState/setState (VST-012).
//
// One instance backs a single save or restore. It owns a byte buffer:
//  - save: the plug-in writes via IBStream; read back with view() after
//    getState() returns (never from the audio callback).
//  - restore: wrap the persisted payload read-only, seek(0), and hand to
//    setState(); the plug-in must not require a growable stream.
//
// Bounds (project guidelines §9/§17, STATE-004/VST-014): writes beyond kMaxStateBytes
// are refused and overflow() is set; no unbounded growth is possible.
class Vst3StateStream : public Steinberg::IBStream
{
public:
    explicit Vst3StateStream(std::size_t maxBytes = kMaxStateBytes);
    ~Vst3StateStream() = default;

    // --- IBStream -----------------------------------------------------------
    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes,
                                       Steinberg::int32* numBytesRead) override;
    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes,
                                        Steinberg::int32* numBytesWritten) override;
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode,
                                       Steinberg::int64* result) override;
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override;

    // --- FUnknown -----------------------------------------------------------
    Steinberg::tresult queryInterface(const Steinberg::TUID requestId, void** obj) override;
    Steinberg::uint32 addRef() override;
    Steinberg::uint32 release() override;

    // The buffer the plug-in has written so far (save path). Read only while
    // the stream is not concurrently written; the view is the produced bytes.
    const std::vector<std::uint8_t>& view() const { return m_buffer; }

    // True when a plugin write was refused because it exceeded maxBytes.
    bool overflow() const { return m_overflow; }
    std::size_t maxBytes() const { return m_maxBytes; }
    // Current stream position (tests inspect cursor bounds).
    std::size_t cursor() const { return m_cursor; }

private:
    std::vector<std::uint8_t> m_buffer;
    std::size_t m_cursor = 0;
    std::size_t m_maxBytes = 0;
    bool m_overflow = false;
    std::atomic<std::uint32_t> m_refs{1};
};

// Opaque persisted blob wrapper around a plug-in's component-state payload.
// Layout (little-endian, fixed at v1):
//   [0..4)   magic     'AVST'
//   [4..6)   version   1
//   [6..8)   reserved  0
//   [8..12)  payload size (u32)
//   [12..20) checksum   FNV-1a 64 over the payload
//   [20..)   payload (the raw bytes IComponent::getState produced)
//
// Both encode() and decode() reject payloads larger than kMaxStateBytes;
// decode() also rejects a bad magic/version, a size field that does not match
// the buffer length, and a checksum mismatch, without touching the payload.
struct Vst3StateBlob
{
    static constexpr std::uint32_t kMagic = 0x54535641u; // 'AVST'
    static constexpr std::uint16_t kVersion = 1;
    static constexpr std::size_t kHeaderSize = 20;

    static std::vector<std::uint8_t> encode(const std::vector<std::uint8_t>& payload);
    // Returns false and clears payload when the blob is malformed/oversized.
    static bool decode(const std::vector<std::uint8_t>& blob, std::vector<std::uint8_t>& payload);

private:
    static std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size);
};

} // namespace audient::vst3