#include "id14-control/protocol/Id14CommandWhitelist.h"
#include "id14-control/semantic-model/Id14SemanticControl.h"
#include "id14-control/simulator/SimulatedId14Transport.h"
#include "id14-control/transport/Id14ControlTransport.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace audient::id14;

TEST(Id14SemanticControlTest, EveryControlHasASpec)
{
    EXPECT_GT(controlCount(), 0u);
    EXPECT_NE(specFor(Id14ControlKind::SpeakerLevel), nullptr);
    EXPECT_NE(specFor(Id14ControlKind::CueMute), nullptr);
    EXPECT_NE(specFor(Id14ControlKind::IdButtonMode), nullptr);
}

TEST(Id14SemanticControlTest, SpeakerVolumeRangeIsNegativeToZeroDbStepOne)
{
    const ControlSpec* spec = specFor(Id14ControlKind::SpeakerLevel);
    ASSERT_NE(spec, nullptr);
    EXPECT_NEAR(spec->min, -80.0f, 1e-6f);
    EXPECT_NEAR(spec->max, 0.0f, 1e-6f);
    EXPECT_NEAR(spec->step, 1.0f, 1e-6f);
    EXPECT_TRUE(spec->isPersistent);
    EXPECT_TRUE(spec->isAudiblyRisky);
    EXPECT_TRUE(spec->hasRollback);
}

TEST(Id14SemanticControlTest, ValueRangeAndStepAlignment)
{
    EXPECT_TRUE(valueInRange(Id14ControlKind::SpeakerLevel, -12.0f));
    EXPECT_FALSE(valueInRange(Id14ControlKind::SpeakerLevel, 2.0f));
    EXPECT_FALSE(valueInRange(Id14ControlKind::SpeakerLevel, -90.0f));
    EXPECT_TRUE(alignedToStep(Id14ControlKind::SpeakerLevel, -12.0f));
    EXPECT_FALSE(alignedToStep(Id14ControlKind::SpeakerLevel, -12.5f));
}

TEST(Id14SemanticControlTest, ClampAndSnapStayInsideSpec)
{
    const float clamped = clampToRange(Id14ControlKind::SpeakerLevel, -200.0f);
    EXPECT_NEAR(clamped, -80.0f, 1e-6f);
    const float snapped = snapToStep(Id14ControlKind::SpeakerLevel, -12.3f);
    EXPECT_TRUE(alignedToStep(Id14ControlKind::SpeakerLevel, snapped));
    EXPECT_TRUE(valueInRange(Id14ControlKind::SpeakerLevel, snapped));
}

TEST(Id14SemanticControlTest, NothingIsSupportedOnTheExactMk1TupleYet)
{
    for (std::size_t i = 0; i < controlCount(); ++i)
    {
        // validated indirectly through the spec table; explicit kinds below
    }
    const std::vector<Id14ControlKind> kinds = {
        Id14ControlKind::SpeakerLevel,      Id14ControlKind::SpeakerMute,
        Id14ControlKind::Dim,               Id14ControlKind::Mono,
        Id14ControlKind::HeadphoneLevel,    Id14ControlKind::CueSource,
        Id14ControlKind::OutputAssignment,  Id14ControlKind::TalkbackLevel,
    };
    for (const Id14ControlKind kind : kinds)
    {
        EXPECT_EQ(supportFor(kind, Id14ControlTuple::Mk1Current), Id14SupportClass::Unverified)
            << "no MK1 control may be marked supported before protocol evidence";
    }
}

TEST(Id14CommandWhitelistTest, EmptyWhitelistRejectsEverything)
{
    Id14CommandWhitelist whitelist;
    EXPECT_EQ(whitelist.size(), 0u);

    EXPECT_FALSE(whitelist.isAllowed(Id14ControlKind::SpeakerLevel, 0u, 4u));
    EXPECT_FALSE(whitelist.isAllowed(Id14ControlKind::SpeakerMute, 1u, 1u));
    EXPECT_FALSE(whitelist.isWritable(Id14ControlKind::SpeakerLevel));
    EXPECT_FALSE(whitelist.isWritable(Id14ControlKind::CueMute));
}

