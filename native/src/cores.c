// How many threads the cleanup model computes with on the CPU: the cores that do arithmetic at
// full speed, which is what llama.cpp's own programs and node-llama-cpp use. Logical processors
// that share a core add nothing to matrix arithmetic, and a count that differs from the Electron
// build's also changes the rounding of the sums, and with it now and then the word sampled.
#include <SDL3/SDL.h>
#include "cores.h"
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <stdio.h>
#endif

int eh_math_cores(void) {
    int cores = 0;
#ifdef _WIN32
    DWORD size = 0;
    GetLogicalProcessorInformation(NULL, &size);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info = SDL_malloc(size ? size : 1);
    if (size && GetLogicalProcessorInformation(info, &size))
        for (DWORD i = 0; i < size / sizeof *info; i++) cores += info[i].Relationship == RelationProcessorCore;
    SDL_free(info);
#elif defined(__APPLE__)
    // The performance cores, where the system tells them apart.
    size_t len = sizeof cores;
    if (sysctlbyname("hw.perflevel0.physicalcpu", &cores, &len, NULL, 0) || cores <= 0) {
        len = sizeof cores;
        if (sysctlbyname("hw.physicalcpu", &cores, &len, NULL, 0)) cores = 0;
    }
#else
    // One per distinct (package, core) pair among the processors the system lists.
    enum { MAX = 4096 };
    static int seen[MAX][2];
    for (int cpu = 0; cpu < MAX; cpu++) {
        char path[128];
        int ids[2] = { -1, -1 };
        static const char *const names[] = { "physical_package_id", "core_id" };
        for (int k = 0; k < 2; k++) {
            SDL_snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/%s", cpu, names[k]);
            FILE *f = fopen(path, "r");
            if (!f || fscanf(f, "%d", &ids[k]) != 1) ids[k] = -1;
            if (f) fclose(f);
        }
        if (ids[1] < 0) break;
        bool known = false;
        for (int i = 0; i < cores && !known; i++) known = seen[i][0] == ids[0] && seen[i][1] == ids[1];
        if (!known) { seen[cores][0] = ids[0]; seen[cores][1] = ids[1]; cores++; }
    }
#endif
    return cores > 0 ? cores : SDL_max(1, SDL_GetNumLogicalCPUCores() / 2);
}
