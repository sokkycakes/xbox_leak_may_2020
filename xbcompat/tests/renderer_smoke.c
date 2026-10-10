/* Real renderer through client.c + generated HLE stubs. No display needed.
 * Can run directly with the i386 backend or through Box86 with armhf backend.
 */
#include "xbcompat.h"
#include "hle/hle.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <GL/gl.h>
int g_trace, g_screenshot_frame, g_exit_after_frames;
const char *g_screenshot_path;
volatile unsigned g_guest_traps;
void (*g_vblank_hook)(void);
static unsigned reset_combos, reset_checks, vblanks, event_signals;
static KEVENT *last_event;
void xinput_check_reset_combo(void) { reset_combos++; }
void reset_check(void) { reset_checks++; }
LONG NTAPI KeSetEvent(KEVENT *event, LONG increment, BOOLEAN wait)
{
    assert(increment == 1 && wait == 0);
    LONG old = event->Header.SignalState;
    event->Header.SignalState = 1;
    last_event = event; event_signals++; return old;
}
static uint32_t render_states[256], texture_states[128];
static unsigned lookup_calls, alloc_calls, callbacks;
static unsigned char *contig;
static size_t contig_used;
void xlog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
void fatal(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort();
}
void xbc_exit(int code) { exit(code); }
void *pool_alloc(size_t n) { alloc_calls++; return calloc(1,n); }
void pool_free(void *p) { free(p); }
bool pool_owns(const void *p) { return p != NULL; }
void *arena_alloc(size_t n, ULONG top) { (void)top; return pool_alloc(n); }
PVOID NTAPI MmAllocateContiguousMemoryEx(SIZE_T n, ULONG_PTR lo, ULONG_PTR hi, ULONG_PTR align, ULONG flags)
{
    (void)lo; (void)hi; (void)align; (void)flags;
    assert(contig_used + n < 1024*1024);
    void *p = contig + contig_used;
    contig_used += (n + 4095) & ~4095u;
    return p;
}
void NTAPI MmFreeContiguousMemory(PVOID p) { (void)p; }
SIZE_T NTAPI MmQueryAllocationSize(PVOID p) { (void)p; return 256; }
void ke_frame_presented(void) {}
void install_fault_handlers(void) {}
void av_title_starting(void) {}
void av_hand_over(void) {}
void av_persist(const unsigned char *p, unsigned w, unsigned h) { (void)p; (void)w; (void)h; }
ULONG hle_lookup(const char *name)
{
    lookup_calls++;
    if (!strcmp(name, "_D3D__RenderState")) return (ULONG)render_states;
    if (!strcmp(name, "_D3D__TextureState")) return (ULONG)texture_states;
    return 0;
}
ULONG hle_lookup_prefix(const char *name) { (void)name; lookup_calls++; return 0; }
static void *find(const char *name)
{
    for (const struct hle_func *f = d3d8_funcs; f->name; f++)
        if (!strcmp(f->name, name)) return f->impl;
    fatal("missing test entry %s", name);
}
static void (NTAPI *get_scale)(float *, float *);
static void CDECLAPI callback(ULONG ctx)
{
    assert(ctx == 0x12345678);
    float x, y;
    get_scale(&x, &y); /* Native -> x86 -> native reentrancy. */
    assert(x == 1.25f && y == 0.75f);
    callbacks++;
}
static void CDECLAPI vblank_callback(ULONG *data)
{
    assert(data[0] == ++vblanks);
    assert(d3d_frame_count() == data[1]); /* Reenter renderer from DPC callback. */
}
int main(int argc, char **argv)
{
    /* Device setup reads the loaded XBE header to select its XDK layout. */
    void *header = mmap((void *)0x10000, 4096, PROT_READ|PROT_WRITE,
                        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
    assert(header == (void *)0x10000);
    contig = mmap((void *)0x81000000, 1024*1024, PROT_READ|PROT_WRITE,
                  MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0);
    assert(contig == (void *)0x81000000);
    d3d_bind_globals();
    assert(lookup_calls >= 5);
    assert(g_vblank_hook);
    g_vblank_hook(); /* Before CreateDevice: must safely do nothing. */
    assert(!vblanks && !event_signals);
    void (NTAPI *set_scale)(float,float) = find("_D3DDevice_SetBackBufferScale@8");
    get_scale = find("_D3DDevice_GetBackBufferScale@8");
    set_scale(1.25f,0.75f);
    float x,y; get_scale(&x,&y); assert(x == 1.25f && y == 0.75f);
    /* Guest inline writes must be visible to the native renderer. */
    void (NTAPI *get_rs)(ULONG,ULONG *) = find("_D3DDevice_GetRenderState@8");
    render_states[58] = 0x1234;
    ULONG state = 0; get_rs(58,&state); assert(state == 0x1234);
    void (FASTCALL *set_const)(LONG,const float *,ULONG) =
        find("@D3DDevice_SetVertexShaderConstantNotInline@12");
    void (NTAPI *get_const)(LONG,float *,ULONG) = find("_D3DDevice_GetVertexShaderConstant@12");
    float v[4] = {1.25f,-2.5f,3.75f,4.0f}, out[4];
    set_const(96,v,4); get_const(0,out,1); assert(!memcmp(v,out,sizeof(v)));
    void *(NTAPI *create_vb)(ULONG) = find("_D3DDevice_CreateVertexBuffer2@4");
    void (NTAPI *lock_vb)(void *,ULONG,ULONG,void **,ULONG) = find("_D3DVertexBuffer_Lock@20");
    void *vb = create_vb(256), *data = NULL;
    assert(vb && alloc_calls);
    lock_vb(vb,4,16,&data,0);
    assert(data == contig+4);
    memset(data,0x5a,16); assert(contig[4] == 0x5a);
    void (NTAPI *insert)(ULONG,void *,ULONG) = find("_D3DDevice_InsertCallback@12");
    for (int i=0;i<65;i++) insert(0,callback,0x12345678);
    assert(callbacks == 64); /* 65th insert flushes the first 64 without GL. */
    for (int i=0;i<10000;i++) {
        set_const(96,v,4); get_const(0,out,1); assert(!memcmp(v,out,sizeof(v)));
    }

    if (argc > 1 && !strcmp(argv[1], "--render")) {
        /* Actual GL work through the split backend, with pixel verification. */
        setenv("XBCOMPAT_MINIPORT_OFFSET", "0x1db4", 1);
        uint32_t pp[21] = {64,64,6,1,0,0,0,1,1,0x2a};
        void *device = NULL;
        LONG (NTAPI *create)(ULONG,ULONG,void *,ULONG,void *,void **) =
            find("_Direct3D_CreateDevice@24");
        assert(create(0,1,NULL,0,pp,&device) == 0 && device);
        void (NTAPI *set_vblank)(void *) = find("_D3DDevice_SetVerticalBlankCallback@4");
        set_vblank(vblank_callback);
        g_vblank_hook();
        assert(vblanks == 1 && event_signals == 1);
        assert(last_event == (KEVENT *)((char *)device + 0x1dbc));
        assert(last_event->Header.SignalState == 1);
        last_event->Header.SignalState = 0;
        g_vblank_hook();
        assert(vblanks == 2 && event_signals == 2 && last_event->Header.SignalState == 1);
        void (NTAPI *clear)(ULONG,void *,ULONG,ULONG,float,ULONG) =
            find("_D3DDevice_Clear@24");
        void (NTAPI *set_vs)(ULONG) = find("_D3DDevice_SetVertexShader@4");
        void (NTAPI *draw)(ULONG,ULONG,const void *,ULONG) =
            find("_D3DDevice_DrawVerticesUP@16");
        ULONG (NTAPI *swap)(ULONG) = find("_D3DDevice_Swap@4");
        clear(0,NULL,0xf3,0xff000000,1.0f,0);
        /* Guest inline render-state updates, read by native draw setup. */
        render_states[59] = 0;  /* alpha blending */
        render_states[60] = 0;  /* alpha test */
        render_states[92] = 0;  /* lighting */
        render_states[124] = 0; /* depth test */
        render_states[128] = 1; /* cull none */
        set_vs(0x44); /* XYZRHW + diffuse */
        struct vertex { float x,y,z,w; uint32_t color; };
        struct vertex triangle[] = {
            {4,4,0.5f,1,0xffff0000}, {60,4,0.5f,1,0xffff0000}, {32,60,0.5f,1,0xffff0000}
        };
        draw(5,3,triangle,sizeof(triangle[0]));
        unsigned char pixel[4] = {0};
        glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        assert(pixel[0] > 240 && pixel[1] < 8 && pixel[2] < 8);
        /* Authored mip levels must be uploaded and selected by MIPFILTER.
         * A two-level 64x64 chain deliberately stops before 1x1: without
         * MAX_LEVEL it is incomplete, and without mip uploads it stays red. */
        void *(NTAPI *create_tex)(ULONG,ULONG,ULONG,ULONG,ULONG,ULONG,ULONG) =
            find("_D3DDevice_CreateTexture2@28");
        void (NTAPI *lock_tex)(void *,ULONG,ULONG *,const LONG *,ULONG) =
            find("_D3DTexture_LockRect@20");
        void (NTAPI *set_tex)(ULONG,void *) = find("_D3DDevice_SetTexture@8");
        void *tex = create_tex(64,64,1,2,0,6,3);
        assert(tex);
        for (unsigned level = 0; level < 2; level++) {
            ULONG locked[2];
            lock_tex(tex,level,locked,NULL,0);
            uint32_t *pixels = (void *)locked[1];
            for (unsigned i = 0; i < (64u >> level) * (64u >> level); i++)
                pixels[i] = level ? 0xff00ff00 : 0xffff0000;
        }
        set_tex(0,tex);
        texture_states[3] = texture_states[4] = 1; /* point mag/min */
        texture_states[12] = texture_states[16] = 2; /* select arg1 */
        texture_states[14] = texture_states[18] = 2; /* texture */
        set_vs(0x144); /* XYZRHW + diffuse + TEX1 */
        struct tex_vertex { float x,y,z,w; uint32_t color; float u,v; };
        struct tex_vertex textured[] = {
            {4,4,0.5f,1,0xffffffff,0,0}, {60,4,0.5f,1,0xffffffff,8,0},
            {32,60,0.5f,1,0xffffffff,4,8}
        };
        for (unsigned mip = 0; mip < 3; mip++) {
            texture_states[5] = mip; /* none, point, linear */
            clear(0,NULL,0xf3,0xff000000,1.0f,0);
            draw(5,3,textured,sizeof(textured[0]));
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(pixel[mip ? 1 : 0] > 240 && pixel[mip ? 0 : 1] < 8 && pixel[2] < 8);
        }
        puts("renderer mipmaps: authored partial chain and none/point/linear selection passed");
        /* The outstanding 65th callback executes inside Swap; device creation
         * resets scale, so restore the values its reentrant check expects. */
        set_scale(1.25f,0.75f);
        swap(0);
        assert(callbacks == 65);
        assert(d3d_frame_count() == 1);
        assert(reset_combos == 1 && reset_checks == 1);
        puts("renderer runtime: vblank, miniport event, reentrant callback and reset services passed");
        puts("renderer draw: native GL triangle pixel and presentation passed");
    }
    puts("renderer integration: shared state, fastcall, resources, callbacks and reentrancy passed");
    return 0;
}
