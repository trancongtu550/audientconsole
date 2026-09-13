#include "vst3/Vst3StateStream.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

// VST-012 infrastructure: the bounded host IBStream handed to
// IComponent::getState/setState, and the opaque checksummed blob wrapper the
// session layer persists. These tests pin the bounded/overflow policy and the
// magic/version/length/checksum validation without requiring a plug-in.

namespace
{

constexpr std::uint8_t kByteValue = 0xA5;

std::vector<std::uint8_t> makePayload(std::size_t size)
{
    std::vector<std::uint8_t> payload(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        payload[i] = static_cast<std::uint8_t>(kByteValue + static_cast<std::uint8_t>(i & 0x0F));
    }
    return payload;
}

} // namespace

// ---------------- Vst3StateStream ----------------

TEST(Vst3StateStreamTest, WriteAppendsBytesAndTracksCursor)
{
    audient::vst3::Vst3StateStream stream;
    const std::array<std::uint8_t, 4> bytes = {1, 2, 3, 4};
    Steinberg::int32 written = 0;
    EXPECT_EQ(stream.write(const_cast<std::uint8_t*>(bytes.data()), 4, &written), Steinberg::kResultTrue);
    EXPECT_EQ(written, 4);
    EXPECT_EQ(stream.view().size(), 4u);
    EXPECT_EQ(stream.cursor(), 4u);
    EXPECT_FALSE(stream.overflow());

    const std::array<std::uint8_t, 2> more = {5, 6};
    EXPECT_EQ(stream.write(const_cast<std::uint8_t*>(more.data()), 2, &written), Steinberg::kResultTrue);
    EXPECT_EQ(stream.view().size(), 6u);
    const std::vector<std::uint8_t>& view = stream.view();
    for (std::size_t i = 0; i < view.size(); ++i)
    {
        EXPECT_EQ(view[i], static_cast<std::uint8_t>(i + 1));
    }
}

TEST(Vst3StateStreamTest, ReadReturnsBytesInOrderAndClampsAtEnd)
{
    audient::vst3::Vst3StateStream stream;
    const std::array<std::uint8_t, 5> bytes = {10, 20, 30, 40, 50};
    ASSERT_EQ(stream.write(const_cast<std::uint8_t*>(bytes.data()), 5, nullptr), Steinberg::kResultTrue);
    ASSERT_EQ(stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr), Steinberg::kResultTrue);

    std::array<std::uint8_t, 3> out{};
    Steinberg::int32 read = 0;
    EXPECT_EQ(stream.read(out.data(), 3, &read), Steinberg::kResultTrue);
    EXPECT_EQ(read, 3);
    EXPECT_EQ(out[0], 10);
    EXPECT_EQ(out[1], 20);
    EXPECT_EQ(out[2], 30);
    EXPECT_EQ(stream.cursor(), 3u);

    std::array<std::uint8_t, 4> out2{};
    EXPECT_EQ(stream.read(out2.data(), 4, &read), Steinberg::kResultTrue);
    EXPECT_EQ(read, 2) << "read past end clamps to remaining bytes";
    EXPECT_EQ(out2[0], 40);
    EXPECT_EQ(out2[1], 50);

    EXPECT_EQ(stream.read(out.data(), 3, &read), Steinberg::kResultTrue);
    EXPECT_EQ(read, 0) << "reading at the end yields zero bytes, not an error";
}

