#include "vst3/Vst3StateStream.h"

#include "pluginterfaces/base/funknown.h"

#include <algorithm>
#include <cstring>

namespace audient::vst3
{

Vst3StateStream::Vst3StateStream(std::size_t maxBytes)
    : m_maxBytes(maxBytes > 0 ? maxBytes : kMaxStateBytes)
    , m_refs{1}
{
}

Steinberg::tresult Vst3StateStream::read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead)
{
    if (buffer == nullptr || numBytes < 0)
    {
        if (numBytesRead != nullptr)
        {
            *numBytesRead = 0;
        }
        return numBytes < 0 ? Steinberg::kInvalidArgument : Steinberg::kResultTrue;
    }
    const std::size_t available = m_buffer.size() - m_cursor;
    const std::size_t toRead = std::min<std::size_t>(static_cast<std::size_t>(numBytes), available);
    if (toRead > 0)
    {
        std::memcpy(buffer, m_buffer.data() + m_cursor, toRead);
        m_cursor += toRead;
    }
    if (numBytesRead != nullptr)
    {
        *numBytesRead = static_cast<Steinberg::int32>(toRead);
    }
    return Steinberg::kResultTrue;
}

Steinberg::tresult Vst3StateStream::write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten)
{
    if (buffer == nullptr || numBytes < 0)
    {
        if (numBytesWritten != nullptr)
        {
            *numBytesWritten = 0;
        }
        return numBytes < 0 ? Steinberg::kInvalidArgument : Steinberg::kResultTrue;
    }

    const std::size_t required = m_cursor + static_cast<std::size_t>(numBytes);
    if (required > m_maxBytes)
    {
        // Defined bound (AGENTS §9/§17): refuse the write, never grow unbounded.
        m_overflow = true;
        if (numBytesWritten != nullptr)
        {
            *numBytesWritten = 0;
        }
        return Steinberg::kResultFalse;
    }

    if (required > m_buffer.size())
    {
        m_buffer.resize(required);
    }
    std::memcpy(m_buffer.data() + m_cursor, buffer, static_cast<std::size_t>(numBytes));
    m_cursor += static_cast<std::size_t>(numBytes);
    if (numBytesWritten != nullptr)
    {
        *numBytesWritten = numBytes;
    }
    return Steinberg::kResultTrue;
}

Steinberg::tresult Vst3StateStream::seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result)
{
    Steinberg::int64 target = static_cast<Steinberg::int64>(m_cursor);
    switch (mode)
    {
        case Steinberg::IBStream::kIBSeekSet:
            target = pos;
            break;
        case Steinberg::IBStream::kIBSeekCur:
            target = static_cast<Steinberg::int64>(m_cursor) + pos;
            break;
        case Steinberg::IBStream::kIBSeekEnd:
            target = static_cast<Steinberg::int64>(m_buffer.size()) + pos;
            break;
        default:
            return Steinberg::kInvalidArgument;
    }
    if (target < 0)
    {
        return Steinberg::kInvalidArgument;
    }
    m_cursor = static_cast<std::size_t>(target);
    if (result != nullptr)
    {
        *result = static_cast<Steinberg::int64>(m_cursor);
    }
    return Steinberg::kResultTrue;
}

Steinberg::tresult Vst3StateStream::tell(Steinberg::int64* pos)
{
    if (pos == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }
    *pos = static_cast<Steinberg::int64>(m_cursor);
    return Steinberg::kResultTrue;
}

Steinberg::tresult Vst3StateStream::queryInterface(const Steinberg::TUID requestId, void** obj)
{
    if (obj == nullptr)
    {
        return Steinberg::kInvalidArgument;
    }
    if (Steinberg::FUnknownPrivate::iidEqual(requestId, Steinberg::IBStream::iid.toTUID()) ||
        Steinberg::FUnknownPrivate::iidEqual(requestId, Steinberg::FUnknown::iid.toTUID()))
    {
        *obj = static_cast<Steinberg::IBStream*>(this);
        addRef();
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

Steinberg::uint32 Vst3StateStream::addRef()
{
    return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

Steinberg::uint32 Vst3StateStream::release()
{
    // Owned by the caller for its lifetime; never deleted at zero (member/heap
    // lifetime is the caller's choice, so a zero ref just returns 0).
    const std::uint32_t previous = m_refs.fetch_sub(1, std::memory_order_relaxed);
    if (previous == 1)
    {
        return 0;
    }
    return previous - 1;
}

// ---------------------------------------------------------------------------
// Blob codec
// ---------------------------------------------------------------------------

namespace
{

void putLe32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void putLe16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

std::uint32_t getLe32(const std::vector<std::uint8_t>& in, std::size_t offset)
{
    return static_cast<std::uint32_t>(in[offset]) | (static_cast<std::uint32_t>(in[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(in[offset + 2]) << 16) | (static_cast<std::uint32_t>(in[offset + 3]) << 24);
}

std::uint16_t getLe16(const std::vector<std::uint8_t>& in, std::size_t offset)
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[offset]) |
                                      (static_cast<std::uint16_t>(in[offset + 1]) << 8));
}

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

} // namespace

std::uint64_t Vst3StateBlob::fnv1a64(const std::uint8_t* data, std::size_t size)
{
    std::uint64_t hash = kFnvOffsetBasis;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
    return hash;
}

std::vector<std::uint8_t> Vst3StateBlob::encode(const std::vector<std::uint8_t>& payload)
{
    if (payload.size() > kMaxStateBytes)
    {
        return {};
    }
    std::vector<std::uint8_t> blob;
    blob.reserve(kHeaderSize + payload.size());
    putLe32(blob, kMagic);
    putLe16(blob, kVersion);
    putLe16(blob, 0);
    putLe32(blob, static_cast<std::uint32_t>(payload.size()));
    const std::uint64_t checksum = fnv1a64(payload.data(), payload.size());
    putLe32(blob, static_cast<std::uint32_t>(checksum & 0xFFFFFFFFull));
    putLe32(blob, static_cast<std::uint32_t>((checksum >> 32) & 0xFFFFFFFFull));
    blob.insert(blob.end(), payload.begin(), payload.end());
    return blob;
}

bool Vst3StateBlob::decode(const std::vector<std::uint8_t>& blob, std::vector<std::uint8_t>& payload)
{
    payload.clear();
    if (blob.size() < kHeaderSize)
    {
        return false; // truncated header
    }
    if (getLe32(blob, 0) != kMagic)
    {
        return false; // not a state blob
    }
    if (getLe16(blob, 4) != kVersion)
    {
        return false; // unsupported version
    }
    const std::uint32_t payloadSize = getLe32(blob, 8);
    if (payloadSize > kMaxStateBytes || static_cast<std::size_t>(payloadSize) + kHeaderSize != blob.size())
    {
        return false; // oversized or length field not consistent
    }
    const std::uint64_t expectedChecksum =
        static_cast<std::uint64_t>(getLe32(blob, 12)) |
        (static_cast<std::uint64_t>(getLe32(blob, 16)) << 32);
    const std::uint64_t actualChecksum = fnv1a64(blob.data() + kHeaderSize, payloadSize);
    if (expectedChecksum != actualChecksum || expectedChecksum == 0)
    {
        return false; // corrupt payload
    }
    payload.assign(blob.begin() + static_cast<std::ptrdiff_t>(kHeaderSize), blob.end());
    return true;
}

} // namespace audient::vst3