TEST(Id14ControlTransportTest, SimulatedSubmitReturnsOkAndAcks)
{
    SimulatedId14Transport transport;
    std::uint64_t ackedSequence = 0;
    TransportResult ackedResult = TransportResult::Other;
    transport.setAckHandler([&](std::uint64_t sequence, TransportResult result) {
        ackedSequence = sequence;
        ackedResult = result;
    });

    ControlWrite write;
    write.kind = Id14ControlKind::SpeakerLevel;
    write.value = -12.0f;
    const TransportResult result = transport.submit(write, nullptr);

    EXPECT_EQ(result, TransportResult::Ok);
    EXPECT_EQ(transport.submittedCount(), 1u);
    EXPECT_TRUE(transport.connected());
    EXPECT_FALSE(transport.deviceFingerprint().empty());
    EXPECT_EQ(ackedSequence, 1u);
    EXPECT_EQ(ackedResult, TransportResult::Ok);
    EXPECT_NEAR(transport.lastValuesByKind()[Id14ControlKind::SpeakerLevel], -12.0f, 1e-6f);
}

TEST(Id14ControlTransportTest, DisconnectRejectsSubmits)
{
    SimulatedId14Transport transport;
    transport.setAckHandler(nullptr);
    transport.simulateDisconnect();

    ControlWrite write;
    write.kind = Id14ControlKind::SpeakerMute;
    write.value = 1.0f;
    EXPECT_EQ(transport.submit(write, nullptr), TransportResult::Disconnected);
    EXPECT_FALSE(transport.connected());
}

TEST(Id14ControlTransportTest, ReconnectRequiresFingerprintThenAccepts)
{
    SimulatedId14Transport transport;
    transport.simulateDisconnect();
    EXPECT_FALSE(transport.connected());

    transport.simulateReconnect(transport.deviceFingerprint());
    EXPECT_TRUE(transport.connected());

    ControlWrite write;
    write.kind = Id14ControlKind::Dim;
    write.value = -12.0f;
    EXPECT_EQ(transport.submit(write, nullptr), TransportResult::Ok);
}

TEST(Id14ControlTransportTest, DeviceMismatchPropagatesAsResult)
{
    SimulatedId14Transport transport;
    transport.setNextResult(TransportResult::DeviceMismatch);

    ControlWrite write;
    write.kind = Id14ControlKind::SpeakerLevel;
    write.value = 0.0f;

    TransportResult result = TransportResult::Ok;
    ControlAckHandler handler = [&](std::uint64_t, TransportResult r) { result = r; };
    EXPECT_EQ(transport.submit(write, std::move(handler)), TransportResult::DeviceMismatch);
    EXPECT_EQ(result, TransportResult::DeviceMismatch);
}

TEST(Id14ControlTransportTest, ReleaseOwnershipClearsState)
{
    SimulatedId14Transport transport;
    ControlWrite write;
    write.kind = Id14ControlKind::SpeakerLevel;
    write.value = -6.0f;
    EXPECT_EQ(transport.submit(write, nullptr), TransportResult::Ok);

    std::string error;
    transport.releaseDeviceOwnership(error);
    EXPECT_FALSE(transport.connected());
    EXPECT_EQ(transport.deviceFingerprint().size(), 0u);
    EXPECT_TRUE(transport.lastValuesByKind().empty()) << "last values cleared after release";
}

TEST(Id14ControlTransportTest, ResultNamesAreStable)
{
    EXPECT_STREQ(transportResultName(TransportResult::NotSupported), "not-supported");
    EXPECT_STREQ(transportResultName(TransportResult::InvalidValue), "invalid-value");
    EXPECT_STREQ(transportResultName(TransportResult::DeviceMismatch), "device-mismatch");
}