TEST(Vst3StateStreamTest, SeekAndTellSupportAllModes)
{
    audient::vst3::Vst3StateStream stream;
    const std::array<std::uint8_t, 8> bytes = {0, 1, 2, 3, 4, 5, 6, 7};
    ASSERT_EQ(stream.write(const_cast<std::uint8_t*>(bytes.data()), 8, nullptr), Steinberg::kResultTrue);

    Steinberg::int64 pos = -1;
    EXPECT_EQ(stream.seek(2, Steinberg::IBStream::kIBSeekSet, &pos), Steinberg::kResultTrue);
    EXPECT_EQ(pos, 2);
    EXPECT_EQ(stream.cursor(), 2u);

    EXPECT_EQ(stream.seek(3, Steinberg::IBStream::kIBSeekCur, &pos), Steinberg::kResultTrue);
    EXPECT_EQ(pos, 5);

    EXPECT_EQ(stream.seek(-2, Steinberg::IBStream::kIBSeekEnd, &pos), Steinberg::kResultTrue);
    EXPECT_EQ(pos, 6);

    EXPECT_EQ(stream.tell(&pos), Steinberg::kResultTrue);
    EXPECT_EQ(pos, 6);

    // Reading after a seek returns the byte at the new cursor.
    std::uint8_t byte = 0;
    Steinberg::int32 read = 0;
    EXPECT_EQ(stream.read(&byte, 1, &read), Steinberg::kResultTrue);
    EXPECT_EQ(read, 1);
    EXPECT_EQ(byte, 6);
}

TEST(Vst3StateStreamTest, WriteBeyondBoundIsRefusedNotGrown)
{
    constexpr std::size_t kBound = 64;
    audient::vst3::Vst3StateStream stream(kBound);

    struct NoisyIStream
    {
    };

    // Fill exactly to the bound.
    {
        std::vector<std::uint8_t> block(kBound, 0x11);
        Steinberg::int32 written = 0;
        EXPECT_EQ(stream.write(block.data(), static_cast<Steinberg::int32>(kBound), &written),
                  Steinberg::kResultTrue);
        EXPECT_EQ(written, static_cast<Steinberg::int32>(kBound));
        EXPECT_FALSE(stream.overflow());
        EXPECT_EQ(stream.view().size(), kBound);
    }

    // One more byte must be refused and flagged, with the buffer unchanged.
    {
        std::vector<std::uint8_t> block(kBound + 1, 0x22);
        Steinberg::int32 written = 0;
        EXPECT_EQ(stream.write(block.data(), static_cast<Steinberg::int32>(kBound + 1), &written),
                  Steinberg::kResultFalse);
        EXPECT_EQ(written, 0);
        EXPECT_TRUE(stream.overflow());
        EXPECT_EQ(stream.view().size(), kBound) << "no unbounded growth on overflow";
        EXPECT_EQ(stream.view()[0], 0x11);
    }
}

TEST(Vst3StateStreamTest, NullBufferOrNegativeCountIsHandled)
{
    audient::vst3::Vst3StateStream stream;
    Steinberg::int32 written = -1;
    EXPECT_EQ(stream.write(nullptr, 4, &written), Steinberg::kResultTrue) << "null buffer is a no-op";
    EXPECT_EQ(written, 0);
    EXPECT_EQ(stream.write(nullptr, -1, &written), Steinberg::kInvalidArgument);

    Steinberg::int32 read = -1;
    EXPECT_EQ(stream.read(nullptr, -1, &read), Steinberg::kInvalidArgument);
    EXPECT_EQ(stream.tell(nullptr), Steinberg::kInvalidArgument);
}

TEST(Vst3StateStreamTest, NegativeSeekIsRejected)
{
    audient::vst3::Vst3StateStream stream;
    const std::array<std::uint8_t, 4> bytes = {0, 1, 2, 3};
    ASSERT_EQ(stream.write(const_cast<std::uint8_t*>(bytes.data()), 4, nullptr), Steinberg::kResultTrue);
    ASSERT_EQ(stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr), Steinberg::kResultTrue);

    Steinberg::int64 pos = -1;
    EXPECT_EQ(stream.seek(-1, Steinberg::IBStream::kIBSeekCur, &pos), Steinberg::kInvalidArgument)
        << "seeking before the start of the stream is rejected";
}

