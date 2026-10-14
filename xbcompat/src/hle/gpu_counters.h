/* XBCOMPAT_GPU_COUNTERS=1: the Pi GPU's own counters (Mesa's VC4 driver
   offers them through GL_AMD_performance_monitor, group "V3D counters"),
   summed over 60 frames and logged per frame.  They split the GPU's time
   between vertex and fragment shading, texture waits and idling (idle QPUs
   while the CPU waits on the GPU means the time goes to work outside the
   shader cores: tile loads and stores, binning, memory).  Included by
   d3d8.c; called once per frame from Swap. */

#define GPU_COUNTER_FRAMES 60

static void gpu_counters_frame(void)
{
    static int state;   /* 0 unread, 1 off, 2 ready, 3 counting, 4 waiting for the result */
    static GLuint monitor, group, counters[16];
    static unsigned ncounters, started;
    static char names[16][48];
    static uint64_t t_begin;
    static void (APIENTRY *GetGroups)(GLint *, GLsizei, GLuint *);
    static void (APIENTRY *GetCounters)(GLuint, GLint *, GLint *, GLsizei, GLuint *);
    static void (APIENTRY *GetGroupString)(GLuint, GLsizei, GLsizei *, char *);
    static void (APIENTRY *GetCounterString)(GLuint, GLuint, GLsizei, GLsizei *, char *);
    static void (APIENTRY *Gen)(GLsizei, GLuint *);
    static void (APIENTRY *Select)(GLuint, GLboolean, GLuint, GLint, GLuint *);
    static void (APIENTRY *Begin)(GLuint);
    static void (APIENTRY *End)(GLuint);
    static void (APIENTRY *GetData)(GLuint, GLenum, GLsizei, GLuint *, GLint *);
    if (state == 1) return;
    if (state == 0) {
        state = 1;
        const char *e = getenv("XBCOMPAT_GPU_COUNTERS");
        if (!e || !*e || *e == '0') return;
        GetGroups = SDL_GL_GetProcAddress("glGetPerfMonitorGroupsAMD");
        GetCounters = SDL_GL_GetProcAddress("glGetPerfMonitorCountersAMD");
        GetGroupString = SDL_GL_GetProcAddress("glGetPerfMonitorGroupStringAMD");
        GetCounterString = SDL_GL_GetProcAddress("glGetPerfMonitorCounterStringAMD");
        Gen = SDL_GL_GetProcAddress("glGenPerfMonitorsAMD");
        Select = SDL_GL_GetProcAddress("glSelectPerfMonitorCountersAMD");
        Begin = SDL_GL_GetProcAddress("glBeginPerfMonitorAMD");
        End = SDL_GL_GetProcAddress("glEndPerfMonitorAMD");
        GetData = SDL_GL_GetProcAddress("glGetPerfMonitorCounterDataAMD");
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        if (!ext || !strstr(ext, "GL_AMD_performance_monitor") || !GetGroups || !GetCounters || !GetGroupString
            || !GetCounterString || !Gen || !Select || !Begin || !End || !GetData) {
            xlog("D3D GPU counters: GL_AMD_performance_monitor is not available");
            return;
        }
        GLint ngroups = 0;
        GLuint groups[8];
        GetGroups(&ngroups, 8, groups);
        bool found = false;
        for (GLint g = 0; g < ngroups && g < 8 && !found; g++) {
            char name[64] = "";
            GetGroupString(groups[g], sizeof name, NULL, name);
            if (strstr(name, "V3D")) { group = groups[g]; found = true; }
        }
        if (!found) { xlog("D3D GPU counters: no V3D counter group (%d groups)", ngroups); return; }
        static const char *const want[] = {
            "QPU-total-idle-clk-cycles", "QPU-total-clk-cycles-vertex-coord-shading",
            "QPU-total-clk-cycles-fragment-shading", "QPU-total-clk-cycles-executing-valid-instr",
            "QPU-total-clk-cycles-waiting-TMU", "QPU-total-clk-cycles-waiting-scoreboard",
            "QPU-total-clk-cycles-waiting-varyings", "QPU-total-instr-cache-miss",
            "TMU-total-text-quads-processed", "TMU-total-text-cache-miss",
            "VPM-total-clk-cycles-VDW-stalled", "VPM-total-clk-cycles-VCD-stalled",
            "FEP-valid-quads", "TLB-quads-written-to-color-buffer",
            "PTB-primitives-need-clipping", "L2C-total-cache-miss",
        };
        GLint n = 0, max_active = 0;
        GLuint all[64];
        GetCounters(group, &n, &max_active, 64, all);
        for (unsigned w = 0; w < 16; w++)
            for (GLint c = 0; c < n && c < 64; c++) {
                char name[64] = "";
                GetCounterString(group, all[c], sizeof name, NULL, name);
                if (!strcmp(name, want[w]) && ncounters < 16 && (GLint)ncounters < max_active) {
                    counters[ncounters] = all[c];
                    snprintf(names[ncounters], sizeof names[0], "%s", name);
                    ncounters++;
                    break;
                }
            }
        if (!ncounters) { xlog("D3D GPU counters: none of the wanted counters exist"); return; }
        Gen(1, &monitor);
        Select(monitor, GL_TRUE, group, ncounters, counters);
        xlog("D3D GPU counters: %u counters, reported every %u frames", ncounters, GPU_COUNTER_FRAMES);
        state = 2;
    }
    if (state == 2) {
        Begin(monitor);
        started = d3d.frame;
        t_begin = now_ns();
        state = 3;
        return;
    }
    if (state == 3) {
        if (d3d.frame - started < GPU_COUNTER_FRAMES) return;
        End(monitor);
        state = 4;
    }
    /* state 4: read once the GPU has finished the frames counted. */
    GLuint avail = 0;
    GetData(monitor, GL_PERFMON_RESULT_AVAILABLE_AMD, sizeof avail, &avail, NULL);
    if (!avail) return;
    GLuint data[16 * 4];
    GLint bytes = 0;
    GetData(monitor, GL_PERFMON_RESULT_AMD, sizeof data, data, &bytes);
    double frames = d3d.frame - started, ms = (now_ns() - t_begin) / 1e6 / frames;
    xlog("D3D GPU counters over %.0f frames (%.1f ms a frame), per frame:", frames, ms);
    /* Each entry: group, counter, then the 64-bit value. */
    for (GLint i = 0; i + 4 <= bytes / 4; i += 4) {
        uint64_t v = (uint64_t)data[i + 2] | (uint64_t)data[i + 3] << 32;
        const char *name = "?";
        for (unsigned k = 0; k < ncounters; k++) if (counters[k] == data[i + 1]) name = names[k];
        xlog("D3D GPU counters:   %-44s %14.0f", name, v / frames);
    }
    state = 2;
}
