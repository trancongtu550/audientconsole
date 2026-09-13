#include <cstdio>
#include <cstring>

void runAllocationBench();
void runCallbackTimingBench();
void runRingBench();

int main(int argc, char** argv)
{
    const char* which = argc > 1 ? argv[1] : "all";

    if (std::strcmp(which, "alloc") == 0 || std::strcmp(which, "all") == 0)
    {
        runAllocationBench();
    }
    if (std::strcmp(which, "callback") == 0 || std::strcmp(which, "all") == 0)
    {
        runCallbackTimingBench();
    }
    if (std::strcmp(which, "ring") == 0 || std::strcmp(which, "all") == 0)
    {
        runRingBench();
    }

    if (std::strcmp(which, "alloc") != 0 && std::strcmp(which, "callback") != 0 &&
        std::strcmp(which, "ring") != 0 && std::strcmp(which, "all") != 0)
    {
        std::fprintf(stderr, "usage: audient_console_bench [alloc|callback|ring|all]\n");
        return 2;
    }
    return 0;
}