TEST(Vst3StateStreamTest, QueryInterfaceExposesIBStreamAndRefcounts)
{
    audient::vst3::Vst3StateStream stream;

    Steinberg::IBStream* asStream = nullptr;
    EXPECT_EQ(stream.queryInterface(Steinberg::IBStream::iid.toTUID(), reinterpret_cast<void**>(&asStream)),
              Steinberg::kResultOk);
    ASSERT_NE(asStream, nullptr);
    // queryInterface already took one ref (1 -> 2); addRef is then balanced.
    EXPECT_EQ(asStream->addRef(), 3u);
    EXPECT_EQ(asStream->release(), 2u);
    EXPECT_EQ(asStream->release(), 1u);

    Steinberg::FUnknown* asUnknown = nullptr;
    EXPECT_EQ(stream.queryInterface(Steinberg::FUnknown::iid.toTUID(), reinterpret_cast<void**>(&asUnknown)),
              Steinberg::kResultOk);
    ASSERT_NE(asUnknown, nullptr);
    EXPECT_EQ(asUnknown->release(), 1u) << "release cannot delete an object owned by the caller";
}

// ---------------- Vst3StateBlob ----------------

TEST(Vst3StateBlobTest, RoundTripsNonEmptyPayload)
{
    const std::vector<std::uint8_t> payload = makePayload(256);
    const std::vector<std::uint8_t> blob = audient::vst3::Vst3StateBlob::encode(payload);
    ASSERT_FALSE(blob.empty());
    EXPECT_EQ(blob.size(), audient::vst3::Vst3StateBlob::kHeaderSize + payload.size());

    std::vector<std::uint8_t> decoded;
    ASSERT_TRUE(audient::vst3::Vst3StateBlob::decode(blob, decoded));
    EXPECT_EQ(decoded, payload);
}

TEST(Vst3StateBlobTest, RoundTripsEmptyPayload)
{
    const std::vector<std::uint8_t> payload;
    const std::vector<std::uint8_t> blob = audient::vst3::Vst3StateBlob::encode(payload);
    ASSERT_FALSE(blob.empty());
    EXPECT_EQ(blob.size(), audient::vst3::Vst3StateBlob::kHeaderSize);

    std::vector<std::uint8_t> decoded;
    decoded.push_back(0xEE); // must be cleared
    ASSERT_TRUE(audient::vst3::Vst3StateBlob::decode(blob, decoded));
    EXPECT_TRUE(decoded.empty());
}

TEST(Vst3StateBlobTest, RejectsTruncatedBlob)
{
    const std::vector<std::uint8_t> payload = makePayload(32);
    const std::vector<std::uint8_t> blob = audient::vst3::Vst3StateBlob::encode(payload);

    std::vector<std::uint8_t> decoded;
    std::vector<std::uint8_t> cutBlob(blob.begin(), blob.begin() + 10); // < kHeaderSize
    EXPECT_FALSE(audient::vst3::Vst3StateBlob::decode(cutBlob, decoded));
    EXPECT_TRUE(decoded.empty());

    // Truncating by cutting off payload bytes must also fail (length check).
    std::vector<std::uint8_t> cutPayload(blob.begin(), blob.end() - 10);
    EXPECT_FALSE(audient::vst3::Vst3StateBlob::decode(cutPayload, decoded));
}

TEST(Vst3StateBlobTest, RejectsBadMagic)
{
    const std::vector<std::uint8_t> payload = makePayload(16);
    std::vector<std::uint8_t> blob = audient::vst3::Vst3StateBlob::encode(payload);
    blob[0] ^= 0xFF; // corrupt first magic byte
    std::vector<std::uint8_t> decoded;
    EXPECT_FALSE(audient::vst3::Vst3StateBlob::decode(blob, decoded));
    EXPECT_TRUE(decoded.empty());
}

TEST(Vst3StateBlobTest, RejectsUnsupportedVersion)
{
    const std::vector<std::uint8_t> payload = makePayload(16);
    std::vector<std::uint8_t> blob = audient::vst3::Vst3StateBlob::encode(payload);
    blob[4] = 0x7F; // version -> 0x7F01, unsupported
    std::vector<std::uint8_t> decoded;
    EXPECT_FALSE(audient::vst3::Vst3StateBlob::decode(blob, decoded));
}

