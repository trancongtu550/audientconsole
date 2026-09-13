#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3HostContext.h"
#include "vst3/Vst3ModuleLoader.h"
#include "vst3/Vst3Processor.h"

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

// Phase 4 gate — OFFLINE single licensed plug-in end-to-end (Bước 2 + Bước 3).
// No ASIO, no device: Vst3ModuleLoader -> Vst3Host -> Vst3Processor -> process()
// against a deterministic loud tone block, then teardown. These tests are
// environment-gated: they compile only when the plugin path was passed at
// configure time, and SKIP at runtime if the file is absent. Raw captures are
// not produced; no audio content is logged.
//
// Success criteria (offline):
//   - full lifecycle transitions succeed (prepare -> setProcessing -> process
//     -> terminate -> unload)
//   - output finite, no NaN/Inf
//   - correct frame count / channel count
//   - process() succeeds
//   - output differs from input when the effect engages at default state
//   - getLatencySamples() is reported in the record

namespace
{

bool isFiniteFloat(float value)
{
    return std::isfinite(value) != 0;
}

struct OfflineResult
{
    bool prepared = false;
    std::uint32_t latency = 0;
    std::size_t frames = 0;
    std::size_t changed = 0;
    std::size_t nonFinite = 0;
    float peak = 0.0f;
};

// Processes a fixed loud tone through the selected effect for N blocks (so a
// dynamics plug-in's envelope settles) and returns the validation record for
// the final block. Invalid input vector is a hard test failure.
OfflineResult runOffline(const std::string& pluginPath, audient::vst3::BusLayout layout)
{
    OfflineResult result;

    audient::vst3::Vst3ModuleLoader loader;
    if (!loader.load(pluginPath))
    {
        ADD_FAILURE() << "load failed: " << loader.lastError();
        return result;
    }

    audient::vst3::Vst3Host host;
    if (!host.attachFactory(loader.factory()))
    {
        ADD_FAILURE() << "attach failed";
        return result;
    }

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    const audient::vst3::PluginClassInfo* effect = nullptr;
    for (const audient::vst3::PluginClassInfo& cls : classes)
    {
        if (cls.isEffect())
        {
            effect = &cls;
            break;
        }
    }
    if (effect == nullptr)
    {
        ADD_FAILURE() << "no Audio Effect class found";
        return result;
    }

    const double sampleRate = 48000.0;
    const long blockSamples = 64;
    std::unique_ptr<audient::vst3::Vst3Processor> processor;
    try
    {
        processor = host.createEffectProcessor(*effect, sampleRate, blockSamples, layout);
    }
    catch (const std::exception& e)
    {
        ADD_FAILURE() << "createEffectProcessor threw: " << e.what();
        return result;
    }
    if (processor == nullptr || !processor->valid())
    {
        ADD_FAILURE() << "prepare failed: " << host.lastError();
        return result;
    }
    result.prepared = true;
    result.latency = processor->latencySamples();

    constexpr int kBlocks = 2000; // ~2.67 s at 48 kHz
    std::vector<float> tone(static_cast<std::size_t>(blockSamples));
    for (std::size_t i = 0; i < tone.size(); ++i)
    {
        tone[i] = 0.99f * std::sinf(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / static_cast<float>(sampleRate));
    }

    const bool stereo = layout == audient::vst3::BusLayout::Stereo;
    std::vector<float> monoOut(static_cast<std::size_t>(blockSamples), 0.0f);
    std::vector<float> leftOut(static_cast<std::size_t>(blockSamples), 0.0f);
    std::vector<float> rightOut(static_cast<std::size_t>(blockSamples), 0.0f);
    std::vector<float> rightIn(tone);

    for (int block = 0; block < kBlocks; ++block)
    {
        if (stereo)
        {
            std::fill(leftOut.begin(), leftOut.end(), 0.0f);
            std::fill(rightOut.begin(), rightOut.end(), 0.0f);
            audient::vst3::Vst3Processor::chainProcessStereo(tone.data(), rightIn.data(), leftOut.data(),
                                                            rightOut.data(), tone.size(), processor.get());
        }
        else
        {
            std::fill(monoOut.begin(), monoOut.end(), 0.0f);
            audient::vst3::Vst3Processor::chainProcess(tone.data(), monoOut.data(), tone.size(), processor.get());
        }
    }

    const std::vector<float>& out = stereo ? leftOut : monoOut;
    result.frames = out.size();
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        if (!isFiniteFloat(out[i]))
        {
            ++result.nonFinite;
            continue;
        }
        result.peak = std::max(result.peak, std::fabs(out[i]));
        if (std::fabs(out[i] - tone[i]) > 1e-6f)
        {
            ++result.changed;
        }
    }

