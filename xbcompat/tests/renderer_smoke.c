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
        /* Post-processing copies must read the GPU image, not the stale
         * CPU allocation behind a render-to-texture surface. Repeat with
         * new colors and exercise both linear and swizzled destinations. */
        LONG (NTAPI *surface)(void *,ULONG,void **) = find("_D3DTexture_GetSurfaceLevel@12");
        void (NTAPI *get_rt)(void **) = find("_D3DDevice_GetRenderTarget@4");
        void (NTAPI *set_rt)(void *,void *) = find("_D3DDevice_SetRenderTarget@8");
        void (NTAPI *copy_rects)(void *,const LONG *,ULONG,void *,const LONG *) =
            find("_D3DDevice_CopyRects@20");
        void *back = NULL, *source_surface = NULL;
        void *source_tex = create_tex(16,16,1,1,0,6,3);
        get_rt(&back);
        assert(surface(source_tex,0,&source_surface) == 0);
        texture_states[5] = 0;
        for (unsigned layout = 0; layout < 2; layout++) {
            void *dest_tex = create_tex(16,16,1,1,0,layout ? 6 : 0x12,3);
            void *dest_surface = NULL;
            assert(surface(dest_tex,0,&dest_surface) == 0);
            for (unsigned frame = 0; frame < 2; frame++) {
                set_tex(0,NULL);
                set_rt(source_surface,NULL);
                clear(0,NULL,0xf0,frame ? 0xff0000ff : 0xff00ff00,1.0f,0);
                set_rt(back,NULL);
                copy_rects(source_surface,NULL,0,dest_surface,NULL);
                set_tex(0,dest_tex);
                for (unsigned v = 0; v < 3; v++) {
                    textured[v].u = textured[v].v = layout ? 0.5f : 8.0f;
                }
                clear(0,NULL,0xf0,0xff000000,1.0f,0);
                draw(5,3,textured,sizeof(textured[0]));
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[frame ? 2 : 1] > 240 && pixel[frame ? 1 : 2] < 8 && pixel[0] < 8);
            }
        }
        puts("renderer copies: fresh GPU render targets to linear/swizzled textures passed");
        /* Texture controls participate in the fragment-program cache, while
         * key colors are per-draw uniforms. Reuse the same shader/texture to
         * catch stale variants and stale uniforms through the HLE backend. */
        void (NTAPI *create_ps)(const ULONG *, ULONG *) =
            find("_D3DDevice_CreatePixelShader@8");
        void (NTAPI *set_ps)(ULONG) = find("_D3DDevice_SetPixelShader@4");
        void (NTAPI *set_key_color)(ULONG, ULONG) =
            find("_D3DDevice_SetTextureState_ColorKeyColor@8");
        ULONG control_def[60] = {0}, control_ps = 0;
        control_def[8] = 8;            /* explicit final RGB = T0 */
        control_def[9] = 0x18u << 8;   /* final alpha = T0.a */
        control_def[54] = 1;           /* PROJECT2D, then disabled stages */
        create_ps(control_def, &control_ps);
        assert(control_ps);
        void *control_tex = create_tex(4,4,1,1,0,6,3);
        assert(control_tex);
        ULONG control_lock[2];
        lock_tex(control_tex,0,control_lock,NULL,0);
        for (unsigned i = 0; i < 16; i++)
            ((uint32_t *)control_lock[1])[i] = 0x00ff0000; /* red, zero alpha */
        set_tex(0,control_tex);
        set_ps(control_ps);
        set_vs(0x144);
        texture_states[5] = 0;  /* no mip filtering */
        texture_states[9] = 0;  /* color key disabled */
        texture_states[11] = 0; /* alpha kill disabled */
        render_states[59] = render_states[60] = 0;
        render_states[92] = render_states[124] = 0;
        for (unsigned pass = 0; pass < 3; pass++) {
            /* D3DTEXTUREALPHAKILL_ENABLE is the hardware bit value 4. */
            texture_states[11] = pass == 1 ? 4 : 0;
            clear(0,NULL,0xf3,0xff000000,1.0f,0);
            draw(5,3,textured,sizeof(textured[0]));
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(pass == 1 ? pixel[0] < 8 : pixel[0] > 240);
            assert(pixel[1] < 8 && pixel[2] < 8);
        }
        set_key_color(0,0x00ff0000);
        for (unsigned pass = 0; pass < 3; pass++) {
            texture_states[9] = pass == 1 ? 3 : 0; /* kill matching texels */
            clear(0,NULL,0xf3,0xff000000,1.0f,0);
            draw(5,3,textured,sizeof(textured[0]));
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(pass == 1 ? pixel[0] < 8 : pixel[0] > 240);
            assert(pixel[1] < 8 && pixel[2] < 8);
        }
        texture_states[9] = 3;
        for (unsigned pass = 0; pass < 3; pass++) {
            /* Identical shader variant: only the key-color uniform changes. */
            set_key_color(0,pass == 1 ? 0x000000ff : 0x00ff0000);
            clear(0,NULL,0xf3,0xff000000,1.0f,0);
            draw(5,3,textured,sizeof(textured[0]));
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(pass == 1 ? pixel[0] > 240 : pixel[0] < 8);
            assert(pixel[1] < 8 && pixel[2] < 8);
        }
        set_ps(0);
        set_tex(0,tex);
        texture_states[9] = texture_states[11] = 0;
        texture_states[5] = 2;
        puts("renderer shader controls: cached kill/key toggles and key uniform updates passed");
        /* A partial CopyRects into the backbuffer must preserve untouched GPU
         * pixels before the full CPU image is flushed back to the renderer. */
        {
            void (NTAPI *get_bb)(LONG,ULONG,void **) = find("_D3DDevice_GetBackBuffer@12");
            LONG (NTAPI *get_depth)(void **) = find("_D3DDevice_GetDepthStencilSurface@4");
            void (NTAPI *set_target)(void *,void *) = find("_D3DDevice_SetRenderTarget@8");
            LONG (NTAPI *create_image)(ULONG,ULONG,ULONG,void **) =
                find("_D3DDevice_CreateImageSurface@16");
            void (NTAPI *lock_surface)(void *,ULONG *,const LONG *,ULONG) =
                find("_D3DSurface_LockRect@16");
            void (NTAPI *copy_rects)(void *,const LONG *,ULONG,void *,const LONG *) =
                find("_D3DDevice_CopyRects@20");
            void *bb = NULL, *depth = NULL, *source = NULL;
            get_bb(0,0,&bb);
            assert(get_depth(&depth) == 0);
            assert(create_image(1,1,6,&source) == 0);
            ULONG locked[2];
            lock_surface(source,locked,NULL,0);
            *(uint32_t *)locked[1] = 0xffff0000;
            set_target(bb,depth);
            clear(0,NULL,0xf3,0xff0000ff,1.0f,0);
            LONG rect[4] = {0,0,1,1}, point[2] = {8,8};
            copy_rects(source,rect,1,bb,point);
            /* A zero-mask clear flushes CPU writes without overwriting pixels. */
            clear(0,NULL,0,0,1.0f,0);
            unsigned char copied[4] = {0}, untouched[4] = {0};
            glReadPixels(8,55,1,1,GL_RGBA,GL_UNSIGNED_BYTE,copied);
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,untouched);
            assert(copied[0] > 240 && copied[1] < 8 && copied[2] < 8);
            assert(untouched[0] < 8 && untouched[1] < 8 && untouched[2] > 240);
            puts("renderer CopyRects: partial backbuffer copy preserves untouched pixels");
        }

        /* Updating a standalone render target must keep its current attachment
         * valid and preserve the copied pixels when the target is rebound. */
        {
            void (NTAPI *get_bb)(LONG,ULONG,void **) = find("_D3DDevice_GetBackBuffer@12");
            LONG (NTAPI *get_depth)(void **) = find("_D3DDevice_GetDepthStencilSurface@4");
            void (NTAPI *set_target)(void *,void *) = find("_D3DDevice_SetRenderTarget@8");
            LONG (NTAPI *create_image)(ULONG,ULONG,ULONG,void **) =
                find("_D3DDevice_CreateImageSurface@16");
            LONG (NTAPI *create_target)(ULONG,ULONG,ULONG,ULONG,BOOLEAN,void **) =
                find("_D3DDevice_CreateRenderTarget@24");
            void (NTAPI *lock_surface)(void *,ULONG *,const LONG *,ULONG) =
                find("_D3DSurface_LockRect@16");
            void (NTAPI *copy_rects)(void *,const LONG *,ULONG,void *,const LONG *) =
                find("_D3DDevice_CopyRects@20");
            void *bb = NULL, *depth = NULL, *source = NULL, *target = NULL, *uncached = NULL;
            get_bb(0,0,&bb);
            assert(get_depth(&depth) == 0);
            assert(create_image(1,1,6,&source) == 0);
            assert(create_target(8,8,6,0,1,&target) == 0);
            assert(create_target(8,8,6,0,1,&uncached) == 0);
            ULONG locked[2];
            lock_surface(source,locked,NULL,0);
            *(uint32_t *)locked[1] = 0xffff0000;
            LONG rect[4] = {0,0,1,1}, point[2] = {2,2};
            set_target(target,NULL);
            clear(0,NULL,0xf3,0xff0000ff,1.0f,0);
            copy_rects(source,rect,1,target,point);
            unsigned char copied[4] = {0}, untouched[4] = {0};
            glReadPixels(2,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,copied);
            glReadPixels(7,7,1,1,GL_RGBA,GL_UNSIGNED_BYTE,untouched);
            assert(copied[0] > 240 && copied[1] < 8 && copied[2] < 8);
            assert(untouched[0] < 8 && untouched[1] < 8 && untouched[2] > 240);
            set_target(bb,depth);
            set_target(target,NULL);
            memset(copied,0,sizeof copied);
            glReadPixels(2,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,copied);
            assert(copied[0] > 240 && copied[1] < 8 && copied[2] < 8);
            /* Copying before the first binding must initialize the later GL
             * image from surface memory, rather than allocating empty pixels. */
            set_target(bb,depth);
            copy_rects(source,rect,1,uncached,point);
            set_target(uncached,NULL);
            memset(copied,0,sizeof copied);
            glReadPixels(2,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,copied);
            assert(copied[0] > 240 && copied[1] < 8 && copied[2] < 8);
            set_target(bb,depth);
            puts("renderer CopyRects: standalone attachment, rebind and first-bind pixels persist");
        }

        /* Updating one cube face must not discard GPU-only pixels rendered
         * into its siblings when the shared parent texture is cached. */
        {
            void (NTAPI *get_bb)(LONG,ULONG,void **) = find("_D3DDevice_GetBackBuffer@12");
            LONG (NTAPI *get_depth)(void **) = find("_D3DDevice_GetDepthStencilSurface@4");
            void (NTAPI *set_target)(void *,void *) = find("_D3DDevice_SetRenderTarget@8");
            LONG (NTAPI *create_image)(ULONG,ULONG,ULONG,void **) =
                find("_D3DDevice_CreateImageSurface@16");
            LONG (NTAPI *create_cube)(ULONG,ULONG,ULONG,ULONG,ULONG,void **) =
                find("_D3DDevice_CreateCubeTexture@24");
            LONG (NTAPI *get_face)(void *,ULONG,ULONG,void **) =
                find("_D3DCubeTexture_GetCubeMapSurface@16");
            void (NTAPI *lock_surface)(void *,ULONG *,const LONG *,ULONG) =
                find("_D3DSurface_LockRect@16");
            void (NTAPI *copy_rects)(void *,const LONG *,ULONG,void *,const LONG *) =
                find("_D3DDevice_CopyRects@20");
            void *bb = NULL, *depth = NULL, *source = NULL, *cube = NULL;
            void *face0 = NULL, *face1 = NULL;
            get_bb(0,0,&bb);
            assert(get_depth(&depth) == 0);
            assert(create_image(1,1,6,&source) == 0);
            assert(create_cube(8,1,0,6,0,&cube) == 0);
            assert(get_face(cube,0,0,&face0) == 0);
            assert(get_face(cube,1,0,&face1) == 0);
            ULONG locked[2];
            lock_surface(source,locked,NULL,0);
            *(uint32_t *)locked[1] = 0xffff0000;
            set_target(face0,NULL);
            clear(0,NULL,0xf3,0xff00ff00,1.0f,0);
            set_target(face1,NULL);
            clear(0,NULL,0xf3,0xff0000ff,1.0f,0);
            LONG rect[4] = {0,0,1,1}, point[2] = {2,2};
            copy_rects(source,rect,1,face1,point);
            set_target(face0,NULL);
            unsigned char sibling[4] = {0}, copied[4] = {0}, untouched[4] = {0};
            glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,sibling);
            assert(sibling[0] < 8 && sibling[1] > 240 && sibling[2] < 8);
            set_target(face1,NULL);
            glReadPixels(2,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,copied);
            glReadPixels(7,7,1,1,GL_RGBA,GL_UNSIGNED_BYTE,untouched);
            assert(copied[0] > 240 && copied[1] < 8 && copied[2] < 8);
            assert(untouched[0] < 8 && untouched[1] < 8 && untouched[2] > 240);
            set_target(bb,depth);
            puts("renderer CopyRects: cached cube siblings and destination pixels persist");
        }

        /* Owned FF lighting through the real HLE state/matrix/uniform path.
         * No pixel shader: this also verifies the compatibility fragment
         * stage receives our separate primary and specular outputs. */
        {
            void (NTAPI *ff_set_transform)(ULONG,const void *) = find("_D3DDevice_SetTransform@8");
            void (NTAPI *ff_set_material)(const float *) = find("_D3DDevice_SetMaterial@4");
            LONG (NTAPI *ff_set_light)(ULONG,const void *) = find("_D3DDevice_SetLight@8");
            LONG (NTAPI *ff_light_enable)(ULONG,BOOLEAN) = find("_D3DDevice_LightEnable@8");
            void (NTAPI *ff_set_ps)(ULONG) = find("_D3DDevice_SetPixelShader@4");
            struct ff_light {
                ULONG type;
                float diffuse[4], specular[4], ambient[4], position[3], direction[3];
                float range, falloff, a0, a1, a2, theta, phi;
            } light = { 0 };
            struct ff_vertex { float x,y,z,nx,ny,nz; uint32_t color; };
            struct ff_vertex vertices[] = {
                {-.75f,-.75f,2,0,0,1,0xff00ff00},
                { .75f,-.75f,2,0,0,1,0xff00ff00},
                {0,.75f,2,0,0,1,0xff00ff00}
            };
            struct ff_plain_vertex { float x,y,z,nx,ny,nz; };
            struct ff_plain_vertex plain[] = {
                {-.75f,-.75f,2,0,0,1}, {.75f,-.75f,2,0,0,1}, {0,.75f,2,0,0,1}
            };
            float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            float projection[16] = {1,0,0,0, 0,1,0,0, 0,0,.1f,0, 0,0,0,1};
            float material[17] = {1,0,0,1}; /* red diffuse */
            ff_set_ps(0);
            for (unsigned unit = 0; unit < 4; unit++) {
                set_tex(unit,NULL);
                texture_states[unit * 32 + 12] = 1; /* disable texture stage */
                texture_states[unit * 32 + 21] = 0;
                texture_states[unit * 32 + 28] = unit;
            }
            render_states[59] = render_states[60] = render_states[82] = 0;
            render_states[108] = render_states[109] = render_states[118] = render_states[122] = 0;
            render_states[123] = render_states[124] = render_states[125] = render_states[128] = 0;
            render_states[67] = 0x01010101;
            render_states[92] = render_states[95] = 1;
            render_states[93] = render_states[94] = 0;
            render_states[100] = render_states[101] = render_states[102] = render_states[103] = 0;
            render_states[105] = 0;
            render_states[120] = GL_FILL;
            ff_set_transform(0,identity);
            ff_set_transform(1,projection);
            ff_set_transform(6,identity);
            for (unsigned i = 0; i < 8; i++) ff_light_enable(i,0);
            light.type = 3; /* directional: range must be ignored */
            light.direction[2] = -1;
            light.diffuse[0] = light.diffuse[1] = light.diffuse[2] = 1;
            ff_set_light(0,&light);
            ff_light_enable(0,1);
            ff_set_material(material);
            set_vs(0x52); /* XYZ, NORMAL, DIFFUSE */
            clear(0,NULL,0xf3,0xff000000,1,0);
            draw(5,3,vertices,sizeof vertices[0]);
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(pixel[0] > 240 && pixel[1] < 8 && pixel[2] < 8);
            render_states[101] = 1; /* primary vertex color as diffuse */
            clear(0,NULL,0xf3,0xff000000,1,0);
            draw(5,3,vertices,sizeof vertices[0]);
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(pixel[1] > 240 && pixel[0] < 8 && pixel[2] < 8);
            render_states[101] = 2; /* absent secondary must use red material, not green primary */
            clear(0,NULL,0xf3,0xff000000,1,0);
            draw(5,3,vertices,sizeof vertices[0]);
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(pixel[0] > 240 && pixel[1] < 8 && pixel[2] < 8);
            render_states[101] = 1;
            set_vs(0x12); /* same material source, but primary is absent */
            clear(0,NULL,0xf3,0xff000000,1,0);
            draw(5,3,plain,sizeof plain[0]);
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(pixel[0] > 240 && pixel[1] < 8 && pixel[2] < 8);

            /* Ambient point contribution isolates range from varying
             * vertex-to-light angles across the triangle. */
            memset(material,0,sizeof material);
            material[3] = material[4] = 1; /* opaque, red ambient */
            ff_set_material(material);
            memset(&light,0,sizeof light);
            light.type = 1; light.a0 = 1;
            light.ambient[0] = light.ambient[1] = light.ambient[2] = 1;
            for (unsigned pass = 0; pass < 3; pass++) {
                light.range = pass ? 4 : 1;
                light.a0 = pass == 2 ? 2 : 1;
                ff_set_light(0,&light);
                clear(0,NULL,0xf3,0xff000000,1,0);
                draw(5,3,plain,sizeof plain[0]);
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                if (!pass) assert(pixel[0] < 8);
                else if (pass == 1) assert(pixel[0] > 240);
                else assert(pixel[0] > 120 && pixel[0] < 135);
                assert(pixel[1] < 8 && pixel[2] < 8);
            }

            /* Move the sample away from the camera axis; LOCALVIEWER
             * changes the halfway vector but not its screen position. */
            struct ff_plain_vertex eye_test[] = {
                {.8f,-.2f,2,0,0,-1}, {1.2f,-.2f,2,0,0,-1}, {1,.2f,2,0,0,-1}
            };
            projection[0] = projection[5] = 4;
            projection[12] = -4;
            ff_set_transform(1,projection);
            memset(material,0,sizeof material);
            material[3] = 1;
            material[8] = material[9] = material[10] = 1;
            material[16] = 16;
            ff_set_material(material);
            memset(&light,0,sizeof light);
            light.type = 3; light.direction[2] = 1;
            light.specular[0] = light.specular[1] = light.specular[2] = 1;
            ff_set_light(0,&light);
            render_states[93] = 1;
            render_states[101] = 0;
            for (unsigned local = 0; local < 2; local++) {
                render_states[94] = local;
                clear(0,NULL,0xf3,0xff000000,1,0);
                draw(5,3,eye_test,sizeof eye_test[0]);
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                if (!local) assert(pixel[0] < 8 && pixel[1] < 8 && pixel[2] < 8);
                else for (int channel = 0; channel < 3; channel++)
                    assert(pixel[channel] > 130 && pixel[channel] < 190);
            }
            ff_light_enable(0,0);
            render_states[92] = render_states[93] = 0;
            puts("renderer FF lighting: material sources, absent color, point range/attenuation and local viewer passed");
        }
        /* The pixel-shader draw path must set stage-3 point-sprite coordinate
         * replacement itself, including clearing it when sprites are disabled. */
        {
            void (NTAPI *create_sprite_ps)(const ULONG *,ULONG *) =
                find("_D3DDevice_CreatePixelShader@8");
            void (NTAPI *set_sprite_ps)(ULONG) = find("_D3DDevice_SetPixelShader@4");
            uint32_t saved_rs[256], saved_tss[128];
            memcpy(saved_rs,render_states,sizeof saved_rs);
            memcpy(saved_tss,texture_states,sizeof saved_tss);
            render_states[59] = render_states[60] = 0;
            render_states[67] = 0xffffffffu; /* all color channels */
            render_states[82] = render_states[92] = render_states[93] = 0;
            render_states[124] = render_states[125] = 0;
            render_states[106] = 0x41800000u; /* POINTSIZE = 16.0f */
            render_states[107] = 0x3f800000u; /* POINTSIZE_MIN = 1.0f */
            render_states[113] = 0x42800000u; /* POINTSIZE_MAX = 64.0f */
            render_states[108] = render_states[109] = 0;
            for (unsigned s = 0; s < 4; s++) {
                set_tex(s,NULL);
                texture_states[s*32+12] = 1; /* fixed-function chain disabled */
                texture_states[s*32+9] = texture_states[s*32+11] = 0;
                texture_states[s*32+21] = 0; /* no texture transform */
                texture_states[s*32+28] = s;
            }
            struct sprite_vertex {
                float x,y,z,w;
                uint32_t color;
                float uv[4][2];
            } point = {32,32,0.5f,1,0xffffffffu,
                       {{0.25f,0.25f},{0.25f,0.25f},{0.25f,0.25f},{0.25f,0.25f}}};
            set_vs(0x444); /* XYZRHW, diffuse, four pairs of texture coordinates */
            set_sprite_ps(0);
            /* Seed all coordinate-replacement flags to false using the
             * existing fixed-function setup. The following enable occurs
             * exclusively through the pixel-shader draw path. */
            draw(1,1,&point,sizeof point);
            void *sprite_tex = create_tex(2,2,1,1,0,6,3);
            assert(sprite_tex);
            ULONG locked[2];
            lock_tex(sprite_tex,0,locked,NULL,0);
            /* A 2x2 Morton image has the same order as these two linear rows.
             * Left texels have green=0; right texels have green=255. */
            uint32_t sprite_pixels[4] = {
                0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu
            };
            memcpy((void *)locked[1],sprite_pixels,sizeof sprite_pixels);
            set_tex(3,sprite_tex);
            texture_states[3*32+0] = texture_states[3*32+1] = 3; /* clamp */
            texture_states[3*32+3] = texture_states[3*32+4] = 1; /* point */
            texture_states[3*32+5] = 0; /* no mips */
            ULONG sprite_def[60] = {0}, sprite_ps = 0;
            sprite_def[8] = 11;          /* explicit final RGB = T3 */
            sprite_def[9] = 0x1bu << 8;   /* final alpha = T3.a */
            sprite_def[54] = 1u << 15;    /* stage 3 PROJECT2D */
            create_sprite_ps(sprite_def,&sprite_ps);
            assert(sprite_ps);
            set_sprite_ps(sprite_ps);
            for (unsigned pass = 0; pass < 2; pass++) {
                render_states[108] = pass == 0; /* enable, then disable */
                clear(0,NULL,0xf3,0xff000000,1.0f,0);
                draw(1,1,&point,sizeof point);
                unsigned char left[4] = {0}, right[4] = {0};
                glReadPixels(27,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,left);
                glReadPixels(36,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,right);
                assert(left[3] > 240 && right[3] > 240);
                assert(left[1] < 8);
                if (pass == 0) {
                    /* V orientation is immaterial: both right-hand texels
                     * have green=255, unlike either left-hand texel. */
                    assert(right[1] > 240);
                } else {
                    /* Restored vertex UV(.25,.25) samples red everywhere. */
                    assert(left[0] > 240 && right[0] > 240);
                    assert(right[1] < 8 && left[2] < 8 && right[2] < 8);
                }
            }
            set_sprite_ps(0);
            set_tex(3,NULL);
            set_tex(0,tex);
            set_vs(0x144);
            memcpy(render_states,saved_rs,sizeof saved_rs);
            memcpy(texture_states,saved_tss,sizeof saved_tss);
            puts("renderer point sprites: pixel-shader stage-3 coordinates enable and disable passed");
        }


        /* nv2a-vertex-spec: point parameters select shader oPts, otherwise
         * POINTSIZE controls the rasterizer. Exercise cached-program reuse,
         * both pixel paths, and return to fixed-function vertices. */
        {
            uint32_t saved_rs[256], saved_tss[128];
            memcpy(saved_rs,render_states,sizeof saved_rs);
            memcpy(saved_tss,texture_states,sizeof saved_tss);
            LONG (NTAPI *create_vshader)(const ULONG *,const ULONG *,ULONG *,ULONG) =
                find("_D3DDevice_CreateVertexShader@16");
            void (NTAPI *delete_vshader)(ULONG) = find("_D3DDevice_DeleteVertexShader@4");
            static const ULONG decl[] = {
                0x20000000, 0x40420000, 0x40120001, 0x40420002, 0xffffffff
            };
            /* MOV oPos,v0; MOV oPts.x,v1.x; MOV oD0,v2. */
            static const ULONG function[] = {
                0x00032078,
                0,0x0020001b,0x08000000,0x0000f800,
                0,0x00200200,0x08000000,0x00008830,
                0,0x0020041b,0x08000000,0x0000f819
            };
            ULONG point_vs = 0, point_ps = 0, def[60] = {0};
            assert(create_vshader(decl,function,&point_vs,0) == 0 && point_vs);
            def[8] = 4; def[9] = 0x14u << 8; /* explicit final V0 */
            create_ps(def,&point_ps);
            assert(point_ps);
            render_states[59] = render_states[60] = render_states[82] = 0;
            render_states[92] = render_states[93] = render_states[124] = 0;
            render_states[67] = 0xffffffffu;
            render_states[106] = 0x40800000u; /* register size 4 */
            render_states[107] = 0x3f800000u;
            render_states[113] = 0x427c0000u; /* 63 */
            render_states[108] = 0;
            render_states[110] = 0x3f800000u;
            render_states[111] = render_states[112] = 0;
            for (int s = 0; s < 4; s++) {
                set_tex(s,NULL);
                texture_states[s*32+12] = 1;
            }
            struct { float position[4], size, color[4]; } point =
                {{32,32,0.5f,1},16,{1,1,1,1}};
            struct vertex fixed_point = {32,32,0.5f,1,0xffffffffu};
            for (int pixel_path = 0; pixel_path < 2; pixel_path++) {
                set_ps(pixel_path ? point_ps : 0);
                for (int pass = 0; pass < 4; pass++) {
                    render_states[109] = pass == 1 || pass == 3;
                    set_vs(point_vs);
                    clear(0,NULL,0xf3,0xff000000,1.0f,0);
                    draw(1,1,&point,sizeof point);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert(pixel[0] > 240);
                    glReadPixels(38,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert((pixel[0] > 240) == (pass == 1 || pass == 3));
                }
                /* Switch to XYZRHW and restore the register-size state. */
                render_states[109] = 0;
                set_vs(0x44);
                clear(0,NULL,0xf3,0xff000000,1.0f,0);
                draw(1,1,&fixed_point,sizeof fixed_point);
                glReadPixels(38,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[0] < 8);
            }
            set_ps(0);
            set_vs(0x144);
            delete_vshader(point_vs);
            set_tex(0,tex);
            memcpy(render_states,saved_rs,sizeof saved_rs);
            memcpy(texture_states,saved_tss,sizeof saved_tss);
            puts("renderer programmable points: oPts selection, toggles and fixed-function return passed");
        }


        /* Programmable fog is evaluated before interpolation. Literal endpoint
         * factors separate exp/interpolate from interpolate/exp, and distinguish
         * unclamped vertex factors from early [0,1] clamping. */
        {
            uint32_t saved_rs[256], saved_tss[128];
            memcpy(saved_rs,render_states,sizeof saved_rs);
            memcpy(saved_tss,texture_states,sizeof saved_tss);
            LONG (NTAPI *create_vshader)(const ULONG *,const ULONG *,ULONG *,ULONG) =
                find("_D3DDevice_CreateVertexShader@16");
            void (NTAPI *delete_vshader)(ULONG) = find("_D3DDevice_DeleteVertexShader@4");
            static const ULONG decl[] = {
                0x20000000,0x40420000,0x40120001,0x40420002,0xffffffff
            };
            /* MOV oPos,v0; MOV oFog.y,v1.x; MOV oD0,v2.
             * A non-X output mask still publishes the selected scalar. */
            static const ULONG function[] = {
                0x00032078,
                0,0x0020001b,0x08000000,0x0000f800,
                0,0x00200200,0x08000000,0x00004828,
                0,0x0020041b,0x08000000,0x0000f819
            };
            ULONG fog_vs = 0, fog_ps = 0, def[60] = {0};
            assert(create_vshader(decl,function,&fog_vs,0) == 0 && fog_vs);
            def[8] = 0x13; def[9] = 0x14u << 8; /* FOG.a as RGB, V0.a */
            create_ps(def,&fog_ps);
            assert(fog_ps);
            set_ps(fog_ps);
            set_vs(fog_vs);
            render_states[59] = render_states[60] = render_states[92] = 0;
            render_states[93] = render_states[109] = render_states[124] = 0;
            render_states[66] = GL_SMOOTH;
            render_states[67] = 0xffffffffu;
            render_states[82] = 1;
            render_states[84] = 0; /* start 0 */
            render_states[85] = render_states[86] = 0x3f800000u; /* end/density 1 */
            for (int s = 0; s < 4; s++) {
                set_tex(s,NULL);
                texture_states[s*32+12] = 1;
            }
            struct fog_vertex { float position[4], fog, color[4]; } quad[] = {
                {{4,4,0.5f,1},0,{1,1,1,1}}, {{60,4,0.5f,1},4,{1,1,1,1}},
                {{4,60,0.5f,1},0,{1,1,1,1}}, {{60,60,0.5f,1},4,{1,1,1,1}}
            };
            static const float endpoints[3][2] = {
                {1,0.018315639f}, {1,0.000000112535f}, {3,-1}
            };
            for (int mode = 1; mode <= 3; mode++) {
                render_states[83] = mode;
                for (int i = 0; i < 4; i++)
                    quad[i].fog = mode == 3 ? ((i&1) ? 2 : -2) : ((i&1) ? 4 : 0);
                clear(0,NULL,0xf3,0xff000000,1.0f,0);
                draw(6,4,quad,sizeof quad[0]); /* triangle strip */
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                float factor = endpoints[mode-1][0] * (1-28.5f/56)
                             + endpoints[mode-1][1] * (28.5f/56);
                int expected = (int)(factor * 255 + 0.5f);
                /* Account for the renderer's half-pixel viewport convention. */
                assert(abs((int)pixel[0]-expected) <= 12);
                assert(abs((int)pixel[1]-expected) <= 12 && pixel[3] > 240);
            }
            render_states[82] = 0;
            clear(0,NULL,0xf3,0xff000000,1.0f,0);
            draw(6,4,quad,sizeof quad[0]);
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(pixel[0] > 240 && pixel[1] > 240);
            set_ps(0); set_vs(0x144); delete_vshader(fog_vs);
            set_tex(0,tex);
            memcpy(render_states,saved_rs,sizeof saved_rs);
            memcpy(texture_states,saved_tss,sizeof saved_tss);
            puts("renderer programmable fog: exponential interpolation, unclamped factors, masks and disable passed");
        }


        /* Rendering to a nonzero mip must preserve level zero and use the
         * view's dimensions for the framebuffer/viewport. Repeat for a cube. */
        {
            LONG (NTAPI *cube_surface)(void *,ULONG,ULONG,void **) =
                find("_D3DCubeTexture_GetCubeMapSurface@16");
            for (unsigned cube = 0; cube < 2; cube++) {
                void *mipped = create_tex(16,16,1,2,0,6,cube ? 5 : 3);
                void *level0 = NULL, *level1 = NULL;
                assert(mipped);
                if (cube) {
                    assert(cube_surface(mipped,2,0,&level0) == 0);
                    assert(cube_surface(mipped,2,1,&level1) == 0);
                } else {
                    assert(surface(mipped,0,&level0) == 0);
                    assert(surface(mipped,1,&level1) == 0);
                }
                set_tex(0,NULL);
                set_rt(level0,NULL);
                clear(0,NULL,0xf0,0xffff0000,1.0f,0);
                set_rt(level1,NULL);
                /* Viewport is applied on draw, so inspect the XDK state. */
                void (NTAPI *get_viewport)(ULONG *) = find("_D3DDevice_GetViewport@4");
                ULONG vp[6];
                get_viewport(vp);
                assert(vp[2] == 8 && vp[3] == 8);
                clear(0,NULL,0xf0,0xff00ff00,1.0f,0);
                set_rt(back,NULL);
                for (unsigned level = 0; level < 2; level++) {
                    unsigned extent = level ? 8 : 16;
                    void *out_tex = create_tex(extent,extent,1,1,0,0x12,3), *out_surface = NULL;
                    assert(surface(out_tex,0,&out_surface) == 0);
                    copy_rects(level ? level1 : level0,NULL,0,out_surface,NULL);
                    set_tex(0,out_tex);
                    set_ps(0); set_vs(0x144);
                    texture_states[3] = texture_states[4] = 1;
                    texture_states[5] = 0;
                    texture_states[12] = texture_states[16] = 2;
                    texture_states[14] = texture_states[18] = 2;
                    for (int i = 0; i < 3; i++) textured[i].u = textured[i].v = extent/2;
                    clear(0,NULL,0xf0,0xff000000,1.0f,0);
                    draw(5,3,textured,sizeof textured[0]);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert(pixel[level ? 1 : 0] > 240 && pixel[level ? 0 : 1] < 8);
                }
            }
            set_tex(0,tex);
            puts("renderer mip targets: 2D/cube level-one attachment, viewport and sibling preservation passed");
        }


        /* Packed render-target readback must use the source's native layout.
         * Partial updates preserve a cached destination's untouched GPU pixels. */
        {
            static const unsigned formats[][2] = {
                {0x05,0x11},{0x02,0x10},{0x03,0x1c},{0x04,0x1d},
                {0x38,0x3d},{0x39,0x3e},{0x3a,0x3f},{0x3b,0x40},{0x3c,0x41}
            };
            uint32_t saved_rs[256], saved_tss[128];
            memcpy(saved_rs,render_states,sizeof saved_rs);
            memcpy(saved_tss,texture_states,sizeof saved_tss);
            set_ps(control_ps); set_vs(0x144);
            render_states[59] = render_states[60] = render_states[82] = 0;
            render_states[92] = render_states[124] = 0;
            texture_states[3] = texture_states[4] = 1;
            texture_states[5] = texture_states[9] = texture_states[11] = 0;
            for (unsigned f = 0; f < sizeof formats/sizeof formats[0]; f++) {
                void *src_tex = create_tex(8,8,1,1,0,formats[f][0],3), *src_view = NULL;
                void *dst_tex = create_tex(8,8,1,1,0,formats[f][1],3), *dst_view = NULL;
                assert(surface(src_tex,0,&src_view) == 0 && surface(dst_tex,0,&dst_view) == 0);
                set_tex(0,NULL);
                set_rt(src_view,NULL); clear(0,NULL,0xf0,0xffff0000,1,0);
                set_rt(dst_view,NULL); clear(0,NULL,0xf0,0xff0000ff,1,0);
                set_rt(back,NULL);
                LONG rect[4] = {0,0,4,8}, point[2] = {0,0};
                copy_rects(src_view,rect,1,dst_view,point);
                set_tex(0,dst_tex);
                for (unsigned side = 0; side < 2; side++) {
                    for (int i = 0; i < 3; i++) {
                        textured[i].u = side ? 6 : 2;
                        textured[i].v = 4;
                    }
                    clear(0,NULL,0xf0,0xff000000,1,0);
                    draw(5,3,textured,sizeof textured[0]);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    if (pixel[side ? 2 : 0] <= 240)
                        fprintf(stderr,"packed copy format %x side %u: %u %u %u %u\n",
                                formats[f][0],side,pixel[0],pixel[1],pixel[2],pixel[3]);
                    assert(pixel[side ? 2 : 0] > 240);
                    assert(pixel[side ? 0 : 2] < 8 && pixel[1] < 8 && pixel[3] > 240);
                }
            }
            set_ps(0); set_tex(0,tex);
            memcpy(render_states,saved_rs,sizeof saved_rs);
            memcpy(texture_states,saved_tss,sizeof saved_tss);
            puts("renderer packed copies: nine 16/32-bit layouts and partial cached destinations passed");
        }


        /* All six XDK blend modes, through both FVF and declarations. */
        {
            void (NTAPI *set_transform)(ULONG,const void *) = find("_D3DDevice_SetTransform@8");
            void (NTAPI *get_transform)(ULONG,void *) = find("_D3DDevice_GetTransform@8");
            void (NTAPI *set_material)(const float *) = find("_D3DDevice_SetMaterial@4");
            LONG (NTAPI *set_light)(ULONG,const void *) = find("_D3DDevice_SetLight@8");
            LONG (NTAPI *light_enable)(ULONG,BOOLEAN) = find("_D3DDevice_LightEnable@8");
            LONG (NTAPI *create_vshader)(const ULONG *,const ULONG *,ULONG *,ULONG) =
                find("_D3DDevice_CreateVertexShader@16");
            void (NTAPI *delete_vshader)(ULONG) = find("_D3DDevice_DeleteVertexShader@4");
            uint32_t saved_rs[256], saved_tss[128];
            float saved_transforms[10][16];
            memcpy(saved_rs,render_states,sizeof saved_rs);
            memcpy(saved_tss,texture_states,sizeof saved_tss);
            for (unsigned i = 0; i < 10; i++) get_transform(i,saved_transforms[i]);
            float identity[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
            set_transform(0,identity); set_transform(1,identity);
            static const float offsets[4] = {-.75f,.75f,.25f,-.25f};
            for (int i = 0; i < 4; i++) {
                identity[12] = offsets[i];
                set_transform(6+i,identity);
            }
            identity[12] = 0;
            struct {
                ULONG type;
                float diffuse[4],specular[4],ambient[4],position[3],direction[3];
                float range,falloff,a0,a1,a2,theta,phi;
            } light = {0};
            light.type = 3; light.direction[2] = -1;
            light.diffuse[0] = light.diffuse[1] = light.diffuse[2] = 1;
            for (int i = 0; i < 8; i++) light_enable(i,0);
            set_light(0,&light); light_enable(0,1);
            float material[17] = {1,1,1,1};
            set_material(material);
            render_states[59] = render_states[60] = render_states[82] = 0;
            render_states[93] = render_states[95] = render_states[105] = 0;
            render_states[108] = render_states[109] = render_states[122] = 0;
            render_states[123] = render_states[124] = render_states[125] = 0;
            render_states[92] = 1;
            render_states[67] = 0xffffffffu;
            render_states[106] = 0x40800000u;
            render_states[100] = render_states[101] = render_states[102] = render_states[103] = 0;
            for (int i = 0; i < 4; i++) {
                set_tex(i,NULL);
                texture_states[i*32+12] = 1;
                texture_states[i*32+21] = 0;
                texture_states[i*32+28] = i;
            }
            static const ULONG decl[] = {
                0x20000000,0x40320000,0x40420001,0x40320002,0x40400003,0xffffffff
            };
            ULONG declared = 0;
            assert(create_vshader(decl,NULL,&declared,0) == 0 && declared);
            struct { float position[3],weights[4],normal[3]; uint32_t color; } vertex =
                {{0,0,.125f},{.25f,.25f,.25f,.25f},{0,0,1},0xffffffffu};
            static const int xs[6] = {44,32,36,34,32,32};
            static const int colors[6] = {255,128,255,191,255,255};
            set_ps(0);
            for (int layout = 0; layout < 2; layout++) {
                set_vs(layout ? declared : 0x5c); /* XYZB4, normal, diffuse */
                for (int mode = 1; mode <= 6; mode++) {
                    render_states[118] = mode;
                    clear(0,NULL,0xf0,0xff000000,1,0);
                    draw(1,1,&vertex,sizeof vertex);
                    glReadPixels(xs[mode-1],32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    if (abs((int)pixel[0]-colors[mode-1]) > 3)
                        fprintf(stderr,"skin mode %d layout %d: got %u wanted %d\n",mode,layout,pixel[0],colors[mode-1]);
                    assert(abs((int)pixel[0]-colors[mode-1]) <= 3);
                }
                render_states[118] = 1;
                identity[12] = .75f; identity[10] = 4;
                set_transform(7,identity);
                clear(0,NULL,0xf0,0xff000000,1,0);
                draw(1,1,&vertex,sizeof vertex);
                glReadPixels(44,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(abs((int)pixel[0]-112) <= 3);
                render_states[123] = 1;
                clear(0,NULL,0xf0,0xff000000,1,0);
                draw(1,1,&vertex,sizeof vertex);
                glReadPixels(44,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[0] > 240);
                render_states[123] = 0;
                identity[10] = 1; set_transform(7,identity);
            }
            light_enable(0,0);
            set_vs(0x144); delete_vshader(declared);
            for (unsigned i = 0; i < 10; i++) set_transform(i,saved_transforms[i]);
            memcpy(render_states,saved_rs,sizeof saved_rs);
            memcpy(texture_states,saved_tss,sizeof saved_tss);
            set_tex(0,tex);
            puts("renderer skinning: six modes, FVF/declarations, inverse normals and normalization passed");
        }


        /* Literal format fixtures test channel order and opaque-X semantics.
         * Linear rows have sentinel padding; the sampled row is not row zero. */
        {
            static const struct {
                unsigned swizzled, linear, bytes;
                uint32_t texel;
                unsigned char rgba[4];
            } formats[] = {
                {0x00,0x13,1,0x55,{85,85,85,255}},
                {0x01,0x1b,1,0x55,{85,85,85,85}},
                {0x19,0x1f,1,0x55,{255,255,255,85}},
                {0x1a,0x20,2,0xaa55,{85,85,85,170}},
                {0x32,0x35,2,0x5555,{85,85,85,255}},
                {0x06,0x12,4,0xdd115599,{17,85,153,221}},
                {0x07,0x1e,4,0x00115599,{17,85,153,255}},
                {0x3a,0x3f,4,0xdd995511,{17,85,153,221}},
                {0x3b,0x40,4,0x995511dd,{17,85,153,221}},
                {0x3c,0x41,4,0x115599dd,{17,85,153,221}},
                {0x05,0x11,2,0xaaa6,{173,85,49,255}},
                {0x02,0x10,2,0xd543,{173,82,25,255}},
                {0x03,0x1c,2,0x5543,{173,82,25,255}},
                {0x04,0x1d,2,0xd159,{17,85,153,221}},
                {0x38,0x3d,2,0xaa87,{173,82,25,255}},
                {0x39,0x3e,2,0x159d,{17,85,153,221}}
            };
            uint32_t saved_rs[256],saved_tss[128];
            memcpy(saved_rs,render_states,sizeof saved_rs);
            memcpy(saved_tss,texture_states,sizeof saved_tss);
            set_ps(control_ps); set_vs(0x144);
            render_states[59] = render_states[60] = render_states[82] = 0;
            render_states[92] = render_states[124] = 0;
            texture_states[3] = texture_states[4] = 1;
            texture_states[5] = texture_states[9] = texture_states[11] = texture_states[21] = 0;
            for (unsigned f = 0; f < sizeof formats/sizeof formats[0]; f++)
                for (unsigned linear = 0; linear < 2; linear++) {
                    unsigned fmt = linear ? formats[f].linear : formats[f].swizzled;
                    void *sample = create_tex(4,2,1,1,0,fmt,3);
                    ULONG locked[2];
                    lock_tex(sample,0,locked,NULL,0);
                    unsigned char *data = (void *)locked[1];
                    if (linear) {
                        memset(data,0xee,locked[0]*2);
                        memset(data,0,4*formats[f].bytes);
                        for (int x = 0; x < 4; x++)
                            memcpy(data+locked[0]+x*formats[f].bytes,&formats[f].texel,formats[f].bytes);
                    } else {
                        for (int x = 0; x < 8; x++)
                            memcpy(data+x*formats[f].bytes,&formats[f].texel,formats[f].bytes);
                    }
                    set_tex(0,sample);
                    for (int i = 0; i < 3; i++) {
                        textured[i].u = linear ? 2.5f : .625f;
                        textured[i].v = linear ? 1.5f : .75f;
                    }
                    clear(0,NULL,0xf0,0xff000000,1,0);
                    draw(5,3,textured,sizeof textured[0]);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    for (int k = 0; k < 4; k++) {
                        if (abs((int)pixel[k]-formats[f].rgba[k]) > 2)
                            fprintf(stderr,"texture format %x channel %d: %u expected %u\n",fmt,k,pixel[k],formats[f].rgba[k]);
                        assert(abs((int)pixel[k]-formats[f].rgba[k]) <= 2);
                    }
                }
            /* Two luma samples share neutral chroma: byte-order errors
             * cannot turn both endpoints into black and white correctly. */
            static const unsigned char video[2][4] = {{16,128,235,128},{128,16,128,235}};
            for (unsigned fmt = 0; fmt < 2; fmt++) {
                void *sample = create_tex(2,2,1,1,0,0x24+fmt,3);
                ULONG locked[2]; lock_tex(sample,0,locked,NULL,0);
                memcpy((void *)locked[1],video[fmt],4);
                memcpy((void *)(locked[1]+locked[0]),video[fmt],4);
                set_tex(0,sample);
                for (unsigned x = 0; x < 2; x++) {
                    for (int i = 0; i < 3; i++) { textured[i].u = x+.5f; textured[i].v = 1.5f; }
                    clear(0,NULL,0xf0,0xff000000,1,0);
                    draw(5,3,textured,sizeof textured[0]);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    for (int k = 0; k < 3; k++) assert(abs((int)pixel[k]-(x ? 255 : 0)) <= 2);
                    assert(pixel[3] > 240);
                }
            }
            set_ps(0); set_tex(0,tex);
            memcpy(render_states,saved_rs,sizeof saved_rs);
            memcpy(texture_states,saved_tss,sizeof saved_tss);
            puts("renderer texture formats: 32 color layouts, padded rows and both YUV byte orders passed");
        }

        /* Independently tabulated Morton addresses, with each storage word
         * encoding its own address. Dimensions deliberately exhaust axes at
         * different times. No runtime swizzler generates expected values. */
        {
            static const struct {
                unsigned w,h,d,x,y,z,address;
            } cases[] = {
                {8,2,1,7,0,0,13},{8,2,1,3,1,0,7},
                {2,8,1,0,7,0,14},{2,8,1,1,3,0,7},
                {8,4,2,5,2,1,53},{8,4,2,2,1,0,10},
                {2,4,8,1,2,5,45},{2,4,8,0,3,2,26}
            };
            void (NTAPI *lock_volume)(void *,ULONG,ULONG *,const LONG *,ULONG) =
                find("_D3DVolumeTexture_LockBox@20");
            ULONG def[60] = {0}, volume_ps = 0;
            def[8] = 8; def[9] = 0x18u<<8; def[54] = 2;
            create_ps(def,&volume_ps);
            struct {float x,y,z,w; uint32_t color; float u,v,r;} vertices[] = {
                {4,4,.5f,1,0xffffffff,0,0,0},{60,4,.5f,1,0xffffffff,0,0,0},{32,60,.5f,1,0xffffffff,0,0,0}
            };
            set_vs(0x10144);
            texture_states[3] = texture_states[4] = 1;
            texture_states[5] = texture_states[9] = texture_states[11] = texture_states[21] = 0;
            for (unsigned c = 0; c < sizeof cases/sizeof cases[0]; c++) {
                void *sample = create_tex(cases[c].w,cases[c].h,cases[c].d,1,0,6,cases[c].d>1 ? 4 : 3);
                ULONG locked[3];
                uint32_t *data;
                if (cases[c].d>1) {
                    lock_volume(sample,0,locked,NULL,0); data = (void *)locked[2];
                } else {
                    lock_tex(sample,0,locked,NULL,0); data = (void *)locked[1];
                }
                for (unsigned i = 0; i < cases[c].w*cases[c].h*cases[c].d; i++)
                    data[i] = 0xff000000u | ((3*i)<<16) | (i<<8) | (255-i);
                set_tex(0,sample);
                set_ps(cases[c].d>1 ? volume_ps : control_ps);
                for (int i = 0; i < 3; i++) {
                    vertices[i].u = (cases[c].x+.5f)/cases[c].w;
                    vertices[i].v = (cases[c].y+.5f)/cases[c].h;
                    vertices[i].r = (cases[c].z+.5f)/cases[c].d;
                }
                clear(0,NULL,0xf0,0xff000000,1,0);
                draw(5,3,vertices,sizeof vertices[0]);
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(abs((int)pixel[0]-3*(int)cases[c].address) <= 1);
                assert(abs((int)pixel[1]-(int)cases[c].address) <= 1);
                assert(abs((int)pixel[2]-(255-(int)cases[c].address)) <= 1);
            }
            set_ps(0); set_tex(0,tex); set_vs(0x144);
            puts("renderer swizzles: 8x2, 2x8, 8x4x2 and 2x4x8 literal address fixtures passed");
        }


        /* Every supported palette length, highest valid index, and palette
         * edits without touching indexed texture memory or shader state. */
        {
            LONG (NTAPI *create_palette)(ULONG,void **) = find("_D3DDevice_CreatePalette@8");
            void (NTAPI *lock_palette)(void *,void **,ULONG) = find("_D3DPalette_Lock@12");
            void (NTAPI *set_palette)(ULONG,void *) = find("_D3DDevice_SetPalette@8");
            set_ps(control_ps); set_vs(0x144);
            texture_states[3] = texture_states[4] = 1;
            texture_states[5] = texture_states[9] = texture_states[11] = texture_states[21] = 0;
            for (unsigned size = 0; size < 4; size++) {
                unsigned count = 256u>>size;
                void *palette = NULL;
                assert(create_palette(size,&palette) == 0);
                uint32_t *colors = NULL; lock_palette(palette,(void **)&colors,0);
                memset(colors,0,count*4);
                void *indexed = create_tex(2,2,1,1,0,0x0b,3);
                ULONG locked[2]; lock_tex(indexed,0,locked,NULL,0);
                memset((void *)locked[1],count-1,4);
                set_palette(0,palette); set_tex(0,indexed);
                for (int i = 0; i < 3; i++) textured[i].u = textured[i].v = .5f;
                for (unsigned edit = 0; edit < 2; edit++) {
                    colors[count-1] = edit ? 0xff00ff00 : 0xffff0000;
                    clear(0,NULL,0xf0,0xff000000,1,0);
                    draw(5,3,textured,sizeof textured[0]);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert(pixel[edit ? 1 : 0] > 240 && pixel[edit ? 0 : 1] < 8 && pixel[2] < 8);
                }
            }
            set_palette(0,NULL); set_ps(0); set_tex(0,tex);
            puts("renderer palettes: 32/64/128/256 entries and palette-only cache refresh passed");
        }

        /* Compressed volume uploads decode the packed slab order before GL.
         * The selected blocks distinguish XY tiles, slices within a slab,
         * and the second slab. Minification then selects an authored mip. */
        {
            void (NTAPI *lock_volume)(void *,ULONG,ULONG *,const LONG *,ULONG) =
                find("_D3DVolumeTexture_LockBox@20");
            ULONG volume_def[60] = {0}, volume_ps = 0;
            volume_def[8] = 8;
            volume_def[9] = 0x18u << 8;
            volume_def[54] = 2; /* PROJECT3D */
            create_ps(volume_def,&volume_ps);
            assert(volume_ps);
            set_ps(volume_ps);
            set_vs(0x10144); /* XYZRHW + diffuse + TEX1 with 3 coordinates */
            render_states[59] = render_states[60] = 0;
            render_states[92] = render_states[124] = 0;
            texture_states[9] = texture_states[11] = 0;
            texture_states[3] = texture_states[4] = 1;
            struct volume_vertex { float x,y,z,w; uint32_t color; float u,v,r; };
            struct volume_vertex volume_triangle[] = {
                {4,4,0.5f,1,0xffffffff,0,0,0},
                {60,4,0.5f,1,0xffffffff,0,0,0},
                {32,60,0.5f,1,0xffffffff,0,0,0}
            };
            static const unsigned formats[3] = {0x0c,0x0e,0x0f};
            static const unsigned positions[4][3] = {{1,1,1},{5,1,1},{1,5,5},{5,5,7}};
            static const unsigned expected_red[4] = {16,49,214,8};
            for (unsigned format_index = 0; format_index < 3; format_index++) {
                unsigned format = formats[format_index];
                unsigned block_bytes = format == 0x0c ? 8 : 16;
                unsigned color_offset = format == 0x0c ? 0 : 8;
                void *volume_tex = create_tex(8,8,8,2,0,format,4);
                assert(volume_tex);
                for (unsigned level = 0; level < 2; level++) {
                    ULONG locked[3];
                    lock_volume(volume_tex,level,locked,NULL,0);
                    unsigned blocks = level ? 4 : 32;
                    unsigned char *packed = (void *)locked[2];
                    memset(packed,0,blocks*block_bytes);
                    for (unsigned block = 0; block < blocks; block++) {
                        unsigned char *entry = packed + block*block_bytes;
                        if (format == 0x0e) memset(entry,255,8);
                        if (format == 0x0f) entry[0] = entry[1] = 255;
                        unsigned endpoint = level ? 0x07e0 : ((block % 31) + 1) << 11;
                        entry[color_offset] = endpoint & 255;
                        entry[color_offset+1] = endpoint >> 8;
                    }
                }
                set_tex(0,volume_tex);
                texture_states[5] = 0;
                for (unsigned sample = 0; sample < 4; sample++) {
                    for (int vertex = 0; vertex < 3; vertex++) {
                        volume_triangle[vertex].u = (positions[sample][0]+0.5f)/8;
                        volume_triangle[vertex].v = (positions[sample][1]+0.5f)/8;
                        volume_triangle[vertex].r = (positions[sample][2]+0.5f)/8;
                    }
                    clear(0,NULL,0xf3,0xff000000,1.0f,0);
                    draw(5,3,volume_triangle,sizeof(volume_triangle[0]));
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert(abs((int)pixel[0] - (int)expected_red[sample]) <= 2);
                    assert(pixel[1] < 3 && pixel[2] < 3);
                }
                texture_states[5] = 1; /* nearest authored mip */
                volume_triangle[0].u = 0; volume_triangle[0].v = 0;
                volume_triangle[1].u = 16; volume_triangle[1].v = 0;
                volume_triangle[2].u = 8; volume_triangle[2].v = 16;
                clear(0,NULL,0xf3,0xff000000,1.0f,0);
                draw(5,3,volume_triangle,sizeof(volume_triangle[0]));
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[0] < 8 && pixel[1] > 240 && pixel[2] < 8);
            }
            set_ps(0);
            set_tex(0,tex);
            set_vs(0x144);
            texture_states[5] = 2;
            puts("renderer compressed volumes: DXT1/3/5 slab slices and authored mip passed");
        }
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