TEST(Vst3StateBlobTest, RejectsInconsistentLengthField)
{
    const std::vector<std::uint8_t> payload = makePayload(16);
    std::vector<std::uint8_t> blob = audient::vst3::Vst3StateBlob::encode(payload);
    // Claim twice the real payload size: length field must match the buffer.
    blob[8] = static_cast<std::uint8_t>(32);
    std::vector<std::uint8_t> decoded;
    EXPECT_FALSE(audient::vst3::Vst3StateBlob::decode(blob, decoded));
}

TEST(Vst3StateBlobTest, RejectsCorruptPayloadChecksum)
{
    const std::vector<std::uint8_t> payload = makePayload(16);
    std::vector<std::uint8_t> blob = audient::vst3::Vst3StateBlob::encode(payload);
    blob.back() ^= 0x01; // flip one payload bit
    std::vector<std::uint8_t> decoded;
    EXPECT_FALSE(audient::vst3::Vst3StateBlob::decode(blob, decoded));
    EXPECT_TRUE(decoded.empty());
}

TEST(Vst3StateBlobTest, EncodeRejectsOversizedPayload)
{
    // Build a payload too large to fit within kMaxStateBytes without allocating
    // kMaxStateBytes in the test: we can't exceed it easily, so instead verify
    // the guard is present by a boundary probe only in a release-safe manner.
    // kMaxStateBytes is 4 MiB; allocating 4 MiB+1 is fine for a unit test.
    std::vector<std::uint8_t> oversized(audient::vst3::kMaxStateBytes + 1, 0x77);
    const std::vector<std::uint8_t> blob = audient::vst3::Vst3StateBlob::encode(oversized);
    EXPECT_TRUE(blob.empty()) << "encode must refuse a payload over kMaxStateBytes";
}

TEST(Vst3StateBlobTest, HeaderLayoutIsStableLeLittleEndian)
{
    const std::vector<std::uint8_t> payload = {0xDEu, 0xADu};
    const std::vector<std::uint8_t> blob = audient::vst3::Vst3StateBlob::encode(payload);
    EXPECT_EQ(blob[0], 0x41);           // 'A'
    EXPECT_EQ(blob[1], 0x56);           // 'V'
    EXPECT_EQ(blob[2], 0x53);           // 'S'
    EXPECT_EQ(blob[3], 0x54);           // 'T'
    EXPECT_EQ(blob[4], 0x01);           // version LE low byte
    EXPECT_EQ(blob[5], 0x00);           // version LE high byte
    EXPECT_EQ(blob[6], 0x00);           // reserved
    EXPECT_EQ(blob[7], 0x00);           // reserved
    EXPECT_EQ(blob[8], 0x02);           // payload size 2 LE
    EXPECT_EQ(blob[9], 0x00);           //
    EXPECT_EQ(blob[10], 0x00);          //
    EXPECT_EQ(blob[11], 0x00);          //
    EXPECT_EQ(blob[audient::vst3::Vst3StateBlob::kHeaderSize], 0xDE);
    EXPECT_EQ(blob[audient::vst3::Vst3StateBlob::kHeaderSize + 1], 0xAD);
}

TEST(Vst3StateBlobTest, DecodePayloadIsBitForBitStable)
{
    const std::string text = "state blob round-trip";
    const std::vector<std::uint8_t> payload(text.begin(), text.end());
    const std::vector<std::uint8_t> blob1 = audient::vst3::Vst3StateBlob::encode(payload);
    const std::vector<std::uint8_t> blob2 = audient::vst3::Vst3StateBlob::encode(payload);
    EXPECT_EQ(blob1, blob2);

    std::vector<std::uint8_t> decoded;
    ASSERT_TRUE(audient::vst3::Vst3StateBlob::decode(blob1, decoded));
    EXPECT_EQ(decoded, payload);
}