    // Teardown: processor destructor does setProcessing(false)+terminate;
    // detach the factory and unload the module explicitly.
    processor.reset();
    host.detachFactory();
    loader.unload();
    return result;
}

} // namespace

#ifdef TEST_REAL_PLUGIN_SOFTUBE_CL1B_PATH

TEST(Vst3RealPluginOfflineTest, SoftubeCL1B_FullLifecycleAndOfflineProcessing)
{
    const std::string pluginPath = TEST_REAL_PLUGIN_SOFTUBE_CL1B_PATH;
    if (!std::filesystem::exists(pluginPath))
    {
        GTEST_SKIP() << "Softube Tube-Tech CL 1B not installed at configure-time path: " << pluginPath;
    }

    // v1 output chain layout (stereo) is the intended downlink shape.
    const OfflineResult result = runOffline(pluginPath, audient::vst3::BusLayout::Stereo);

    EXPECT_TRUE(result.prepared);
    EXPECT_EQ(result.frames, 64u) << "frame count must be preserved";
    EXPECT_EQ(result.nonFinite, 0u) << "output must contain no NaN/Inf";
    EXPECT_GT(result.changed, 0u) << "a licensed compressor must not be a silent/passthrough at default state";
    EXPECT_GT(result.peak, 0.0f) << "output must be present";
    // Reported latency is documented as part of the record (0 is legal for a
    // zero-latency mode; a non-zero value must be reported as-is).
    EXPECT_GE(result.latency, 0u);
    EXPECT_LT(result.latency, 4096u) << "unexpectedly large reported latency";
}

TEST(Vst3RealPluginOfflineTest, SoftubeCL1B_MonoMicChainLifecycle)
{
    const std::string pluginPath = TEST_REAL_PLUGIN_SOFTUBE_CL1B_PATH;
    if (!std::filesystem::exists(pluginPath))
    {
        GTEST_SKIP() << "Softube Tube-Tech CL 1B not installed at configure-time path: " << pluginPath;
    }

    const OfflineResult result = runOffline(pluginPath, audient::vst3::BusLayout::Mono);
    EXPECT_TRUE(result.prepared);
    EXPECT_EQ(result.frames, 64u);
    EXPECT_EQ(result.nonFinite, 0u);
    EXPECT_GT(result.changed, 0u);
}

