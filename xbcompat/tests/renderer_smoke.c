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
        /* Flat strips/fans use the completing vertex's color, while texture
         * coordinates still interpolate across each primitive. */
        {
            uint32_t saved_rs[256],saved_tss[128];
            memcpy(saved_rs,render_states,sizeof saved_rs);memcpy(saved_tss,texture_states,sizeof saved_tss);
            render_states[66]=GL_FLAT;render_states[59]=render_states[60]=0;
            render_states[82]=render_states[92]=render_states[93]=render_states[124]=0;
            render_states[67]=0xffffffffu;render_states[128]=1;
            for(int unit=0;unit<4;unit++){set_tex(unit,NULL);texture_states[unit*32+12]=1;}
            set_ps(0);set_vs(0x144);
            struct tex_vertex strip[4]={
                {4,4,.5f,1,0xffff0000,0,0},{60,4,.5f,1,0xff00ff00,1,0},
                {4,60,.5f,1,0xff0000ff,0,0},{60,60,.5f,1,0xffffffff,1,0}
            };
            struct tex_vertex fan[4]={strip[0],strip[1],strip[3],strip[2]};
            fan[2].color=0xff0000ff;fan[3].color=0xffffffff;
            for(unsigned kind=0;kind<2;kind++){
                clear(0,NULL,0xf3,0xff000000,1,0);
                draw(kind?7:6,4,kind?fan:strip,sizeof strip[0]);
                glReadPixels(kind?48:16,48,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[0]<8 && pixel[1]<8 && pixel[2]>240);
                glReadPixels(kind?16:48,16,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[0]>240 && pixel[1]>240 && pixel[2]>240);
            }
            void *gradient=create_tex(4,1,1,1,0,6,3);ULONG locked[2];
            lock_tex(gradient,0,locked,NULL,0);
            const uint32_t colors[4]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
            memcpy((void *)locked[1],colors,sizeof colors);set_tex(0,gradient);
            texture_states[12]=texture_states[16]=2;texture_states[14]=texture_states[18]=2;
            texture_states[0]=texture_states[1]=3;texture_states[3]=texture_states[4]=1;
            texture_states[5]=texture_states[6]=texture_states[7]=texture_states[21]=0;texture_states[28]=0;
            for(unsigned kind=0;kind<2;kind++){
                clear(0,NULL,0xf3,0xff000000,1,0);
                draw(kind?7:6,4,kind?fan:strip,sizeof strip[0]);
                glReadPixels(12,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[0]>240 && pixel[1]<8 && pixel[2]<8);
                glReadPixels(52,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[0]>240 && pixel[1]>240 && pixel[2]>240);
            }
            set_tex(0,tex);memcpy(render_states,saved_rs,sizeof saved_rs);memcpy(texture_states,saved_tss,sizeof saved_tss);
            puts("renderer flat primitives: strips/fans completing colors and independent texture interpolation passed");
        }

        /* Texture transform counts select a projection divisor, which must
         * remain a varying until fragment sampling. Each texel has a literal
         * color, avoiding comparison against the renderer's matrix code. */
        {
            void (NTAPI *set_transform)(ULONG,const void *)=find("_D3DDevice_SetTransform@8");
            void (NTAPI *get_transform)(ULONG,void *)=find("_D3DDevice_GetTransform@8");
            uint32_t saved_tss[128];float saved_matrix[16];
            memcpy(saved_tss,texture_states,sizeof saved_tss);get_transform(2,saved_matrix);
            void *projection_tex=create_tex(4,1,1,1,0,6,3);ULONG locked[2];
            lock_tex(projection_tex,0,locked,NULL,0);
            const uint32_t colors[4]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
            memcpy((void *)locked[1],colors,sizeof colors);
            set_tex(0,projection_tex);set_vs(0x20144); /* four input components */
            texture_states[0]=texture_states[1]=3;
            texture_states[3]=texture_states[4]=1;
            texture_states[5]=texture_states[6]=texture_states[7]=0;
            texture_states[9]=texture_states[11]=0;texture_states[28]=0;
            float matrix[16]={2,0,0,0,0,2,0,0,0,0,1,0,0,0,0,1};
            set_transform(2,matrix);
            struct {float p[4];uint32_t color;float uv[4];} vertices[4]={
                {{4,4,.5f,1},0xffffffff,{.1875f,.375f,.5f,.75f}},
                {{60,4,.5f,1},0xffffffff,{.1875f,.375f,.5f,.75f}},
                {{4,60,.5f,1},0xffffffff,{.1875f,.375f,.5f,.75f}},
                {{60,60,.5f,1},0xffffffff,{.1875f,.375f,.5f,.75f}}
            };
            const unsigned flags[7]={1,2,3,4,0x102,0x103,0x104};
            const unsigned expected[7]={1,1,1,1,2,3,2};
            for(unsigned fragment=0;fragment<2;fragment++){
                set_ps(fragment?control_ps:0);
                for(unsigned test=0;test<7;test++){
                    texture_states[21]=flags[test];
                    clear(0,NULL,0xf3,0xff000000,1,0);draw(6,4,vertices,sizeof vertices[0]);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    unsigned color=colors[expected[test]];
                    if (pixel[0] != ((color>>16)&255) || pixel[1] != ((color>>8)&255) || pixel[2] != (color&255))
                        fprintf(stderr,"projection fragment %u flags %x: %u %u %u expected %08x\\n",fragment,flags[test],pixel[0],pixel[1],pixel[2],color);
                    assert(abs((int)pixel[0]-(int)((color>>16)&255))<8);
                    assert(abs((int)pixel[1]-(int)((color>>8)&255))<8);
                    assert(abs((int)pixel[2]-(int)(color&255))<8);
                }
                matrix[0]=matrix[5]=1;set_transform(2,matrix);
                texture_states[21]=0x104;
                for(int i=0;i<4;i++){vertices[i].uv[0]=.5f;vertices[i].uv[3]=(i&1)?2:.5f;}
                clear(0,NULL,0xf3,0xff000000,1,0);draw(6,4,vertices,sizeof vertices[0]);
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[0]<8 && pixel[1]>240 && pixel[2]<8);
                matrix[0]=matrix[5]=2;set_transform(2,matrix);
                for(int i=0;i<4;i++){vertices[i].uv[0]=.1875f;vertices[i].uv[3]=.75f;}
            }
            set_transform(2,saved_matrix);set_ps(0);set_vs(0x144);set_tex(0,tex);
            memcpy(texture_states,saved_tss,sizeof saved_tss);
            puts("renderer projection: COUNT1-4, selected divisors and interpolation before divide passed");
        }

        /* Raw NV097 eye methods must survive capture/apply and recorded
         * push replay. Deliberately leave device-pusher writes pending at
         * capture, so stale CPU snapshots cannot accidentally pass. */
        {
            LONG (NTAPI *create_state)(ULONG,ULONG *)=find("_D3DDevice_CreateStateBlock@8");
            LONG (NTAPI *capture_state)(ULONG)=find("_D3DDevice_CaptureStateBlock@4");
            LONG (NTAPI *apply_state)(ULONG)=find("_D3DDevice_ApplyStateBlock@4");
            LONG (NTAPI *delete_state)(ULONG)=find("_D3DDevice_DeleteStateBlock@4");
            void (NTAPI *begin_push)(ULONG,ULONG **)=find("_D3DDevice_BeginPush@8");
            void (NTAPI *end_push)(ULONG *)=find("_D3DDevice_EndPush@4");
            LONG (NTAPI *create_push)(ULONG,BOOLEAN,void **)=find("_D3DDevice_CreatePushBuffer@12");
            void (NTAPI *begin_record)(void *)=find("_D3DDevice_BeginPushBuffer@4");
            LONG (NTAPI *end_record)(void)=find("_D3DDevice_EndPushBuffer@0");
            void (NTAPI *run_push)(void *,void *)=find("_D3DDevice_RunPushBuffer@8");
            LONG (NTAPI *cube_face)(void *,ULONG,ULONG,void **)=find("_D3DCubeTexture_GetCubeMapSurface@16");
            void (NTAPI *lock_surface)(void *,ULONG *,const LONG *,ULONG)=find("_D3DSurface_LockRect@16");
            ULONG original=0,eye_state=0;
            assert(create_state(1,&original)==0);
            void *cube=create_tex(4,4,1,1,0,6,5);
            const uint32_t faces[6]={0xffff0000,0xff00ffff,0xff00ff00,0xffff00ff,0xff0000ff,0xffffff00};
            for(unsigned face=0;face<6;face++){
                void *view=NULL;ULONG locked[2];
                assert(cube_face(cube,face,0,&view)==0);
                lock_surface(view,locked,NULL,0);
                for(int i=0;i<16;i++)((uint32_t *)locked[1])[i]=faces[face];
            }
            ULONG def[60]={0},reflection_ps=0;
            def[8]=11;def[9]=0x1bu<<8;def[54]=4|(17<<5)|(17<<10)|(18<<15);
            create_ps(def,&reflection_ps);assert(reflection_ps);
            set_ps(reflection_ps);set_vs(0x22220444); /* four 4D coordinates */
            for(int unit=0;unit<4;unit++){
                set_tex(unit,unit==3?cube:NULL);
                texture_states[unit*32+3]=texture_states[unit*32+4]=1;
                texture_states[unit*32+5]=texture_states[unit*32+6]=texture_states[unit*32+7]=0;
                texture_states[unit*32+9]=texture_states[unit*32+11]=texture_states[unit*32+21]=0;
                texture_states[unit*32+28]=unit;
            }
            render_states[59]=render_states[60]=render_states[82]=render_states[92]=render_states[124]=0;
            render_states[67]=0xffffffffu;
            struct {float p[4];uint32_t color;float t[4][4];} vertices[3]={
                {{4,4,.5f,1},0xffffffff,{{1,0,0,1},{2,0,0,7},{0,0,0,9},{0,0,0,11}}},
                {{60,4,.5f,1},0xffffffff,{{1,0,0,1},{2,0,0,7},{0,0,0,9},{0,0,0,11}}},
                {{32,60,.5f,1},0xffffffff,{{1,0,0,1},{2,0,0,7},{0,0,0,9},{0,0,0,11}}}
            };
            const ULONG eye_a[4]={(3u<<18)|0x181c,0x3e4ccccd,0x3f800000,0};
            const ULONG eye_b[4]={(3u<<18)|0x181c,0xbf800000,0,0};
            ULONG *pending=(ULONG *)((ULONG *)device)[0];
            memcpy(pending,eye_a,sizeof eye_a);((ULONG *)device)[0]=(ULONG)(pending+4);
            assert(create_state(1,&eye_state)==0);
            void *recorded=NULL;assert(create_push(128,0,&recorded)==0 && recorded);
            for(int pass=0;pass<7;pass++){
                ULONG *raw;
                if(pass==1){begin_push(4,&raw);memcpy(raw,eye_b,sizeof eye_b);end_push(raw+4);}
                if(pass==2)assert(apply_state(eye_state)==0);
                if(pass==3){
                    pending=(ULONG *)((ULONG *)device)[0];
                    memcpy(pending,eye_b,sizeof eye_b);((ULONG *)device)[0]=(ULONG)(pending+4);
                    assert(capture_state(eye_state)==0);
                    begin_push(4,&raw);memcpy(raw,eye_a,sizeof eye_a);end_push(raw+4);
                    assert(apply_state(eye_state)==0);
                }
                if(pass==4){
                    begin_record(recorded);begin_push(4,&raw);
                    memcpy(raw,eye_a,sizeof eye_a);end_push(raw+4);assert(end_record()==0);
                }
                if(pass==5)run_push(recorded,NULL);
                if(pass==6)assert(apply_state(eye_state)==0);
                clear(0,NULL,0xf3,0xff000000,1,0);draw(5,3,vertices,sizeof vertices[0]);
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                bool magenta=pass==0||pass==2||pass==5;
                assert(pixel[magenta?0:1]>240 && pixel[magenta?1:0]<8 && pixel[2]>240);
            }
            assert(apply_state(original)==0);
            delete_state(eye_state);delete_state(original);
            puts("renderer reflection eye: pending raw methods, state capture/apply and push recording/replay passed");
        }

        /* LOD controls must affect both fragment paths and reset on cached
         * images. A 16x16 chain uses red/green/blue authored levels. */
        {
            uint32_t saved_tss[128]; memcpy(saved_tss,texture_states,sizeof saved_tss);
            void *lod_tex=create_tex(16,16,1,3,0,6,3);
            const uint32_t colors[3]={0xffff0000,0xff00ff00,0xff0000ff};
            for(unsigned level=0;level<3;level++){
                ULONG locked[2];lock_tex(lod_tex,level,locked,NULL,0);
                for(unsigned i=0;i<(16u>>level)*(16u>>level);i++)((uint32_t *)locked[1])[i]=colors[level];
            }
            set_tex(0,lod_tex);set_vs(0x144);
            texture_states[3]=texture_states[4]=texture_states[5]=1;
            texture_states[9]=texture_states[11]=0;
            for(unsigned fragment=0;fragment<2;fragment++){
                set_ps(fragment?control_ps:0);
                /* rho=4 gives LOD 2; biases -2/-1/0 select levels 0/1/2. */
                for(unsigned level=0;level<3;level++){
                    float bias=(float)level-2;
                    memcpy(&texture_states[6],&bias,4);texture_states[7]=0;
                    textured[0].u=0;textured[0].v=0;
                    textured[1].u=14;textured[1].v=0;
                    textured[2].u=7;textured[2].v=14;
                    clear(0,NULL,0xf3,0xff000000,1,0);
                    draw(5,3,textured,sizeof textured[0]);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert(pixel[level]>240 && pixel[(level+1)%3]<8 && pixel[(level+2)%3]<8);
                }
                texture_states[6]=0;
                for(unsigned pass=0;pass<4;pass++){
                    unsigned level=pass==3?0:pass;
                    texture_states[7]=level;
                    for(int v=0;v<3;v++)textured[v].u=textured[v].v=.5f;
                    clear(0,NULL,0xf3,0xff000000,1,0);draw(5,3,textured,sizeof textured[0]);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert(pixel[level]>240 && pixel[(level+1)%3]<8 && pixel[(level+2)%3]<8);
                }
            }
            /* One image on two units must retain independent mip bounds. */
            set_tex(1,lod_tex);texture_states[28]=texture_states[32+28]=0;
            texture_states[32+3]=texture_states[32+4]=texture_states[32+5]=1;
            texture_states[32+6]=0;
            ULONG multi_def[60]={0},multi_ps=0;
            multi_def[8]=(0x20u<<24)|(8u<<16)|9u; /* (1-zero)*T0 + T1 */
            multi_def[9]=0x18u<<8;multi_def[54]=1|(1<<5);
            create_ps(multi_def,&multi_ps);set_ps(multi_ps);
            for(unsigned pass=0;pass<2;pass++){
                texture_states[7]=pass?2:0;texture_states[32+7]=pass?0:2;
                clear(0,NULL,0xf3,0xff000000,1,0);draw(5,3,textured,sizeof textured[0]);
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[0]>240 && pixel[1]<8 && pixel[2]>240);
            }
            set_tex(1,NULL);
            set_ps(0);set_tex(0,tex);
            memcpy(texture_states,saved_tss,sizeof saved_tss);
            puts("renderer LOD: signed bias, minimum sampled mip and cached reset on both fragment paths passed");
        }

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

            /* COLOR2 alpha must survive both native attribute layouts.
             * Toggle COLORVERTEX without changing the cached program. */
            {
                LONG (NTAPI *create_color_vs)(const ULONG *,const ULONG *,ULONG *,ULONG) =
                    find("_D3DDevice_CreateVertexShader@16");
                void (NTAPI *delete_color_vs)(ULONG) = find("_D3DDevice_DeleteVertexShader@4");
                const ULONG color_decl[] = {
                    0x20000000,0x40320000,0x40320002,0x40400003,0x40400004,0xffffffff
                };
                ULONG color_vs = 0;
                assert(create_color_vs(color_decl,NULL,&color_vs,0) == 0 && color_vs);
                struct { float p[3],n[3]; uint32_t primary,secondary; } colors[3] = {
                    {{-.75f,-.75f,2},{0,0,1},0xff0000ff,0x4000ff00},
                    {{ .75f,-.75f,2},{0,0,1},0xff0000ff,0x4000ff00},
                    {{0,.75f,2},{0,0,1},0xff0000ff,0x4000ff00}
                };
                render_states[67] = 0xffffffffu;
                render_states[101] = 2;
                for (unsigned layout = 0; layout < 2; layout++) {
                    set_vs(layout ? color_vs : 0xd2); /* XYZ/NORMAL/DIFFUSE/SPECULAR */
                    for (unsigned pass = 0; pass < 3; pass++) {
                        render_states[95] = pass != 1;
                        clear(0,NULL,0xf3,0xff000000,1,0);
                        draw(5,3,colors,sizeof colors[0]);
                        glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                        if (pass == 1)
                            assert(pixel[0] > 240 && pixel[1] < 8 && pixel[3] > 240);
                        else
                            assert(pixel[0] < 8 && pixel[1] > 240 && abs((int)pixel[3]-64) <= 2);
                        assert(pixel[2] < 8);
                    }
                }
                set_vs(0x12);
                delete_color_vs(color_vs);
                render_states[101] = 1;
                puts("renderer COLOR2: FVF/declaration RGBA and material-source toggles passed");
            }

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


        /* Logical constant endpoints and NORESERVEDCONSTANTS are independent
         * of the low mode bits. Viewport updates and draws must both honor it. */
        {
            LONG (NTAPI *create_vshader)(const ULONG *,const ULONG *,ULONG *,ULONG) =
                find("_D3DDevice_CreateVertexShader@16");
            void (NTAPI *delete_vshader)(ULONG) = find("_D3DDevice_DeleteVertexShader@4");
            void (NTAPI *set_mode)(ULONG) = find("_D3DDevice_SetShaderConstantMode@4");
            void (NTAPI *get_mode)(ULONG *) = find("_D3DDevice_GetShaderConstantMode@4");
            void (NTAPI *set_constant)(LONG,const float *,ULONG) = find("_D3DDevice_SetVertexShaderConstant@12");
            void (NTAPI *get_constant)(LONG,float *,ULONG) = find("_D3DDevice_GetVertexShaderConstant@12");
            void (NTAPI *set_viewport)(const void *) = find("_D3DDevice_SetViewport@4");
            void (NTAPI *get_viewport)(void *) = find("_D3DDevice_GetViewport@4");
            struct { ULONG x,y,w,h; float min,max; } viewport;
            ULONG old_mode;
            uint32_t saved_rs[256],saved_tss[128];
            memcpy(saved_rs,render_states,sizeof saved_rs);
            memcpy(saved_tss,texture_states,sizeof saved_tss);
            get_mode(&old_mode); get_viewport(&viewport);
            const LONG registers[4] = {-96,0,95,-38};
            float saved[4][4],reserved[2][4];
            for (int i=0;i<4;i++) get_constant(registers[i],saved[i],1);
            get_constant(-38,&reserved[0][0],2);
            const float green[4] = {0,1,0,1};
            const ULONG decl[] = {0x20000000,0x40420000,0xffffffff};
            ULONG shaders[4];
            for (int i=0;i<4;i++) {
                /* MOV oPos,v0; MOV oD0,c[physical slot]. */
                ULONG code[] = {0x00022078,
                    0,0x0020001b,0x08000000,0x0000f800,
                    0,0x0020001b | ((registers[i]+96)<<13),0x0c000000,0x0000f819};
                assert(create_vshader(decl,code,&shaders[i],0)==0 && shaders[i]);
            }
            ULONG def[60]={0},constant_ps=0;
            def[8]=4;def[9]=0x14u<<8;
            create_ps(def,&constant_ps);set_ps(constant_ps);
            render_states[59]=render_states[60]=render_states[82]=0;
            render_states[92]=render_states[93]=render_states[109]=render_states[124]=0;
            render_states[67]=0xffffffffu;
            render_states[106]=0x40800000u;
            for(int unit=0;unit<4;unit++){set_tex(unit,NULL);texture_states[unit*32+12]=1;}
            const float vertex[4]={32,32,.5f,1};
            for(ULONG mode=0;mode<3;mode++) {
                set_mode(mode|0x10);
                ULONG actual;get_mode(&actual);assert(actual==(mode|0x10));
                for(int i=0;i<4;i++) set_constant(registers[i],green,1);
                set_viewport(&viewport);
                for(int i=0;i<4;i++) {
                    float value[4];get_constant(registers[i],value,1);
                    assert(!memcmp(value,green,sizeof value));
                    set_vs(shaders[i]);
                    clear(0,NULL,0xf3,0xff000000,1,0);
                    draw(1,1,vertex,sizeof vertex);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert(pixel[0]<8 && pixel[1]>240 && pixel[2]<8 && pixel[3]>240);
                }
                /* Interleave legacy and owned fixed-function vertex paths.
                 * Neither may overwrite application constants or poison the
                 * cached programmable upload when that program resumes. */
                struct vertex fixed={32,32,.5f,1,0xffff0000};
                for(int owned=0;owned<2;owned++){
                    texture_states[21]=owned?2:0;
                    set_vs(0x44);
                    clear(0,NULL,0xf3,0xff000000,1,0);draw(1,1,&fixed,sizeof fixed);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert(pixel[0]>240 && pixel[1]<8 && pixel[2]<8);
                    for(int i=0;i<4;i++){
                        float value[4];get_constant(registers[i],value,1);
                        assert(!memcmp(value,green,sizeof value));
                    }
                    set_vs(shaders[3]);
                    clear(0,NULL,0xf3,0xff000000,1,0);draw(1,1,vertex,sizeof vertex);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert(pixel[0]<8 && pixel[1]>240 && pixel[2]<8);
                }
                texture_states[21]=0;
                set_mode(mode);
                set_viewport(&viewport);
                float value[4];get_constant(-38,value,1);
                assert(value[0]==viewport.w*.5f && value[1]==-(float)viewport.h*.5f);
                get_constant(-37,value,1);
                assert(value[0]==viewport.x+viewport.w*.5f && value[1]==viewport.y+viewport.h*.5f);
            }
            set_mode(old_mode);set_viewport(&viewport);
            for(int i=0;i<4;i++)set_constant(registers[i],saved[i],1);
            set_constant(-38,&reserved[0][0],2);
            set_ps(0);set_vs(0x144);
            for(int i=0;i<4;i++)delete_vshader(shaders[i]);
            set_tex(0,tex);
            memcpy(render_states,saved_rs,sizeof saved_rs);
            memcpy(texture_states,saved_tss,sizeof saved_tss);
            puts("renderer constants: endpoints, three modes and reserved viewport toggles passed");
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
            LONG (NTAPI *create_image)(ULONG,ULONG,ULONG,void **)=find("_D3DDevice_CreateImageSurface@16");
            void (NTAPI *lock_image)(void *,ULONG *,const LONG *,ULONG)=find("_D3DSurface_LockRect@16");
            void (NTAPI *describe_image)(void *,ULONG *)=find("_D3DSurface_GetDesc@8");
            for(unsigned f=0;f<sizeof formats/sizeof formats[0];f++){
                void *image=NULL,*destination=create_tex(4,2,1,1,0,formats[f].swizzled,3),*view=NULL;
                assert(create_image(4,2,formats[f].swizzled,&image)==0);
                ULONG desc[7],locked[2];describe_image(image,desc);
                assert(desc[0]==formats[f].linear && desc[5]==4 && desc[6]==2);
                lock_image(image,locked,NULL,0);
                unsigned char *data=(void *)locked[1];
                memset(data,0xee,locked[0]*2);
                for(int y=0;y<2;y++)for(int x=0;x<4;x++)
                    memcpy(data+y*locked[0]+x*formats[f].bytes,&formats[f].texel,formats[f].bytes);
                assert(surface(destination,0,&view)==0);copy_rects(image,NULL,0,view,NULL);
                set_tex(0,destination);
                for(int v=0;v<3;v++)textured[v].u=textured[v].v=.75f;
                clear(0,NULL,0xf3,0xff000000,1,0);draw(5,3,textured,sizeof textured[0]);
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                for(int channel=0;channel<4;channel++)
                    assert(abs((int)pixel[channel]-formats[f].rgba[channel])<=3);
            }
            /* Equal byte size does not make L8 and AL8 interchangeable. */
            {
                void *a=NULL,*b=NULL;ULONG al[2],bl[2];
                assert(create_image(4,2,0,&a)==0 && create_image(4,2,1,&b)==0);
                lock_image(a,al,NULL,0);lock_image(b,bl,NULL,0);
                memset((void *)al[1],0x11,al[0]*2);memset((void *)bl[1],0x77,bl[0]*2);
                copy_rects(a,NULL,0,b,NULL);
                for(unsigned i=0;i<bl[0]*2;i++)assert(((unsigned char *)bl[1])[i]==0x77);
            }
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


        /* Programmable front/back colors use the two-sided raster selector;
         * disabling it must take effect without rebuilding the program. */
        {
            uint32_t saved_rs[256]; memcpy(saved_rs,render_states,sizeof saved_rs);
            LONG (NTAPI *create_vshader)(const ULONG *,const ULONG *,ULONG *,ULONG) =
                find("_D3DDevice_CreateVertexShader@16");
            void (NTAPI *delete_vshader)(ULONG) = find("_D3DDevice_DeleteVertexShader@4");
            static const ULONG decl[] = {0x20000000,0x40420000,0x40420001,0x40420002,0xffffffff};
            static const ULONG function[] = {
                0x00032078,
                0,0x0020001b,0x08000000,0x0000f800,
                0,0x0020021b,0x08000000,0x0000f818,
                0,0x0020041b,0x08000000,0x0000f839
            };
            ULONG colors_vs = 0, colors_ps = 0, def[60] = {0};
            assert(create_vshader(decl,function,&colors_vs,0) == 0 && colors_vs);
            def[8] = 4; def[9] = 0x14u<<8; create_ps(def,&colors_ps);
            struct color_vertex {float position[4],front[4],back[4];} triangle[] = {
                {{4,4,.5f,1},{1,0,0,1},{0,1,0,1}},
                {{60,4,.5f,1},{1,0,0,1},{0,1,0,1}},
                {{32,60,.5f,1},{1,0,0,1},{0,1,0,1}}
            };
            set_vs(colors_vs); set_ps(colors_ps);
            render_states[59] = render_states[60] = render_states[82] = 0;
            render_states[92] = render_states[124] = render_states[128] = 0;
            render_states[67] = 0xffffffffu;
            render_states[66] = GL_SMOOTH;
            for (int enabled = 1; enabled >= 0; enabled--) {
                render_states[122] = enabled;
                unsigned red_faces = 0, green_faces = 0;
                for (int winding = 0; winding < 2; winding++) {
                    struct color_vertex tmp = triangle[0]; triangle[0] = triangle[1]; triangle[1] = tmp;
                    clear(0,NULL,0xf0,0xff000000,1,0);
                    draw(5,3,triangle,sizeof triangle[0]);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    red_faces += pixel[0]>240 && pixel[1]<8;
                    green_faces += pixel[1]>240 && pixel[0]<8;
                }
                assert(red_faces == (enabled ? 1u : 2u) && green_faces == (enabled ? 1u : 0u));
            }
            /* Ordinary color flat shading must not blend the triangle's
             * red/green/blue vertex colors. This checks current GL last-vertex
             * behavior; broader Xbox strip/fan provoking rules remain open. */
            triangle[1].front[0] = 0; triangle[1].front[1] = 1;
            triangle[2].front[0] = 0; triangle[2].front[2] = 1;
            render_states[66] = GL_FLAT;
            clear(0,NULL,0xf0,0xff000000,1,0);
            draw(5,3,triangle,sizeof triangle[0]);
            glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
            assert(pixel[0]<8 && pixel[1]<8 && pixel[2]>240);
            set_ps(0); set_vs(0x144); delete_vshader(colors_vs);
            memcpy(render_states,saved_rs,sizeof saved_rs);
            puts("renderer vertex colors: front/back selection, disable and flat triangle passed");
        }

        /* Owned texgen emits object/eye/normal coordinates before the texture
         * matrix. Normalize normals even when lighting itself is disabled. */
        {
            uint32_t saved_rs[256],saved_tss[128]; float saved[10][16];
            void (NTAPI *set_transform)(ULONG,const void *) = find("_D3DDevice_SetTransform@8");
            void (NTAPI *get_transform)(ULONG,void *) = find("_D3DDevice_GetTransform@8");
            memcpy(saved_rs,render_states,sizeof saved_rs); memcpy(saved_tss,texture_states,sizeof saved_tss);
            for (int i = 0; i < 10; i++) get_transform(i,saved[i]);
            float matrix[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
            set_transform(0,matrix); set_transform(1,matrix);
            matrix[12] = .25f; matrix[13] = .5f; set_transform(6,matrix);
            matrix[12] = .125f; matrix[13] = 0; set_transform(2,matrix);
            ULONG def[60] = {0}, generated_ps = 0;
            def[8] = 8; def[9] = 0x18u<<8; def[54] = 4; /* PASSTHRU */
            create_ps(def,&generated_ps); set_ps(generated_ps);
            for (int i = 0; i < 4; i++) {
                set_tex(i,NULL); texture_states[i*32+28] = i;
                texture_states[i*32+21] = 0;
            }
            texture_states[21] = 3;
            render_states[59] = render_states[60] = render_states[82] = 0;
            render_states[92] = render_states[93] = render_states[109] = 0;
            render_states[118] = render_states[122] = render_states[124] = 0;
            render_states[67] = 0xffffffffu;
            render_states[106] = 0x40800000u;
            struct {float position[3],normal[3]; uint32_t color; float uv[2];} point =
                {{0,0,.25f},{0,0,.5f},0xffffffffu,{.9f,.9f}};
            set_vs(0x152);
            static const unsigned modes[] = {1,2,4,1};
            static const unsigned char expected[][3] = {{32,0,128},{96,128,64},{32,0,64},{32,0,255}};
            for (unsigned i = 0; i < 4; i++) {
                texture_states[28] = modes[i]<<16;
                render_states[123] = i == 3;
                clear(0,NULL,0xf0,0xff000000,1,0);
                draw(1,1,&point,sizeof point);
                glReadPixels(40,48,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                for (int k = 0; k < 3; k++) {
                    if (abs((int)pixel[k]-expected[i][k]) > 3)
                        fprintf(stderr,"texgen %u channel %d: %u expected %u\n",i,k,pixel[k],expected[i][k]);
                    assert(abs((int)pixel[k]-expected[i][k]) <= 3);
                }
            }
            set_ps(0); set_vs(0x144); set_tex(0,tex);
            for (int i = 0; i < 10; i++) set_transform(i,saved[i]);
            memcpy(render_states,saved_rs,sizeof saved_rs); memcpy(texture_states,saved_tss,sizeof saved_tss);
            puts("renderer texgen: normal/eye/object generation, matrix order and unlit normalization passed");
        }


        /* Integer depth/stencil texture views retain independent mip contents,
         * and CopyRects preserves the untouched destination depth and stencil. */
        for (unsigned depth_format=0x2a; depth_format<=0x2c; depth_format+=2) {
            void *color = create_tex(8,8,1,2,0,6,3), *depth = create_tex(8,8,1,2,0,depth_format,3);
            void *color0=NULL,*color1=NULL,*depth0=NULL,*depth1=NULL;
            assert(surface(color,0,&color0)==0 && surface(color,1,&color1)==0);
            assert(surface(depth,0,&depth0)==0 && surface(depth,1,&depth1)==0);
            set_tex(0,NULL);
            set_rt(color0,depth0); clear(0,NULL,0xf3,0xff000000,.25f,0x12);
            set_rt(color1,depth1); clear(0,NULL,0xf3,0xff000000,.75f,0x34);
            set_rt(color0,depth0);
            float z=0; unsigned char stencil=0;
            glReadPixels(2,2,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&z);
            if (depth_format==0x2a) glReadPixels(2,2,1,1,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,&stencil);
            assert(z>.249f && z<.251f && (depth_format!=0x2a || stencil==0x12));
            LONG rect[4]={0,0,2,2},point[2]={0,0};
            copy_rects(depth0,rect,1,depth1,point);
            set_rt(color1,depth1);
            glReadPixels(0,0,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&z);
            if (depth_format==0x2a) glReadPixels(0,0,1,1,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,&stencil);
            assert(z>.249f && z<.251f && (depth_format!=0x2a || stencil==0x12));
            glReadPixels(3,3,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&z);
            if (depth_format==0x2a) glReadPixels(3,3,1,1,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,&stencil);
            assert(z>.749f && z<.751f && (depth_format!=0x2a || stencil==0x34));
            set_rt(back,NULL); set_tex(0,tex);
            printf("renderer depth views: format %02x mip attachment and partial copy passed\n",depth_format);
        }

        /* Distinct standalone depth surfaces must not share a scratch image.
         * Copy into an already attached destination, then lock it read-only. */
        for(unsigned format=0x2a;format<=0x2c;format+=2){
            LONG (NTAPI *create_depth)(ULONG,ULONG,ULONG,ULONG,void **)=find("_D3DDevice_CreateDepthStencilSurface@20");
            void (NTAPI *lock_surface)(void *,ULONG *,const LONG *,ULONG)=find("_D3DSurface_LockRect@16");
            void *color=create_tex(8,8,1,1,0,6,3),*color_view=NULL,*depth_a=NULL,*depth_b=NULL;
            assert(surface(color,0,&color_view)==0);
            assert(create_depth(8,8,format,0,&depth_a)==0 && create_depth(8,8,format,0,&depth_b)==0);
            set_tex(0,NULL);
            set_rt(color_view,depth_a);clear(0,NULL,0xf3,0xff000000,.25f,0x12);
            set_rt(color_view,depth_b);clear(0,NULL,0xf3,0xff000000,.75f,0x34);
            set_rt(color_view,depth_a);
            float z=0;glReadPixels(1,1,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&z);
            assert(z>.249f && z<.251f);
            set_rt(color_view,depth_b);
            LONG rect[4]={0,0,2,2},point[2]={0,0};copy_rects(depth_a,rect,1,depth_b,point);
            glReadPixels(0,0,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&z);assert(z>.249f && z<.251f);
            glReadPixels(6,6,1,1,GL_DEPTH_COMPONENT,GL_FLOAT,&z);assert(z>.749f && z<.751f);
            ULONG locked[2];lock_surface(depth_b,locked,NULL,0x80);
            unsigned char *data=(void *)locked[1];
            if(format==0x2a){
                uint32_t copied=*(uint32_t *)data,untouched=*(uint32_t *)(data+6*locked[0]+6*4);
                assert((copied&255)==0x12 && (untouched&255)==0x34);
                assert((copied>>8)>4190000 && (copied>>8)<4200000);
                assert((untouched>>8)>12580000 && (untouched>>8)<12590000);
            }else{
                uint16_t copied=*(uint16_t *)data,untouched=*(uint16_t *)(data+6*locked[0]+6*2);
                assert(copied>16370 && copied<16400 && untouched>49140 && untouched<49170);
            }
            set_rt(back,NULL);set_tex(0,tex);
        }
        puts("renderer standalone depth: independent images, active partial copies and read-only locks passed");

        /* Same-format DXT copies preserve whole encoded blocks and support
         * cached destinations, overlap and the final sub-4x4 mip footprint. */
        {
            static const unsigned formats[3]={0x0c,0x0e,0x0f};
            set_vs(0x144); set_ps(control_ps);
            texture_states[3]=texture_states[4]=1;
            texture_states[5]=texture_states[9]=texture_states[11]=texture_states[21]=0;
            for (int format=0; format<3; format++) {
                unsigned bytes=format?16:8, color_offset=format?8:0;
                void *src_tex=create_tex(8,4,1,1,0,formats[format],3), *src_view=NULL;
                void *dst_tex=create_tex(8,4,1,1,0,formats[format],3), *dst_view=NULL;
                assert(surface(src_tex,0,&src_view)==0 && surface(dst_tex,0,&dst_view)==0);
                for (int target=0; target<2; target++) {
                    ULONG locked[2]; lock_tex(target?dst_tex:src_tex,0,locked,NULL,0);
                    unsigned char *data=(void *)locked[1];
                    memset(data,0,2*bytes);
                    for (int block=0; block<2; block++) {
                        unsigned char *b=data+block*bytes;
                        if (format==1) memset(b,255,8);
                        if (format==2) b[0]=b[1]=255;
                        unsigned endpoint=target?0x001f:(block?0x07e0:0xf800);
                        b[color_offset]=endpoint&255; b[color_offset+1]=endpoint>>8;
                    }
                }
                /* Populate the destination's GL cache before mutating it. */
                set_tex(0,dst_tex);
                for (int i=0;i<3;i++) textured[i].u=textured[i].v=.5f;
                draw(5,3,textured,sizeof textured[0]);
                LONG rect[4]={4,0,8,4},point[2]={0,0};
                copy_rects(src_view,rect,1,dst_view,point);
                for (unsigned side=0;side<2;side++) {
                    for (int i=0;i<3;i++) textured[i].u=side?.75f:.25f;
                    clear(0,NULL,0xf0,0xff000000,1,0);
                    draw(5,3,textured,sizeof textured[0]);
                    glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                    assert(pixel[side?2:1]>240 && pixel[side?1:2]<8 && pixel[0]<8);
                }
                rect[0]=0;rect[2]=4;point[0]=4;
                copy_rects(dst_view,rect,1,dst_view,point);
                clear(0,NULL,0xf0,0xff000000,1,0);
                draw(5,3,textured,sizeof textured[0]);
                glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
                assert(pixel[1]>240 && pixel[0]<8 && pixel[2]<8);
                void *small_a=create_tex(2,2,1,1,0,formats[format],3),*small_b=create_tex(2,2,1,1,0,formats[format],3);
                void *small_src=NULL,*small_dst=NULL;
                ULONG locked_a[2],locked_b[2];
                lock_tex(small_a,0,locked_a,NULL,0);lock_tex(small_b,0,locked_b,NULL,0);
                unsigned char *block=(void *)locked_a[1];
                memset(block,0,bytes); if(format==1)memset(block,255,8); if(format==2)block[0]=block[1]=255;
                block[color_offset]=0;block[color_offset+1]=0xf8;
                memset((void *)locked_b[1],0,bytes);
                assert(surface(small_a,0,&small_src)==0 && surface(small_b,0,&small_dst)==0);
                copy_rects(small_src,NULL,0,small_dst,NULL);
                assert(!memcmp((void *)locked_a[1],(void *)locked_b[1],bytes));
            }
            set_ps(0);set_tex(0,tex);
            puts("renderer DXT copies: DXT1/3/5 cached blocks, overlap and small footprints passed");
        }

        /* Compressed volume uploads decode the packed slab order before GL.
         * The selected blocks distinguish XY tiles, slices within a slab,
         * and the second slab. Minification then selects an authored mip. */
        {
            void (NTAPI *lock_volume)(void *,ULONG,ULONG *,const LONG *,ULONG) =
                find("_D3DVolumeTexture_LockBox@20");
            LONG (NTAPI *get_volume)(void *,ULONG,void **)=find("_D3DVolumeTexture_GetVolumeLevel@12");
            void (NTAPI *lock_view)(void *,ULONG *,const LONG *,ULONG)=find("_D3DVolume_LockBox@16");
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
                /* Whole-origin boxes and volume views must address the same
                 * authored mip storage, including a final partial block.
                 * These checks make no claim about nonzero packed-slab origins. */
                for(unsigned level=0;level<2;level++){
                    ULONG whole[3],boxed[3],view_lock[3];void *view=NULL;
                    lock_volume(volume_tex,level,whole,NULL,0x80);
                    LONG box[6]={0,0,4,4,0,1};
                    lock_volume(volume_tex,level,boxed,box,0x80);
                    assert(whole[2]==boxed[2]);
                    assert(whole[0]==(level?1u:2u)*block_bytes);
                    assert(whole[1]==(level?1u:4u)*block_bytes);
                    assert(get_volume(volume_tex,level,&view)==0 && view);
                    lock_view(view,view_lock,box,0x80);
                    assert(!memcmp(view_lock,boxed,sizeof boxed));
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
