#include "bench/Bench.h"
#include "engine/RingBuffer.h"

#include <cstdio>
#include <vector>

void runRingBench()
{
    std::printf("AudientConsole benchmark: SPSC ring buffer transfer\n");

    audient::engine::LockFreeRingBuffer buffer(4096);
    std::vector<float> block(64, 0.5f);

    for (int i = 0; i < 100000; ++i)
    {
        buffer.writeBlock(block.data(), 64);
        buffer.readBlock(block.data(), 64);
    }
    buffer.reset();

    const audient::bench::TimingResult timing = audient::bench::measure(200000, [&]() {
        buffer.writeBlock(block.data(), 64);
        buffer.readBlock(block.data(), 64);
    });

    audient::bench::printTiming("write+read 64-sample block", timing);

    const audient::engine::LockFreeRingBuffer::Snapshot stats = buffer.snapshot();
    std::printf("ring buffer: produced=%llu consumed=%llu dropped=%llu underruns=%llu\n",
                static_cast<unsigned long long>(stats.produced),
                static_cast<unsigned long long>(stats.consumed),
                static_cast<unsigned long long>(stats.dropped),
                static_cast<unsigned long long>(stats.underrunFrames));
}