// Proves add / per-slot bypass / whole-chain bypass / remove on the real
// licensed plug-in through a Vst3Chain (the exact path the demo UI uses).
TEST(Vst3RealPluginOfflineTest, SoftubeCL1B_ChainBypassAndSlotMutation)
{
    const std::string pluginPath = TEST_REAL_PLUGIN_SOFTUBE_CL1B_PATH;
    if (!std::filesystem::exists(pluginPath))
    {
        GTEST_SKIP() << "Softube Tube-Tech CL 1B not installed at configure-time path: " << pluginPath;
    }

    audient::vst3::Vst3ModuleLoader loader;
    ASSERT_TRUE(loader.load(pluginPath)) << loader.lastError();
    audient::vst3::Vst3Host host;
    ASSERT_TRUE(host.attachFactory(loader.factory()));

    const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
    const audient::vst3::PluginClassInfo* effect = nullptr;
    for (const audient::vst3::PluginClassInfo& cls : classes)
    {
        if (cls.isEffect())
        {
            effect = &cls;
            break;
        }
    }
    ASSERT_NE(effect, nullptr);

    const double sampleRate = 48000.0;
    const long blockSamples = 64;
    std::unique_ptr<audient::vst3::Vst3Processor> processor =
        host.createEffectProcessor(*effect, sampleRate, blockSamples, audient::vst3::BusLayout::Mono);
    ASSERT_TRUE(processor != nullptr && processor->valid()) << host.lastError();

    audient::vst3::Vst3Chain chain;
    std::vector<float> tone(static_cast<std::size_t>(blockSamples));
    for (std::size_t i = 0; i < tone.size(); ++i)
    {
        tone[i] = 0.99f * std::sinf(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / static_cast<float>(sampleRate));
    }
    // The engine graph calls the chain in-place (input == output buffer), so a
    // bypassed or empty chain must leave the buffer exactly as it arrived.
    std::vector<float> io(tone);

    const auto countChanged = [&]() {
        std::size_t changed = 0;
        for (std::size_t i = 0; i < io.size(); ++i)
        {
            if (std::fabs(io[i] - tone[i]) > 1e-6f && std::isfinite(io[i]))
            {
                ++changed;
            }
        }
        return changed;
    };

    // Add: publish a single active slot, run enough blocks, expect a processed
    // (non-passthrough) result at default state.
    audient::vst3::Vst3Chain::Slot slot{processor.get(), false};
    EXPECT_TRUE(chain.publish({slot}, false));
    for (int block = 0; block < 300; ++block)
    {
        io = tone;
        audient::vst3::Vst3Chain::processMono(io.data(), io.data(), io.size(), &chain);
    }
    EXPECT_GT(countChanged(), 0u) << "active CL 1B slot must modify audio";

    // Per-slot bypass: same slot, bypass on -> in-place passthrough.
    slot.bypass = true;
    EXPECT_TRUE(chain.publish({slot}, false));
    for (int block = 0; block < 64; ++block)
    {
        io = tone;
        audient::vst3::Vst3Chain::processMono(io.data(), io.data(), io.size(), &chain);
    }
    EXPECT_EQ(countChanged(), 0u) << "bypassed slot must be true passthrough";

    // Whole-chain bypass: even an active slot is skipped.
    slot.bypass = false;
    EXPECT_TRUE(chain.publish({slot}, true));
    for (int block = 0; block < 64; ++block)
    {
        io = tone;
        audient::vst3::Vst3Chain::processMono(io.data(), io.data(), io.size(), &chain);
    }
    EXPECT_EQ(countChanged(), 0u) << "whole-chain bypass must be true passthrough";

    // Remove: republish an empty chain. The removed processor must not be
    // destroyed until the chain settles (no callback can still hold the old
    // snapshot that referenced it).
    EXPECT_TRUE(chain.publish({}, false));
    while (!chain.settled())
    {
        io = tone;
        audient::vst3::Vst3Chain::processMono(io.data(), io.data(), io.size(), &chain);
    }
    chain.reap();
    EXPECT_TRUE(chain.settled());

    processor.reset();
    host.detachFactory();
    loader.unload();
}

#endif // TEST_REAL_PLUGIN_SOFTUBE_CL1B_PATH

#ifdef TEST_REAL_PLUGIN_TUBECHILD670_PATH

TEST(Vst3RealPluginOfflineTest, Tubechild670_FullLifecycle)
{
    const std::string pluginPath = TEST_REAL_PLUGIN_TUBECHILD670_PATH;
    if (!std::filesystem::exists(pluginPath))
    {
        GTEST_SKIP() << "AA_Tubechild670 not installed at configure-time path: " << pluginPath;
    }

    // Recorded observation (2026-09-03): AA Tubechild670 v1.0.1 default state is
    // unity passthrough — process() succeeds and output is finite but EQUAL to
    // input, so we assert lifecycle + finiteness only, and log the passthrough
    // fact. "output != input" for Tubechild requires parameter/preset changes
    // (VST-009 / state blobs), which are later slices by design.
    const OfflineResult result = runOffline(pluginPath, audient::vst3::BusLayout::Mono);
    EXPECT_TRUE(result.prepared);
    EXPECT_EQ(result.frames, 64u);
    EXPECT_EQ(result.nonFinite, 0u) << "output must contain no NaN/Inf";
    EXPECT_LE(result.changed, 0u) << "recorded default-state passthrough must hold at this plugin version";
    EXPECT_EQ(result.latency, 0u);
}

#endif // TEST_REAL_PLUGIN_TUBECHILD670_PATH

#ifdef TEST_REAL_PLUGIN_FABFILTER_Q4_PATH

// VST-018 investigation record (diagnostic): run the full load/prepare/
// negotiation sequence against the REAL FabFilter Pro-Q 4 (per vendor docs it
// supports mono AND stereo) and print the deterministic prepare trace — the
// exact setBusArrangements call made and, on failure, the retained layout the
// plugin holds. This documents WHERE the current host contract diverges; it
// asserts only that the plugin loads and a trace was produced (success/failure
// is recorded, not hard-coded).
TEST(Vst3RealPluginOfflineTest, FabFilterProQ4_PrepareNegotiationTrace)
{
    const std::string pluginPath = TEST_REAL_PLUGIN_FABFILTER_Q4_PATH;
    if (!std::filesystem::exists(pluginPath))
    {
        GTEST_SKIP() << "FabFilter Pro-Q 4 not installed at configure-time path: " << pluginPath;
    }

    const char* phase = "<start>";
    try
    {
        audient::vst3::Vst3ModuleLoader loader;
        phase = "loader.load";
        ASSERT_TRUE(loader.load(pluginPath)) << loader.lastError();

        audient::vst3::Vst3Host host;
        phase = "host.attachFactory";
        ASSERT_TRUE(host.attachFactory(loader.factory()));

        phase = "host.classes (getClassInfo enumeration)";
        const std::vector<audient::vst3::PluginClassInfo> classes = host.classes();
        const audient::vst3::PluginClassInfo* effect = nullptr;
        for (const audient::vst3::PluginClassInfo& cls : classes)
        {
            if (cls.isEffect())
            {
                effect = &cls;
                break;
            }
        }
        ASSERT_NE(effect, nullptr) << "no Audio Effect class found in FabFilter Pro-Q 4";

        phase = "createEffectProcessor (createInstance + prepare)";
        auto processor = host.createEffectProcessor(*effect, 48000.0, 64, audient::vst3::BusLayout::Mono);
        phase = "inspect result";

        const std::string& trace = processor != nullptr ? processor->prepareTrace() : host.prepareTrace();
        const std::string outcome = processor != nullptr
            ? (processor->valid() ? "PREPARE OK" : "PREPARE FAILED (non-valid processor returned)")
            : ("PREPARE FAILED (host error: " + host.lastError() + ")");
        std::printf("\n[VST-018] FabFilter Pro-Q 4 mono-request outcome: %s\n%s\n[VST-018] trace-end\n",
                    outcome.c_str(), trace.c_str());
        std::fflush(stdout);

        EXPECT_FALSE(trace.empty()) << "the prepare negotiation trace must be captured";
        if (processor != nullptr)
        {
            // RESOLVED (VST-018, 2026-09-04): with bus-count-correct
            // setBusArrangements + native-evidence stereo retry, Pro-Q 4 prepares
            // OK as a stereo processor on the mono mic-chain request and the
            // VST-006 mono<->stereo adapter drives it.
            EXPECT_TRUE(processor->valid()) << processor->lastPrepareError();
            EXPECT_EQ(processor->layout(), audient::vst3::BusLayout::Stereo);
        }

        processor.reset();
        host.detachFactory();
        loader.unload();
    }
    catch (const std::exception& exception)
    {
        std::printf("\n[VST-018] FabFilter Pro-Q 4 threw a C++ exception at phase '%s': %s\n", phase, exception.what());
        std::fflush(stdout);
        FAIL() << "FabFilter Pro-Q 4 threw during phase '" << phase << "': " << exception.what();
    }
    catch (...)
    {
        std::printf("\n[VST-018] FabFilter Pro-Q 4 threw an unknown C++ exception at phase '%s'\n", phase);
        std::fflush(stdout);
        FAIL() << "FabFilter Pro-Q 4 threw an unknown C++ exception during phase '" << phase << "'";
    }
}

#endif // TEST_REAL_PLUGIN_FABFILTER_Q4_PATH