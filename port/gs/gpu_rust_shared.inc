/* The Rust renderer's C glue shared by x64 (gpu_rust.c, gpu_rust_native.inc) and the browser
   (port/web/gpu_rust_web.c, gpu_rust_web_native.inc): the device ops that are one dhdr_exact_*
   call each, op_clip, Native's members (with native_core.inc and native_snapshot.inc), and the
   native backend functions both sides have alike.

   Included by each side's native .inc, after NativeExact (the provider, Rust or Web: an Exact, then
   DhdrExact *x; members native, clip_on, clip[4]), NATIVE_GPU_SIDE (its own Native members) and
   NATIVE_SCALE_MAX (in the browser also NATIVE_INLINE and the gs_native_* renames). The side then
   defines what differs: op_read (the browser suspends), op_clock, op_draw_skipped,
   native_replacement_view (texture packs), native_match_window, native_present (the window and
   pacing, or the canvas at web_present), native_read_display and native_init. */

#include "dhd_render_ext.h"       /* dhdr_native_picture_indexed (-Iport/render/include) */

typedef DhdrTexture *NativeView;   /* a replacement: a texture pack's or the sharp font's */
/* A decoded texture: its Rust texture (gpu; native_core.inc asks for it as texture) and its sharp
   font, or NULL. */
#define NATIVE_TEXTURE_GPU union { DhdrTexture *gpu; DhdrTexture *texture; }; DhdrTexture *sharp;
/* A surface's texture lives in Rust under the surface's index; native_core.inc only asks whether
   it exists (texture, srv: 1 once dhdr_native_surface made it). */
#define NATIVE_SURFACE_GPU unsigned texture, srv;
#define NATIVE_GPU \
    DhdrNative *r;                     /* the Rust renderer (x64: render thread, under context_lock) */ \
    ReplaceDevice replace;             /* replacements over dhdr_replace_create */ \
    struct NativeSnapshot *snapshot;   /* the picture handed to dhdr_native_picture */ \
    int dual;                          /* asked for dual-source blending */ \
    int present_failed;                /* dhdr_native_present failed: no more presents */ \
    NATIVE_GPU_SIDE

#include "native_core.inc"

#include "native_snapshot.inc"   /* NativeSnapshot, native_snapshot: Native's field as a DhdrPicture */

/* ---- the device ops (gpu_exact.inc's op_* seam): one dhdr_exact_* call each ---- */

static void op_write(Exact *gpu, int buffer, uint32_t offset, const void *data, uint32_t bytes)
{
    dhdr_exact_write(((NativeExact *)gpu)->x, (uint32_t)buffer, offset, data, bytes);
}

static void op_copy(Exact *gpu, int from, uint32_t from_offset, int to, uint32_t to_offset, uint32_t bytes)
{
    dhdr_exact_copy(((NativeExact *)gpu)->x, (uint32_t)from, from_offset, (uint32_t)to, to_offset, bytes);
}

static void op_batch(Exact *gpu, uint32_t offset, uint32_t x, uint32_t y)
{
    dhdr_exact_batch(((NativeExact *)gpu)->x, offset, x, y);
}

/* P_COPY, or P_SCANOUT with the picture's size in the high halves: Rust sizes the display texture
   and logs a failure itself (the record is then dropped, and the capture finds no picture), so
   this always reports 1. */
static int op_record(Exact *gpu, int pipeline, uint32_t x, uint32_t y)
{
    dhdr_exact_record(((NativeExact *)gpu)->x, (uint32_t)pipeline, x, y);
    return 1;
}

static void op_submit(Exact *gpu)
{
    dhdr_exact_submit(((NativeExact *)gpu)->x);
}

/* The native renderer's probe window (native_core.inc sets it; never on without DHD_RENDER=native). */
static int op_clip(const Exact *gpu, int box[4])
{
    const NativeExact *p = (const NativeExact *)gpu;
    if (!p->clip_on) return 0;
    memcpy(box, p->clip, sizeof p->clip);
    return 1;
}

/* ---- replacement textures (replace_texture.h; x64: loader threads and the GS worker) ---- */

static void *native_replace_create(void *context, const uint8_t *pixels, unsigned w, unsigned h, unsigned levels)
{
    (void)context;
    return dhdr_replace_create(pixels, w, h, levels);
}
static void native_replace_retain(void *view) { dhdr_texture_retain(view); }
static void native_replace_release(void *view) { dhdr_texture_release(view); }

/* ---- decoded textures (GS worker; dhdr_texture_* run on any thread) ---- */

static int native_texture_create(Native *n, NativeTexture *t, const uint32_t *pixels, unsigned w, unsigned h,
                                 int sharp, uint64_t key)
{
    if (!(t->gpu = dhdr_texture_create(pixels, w, h))) return 0;
    if (sharp) t->sharp = font_sharp_view(&n->replace, pixels, w, h, key);
    return 1;
}

static void native_texture_release(NativeTexture *t)
{
    dhdr_texture_release(t->gpu);
    dhdr_texture_release(t->sharp);
    t->gpu = NULL; t->sharp = NULL;
}

/* ---- the picture (x64: render thread, context lock held) ---- */

/* The surface's texture at its scale and at least want[] GS pixels, keeping what it holds (Rust
   grows it and copies the old contents in; new texels are zero). On failure the old texture, if
   any, stays. */
static int native_surface_resources(Native *n, NativeSurface *s, const unsigned want[2])
{
    if (!s->scale) s->scale = s->bw >= NATIVE_SCALED_BW ? n->scale : 1;
    unsigned w = want[0] * s->scale, h = want[1] * s->scale;
    if (s->texture && s->size[0] >= w && s->size[1] >= h) return 1;
    uint32_t size[2] = {0, 0};
    if (!dhdr_native_surface(n->r, (uint32_t)(s - n->surfaces), s->scale, w, h, s->depth, size)) return s->texture != 0;
    s->size[0] = size[0]; s->size[1] = size[1];
    s->texture = s->srv = 1;
    return 1;
}

/* DHD_SCALE=window: the smallest scale whose picture is as tall as the window (the canvas in the
   browser). Per side. */
static unsigned native_match_window(Native *n, const NativeFrame *f);

/* One picture: the field's commands (with these vertices) drawn and scanned out, then
   native_present (x64: presented at once; the browser: shown by the web_present that follows). */
static void native_picture(Native *n, NativeFrame *f, const NativeVertex *vertices, float t)
{
    static int spikes = -1;
    if (spikes < 0) spikes = getenv("DHD_SPIKES") != NULL;
    double start = n->trace_pictures || spikes ? host_wall_seconds() : 0;
    if (!n->scale) n->scale = n->scale_setting ? n->scale_setting : native_match_window(n, f);
    native_prepare_surfaces(n, f);
    double prepared = spikes ? host_wall_seconds() : 0;
    const DhdrPicture *picture = native_snapshot(n->snapshot, n, f, vertices);
    double snapped = spikes ? host_wall_seconds() : 0;
    DhdrPictureResult r;
    memset(&r, 0, sizeof r);
    int shown = 0;
    /* An indexed field's draws are ranges of its indices (native_core.inc's Native indexed). */
    int drawn = n->indexed ? dhdr_native_picture_indexed(n->r, picture, f->indices, f->index_count, &r)
                           : dhdr_native_picture(n->r, picture, &r);
    if (drawn) {
        n->display_width = r.display_width; n->display_height = r.display_height;
        shown = r.shown != 0;
    }
    double encoded = spikes ? host_wall_seconds() : 0;
    if (n->trace_pictures) {   /* no GPU timestamps on WebGPU yet: gpu_ms -1 */
        fprintf(n->trace_pictures, "%llu,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                (unsigned long long)f->field, t, (host_wall_seconds() - start) * 1000, -1.0,
                (unsigned long long)r.draws, (unsigned long long)r.self_copies, (unsigned long long)r.self_copy_px,
                (unsigned long long)r.copies, (unsigned long long)r.copy_px, (unsigned long long)r.imports,
                (unsigned long long)r.import_px);
    }
    if (shown) native_present(n);  /* movies and loading cards honor the selected picture shape */
    if (spikes) {   /* the render thread's picture by part: surfaces, snapshot, draw, present */
        double end = host_wall_seconds();
        if (end - start >= 0.004)
            fprintf(stderr, "spike_render: field=%llu start=%.9f ms=%.3f prepare_ms=%.3f snapshot_ms=%.3f draw_ms=%.3f present_ms=%.3f\n",
                    (unsigned long long)f->field, start, (end - start) * 1e3, (prepared - start) * 1e3,
                    (snapped - prepared) * 1e3, (encoded - snapped) * 1e3, (end - encoded) * 1e3);
    }
}

/* The displayed picture onto surface, configured to w x h (the window or the canvas): drawn in the
   rectangle that keeps its shape (native_present_rect at n->aspect), resize when the size changed.
   dhdr_native_present's answer: 1 drawn (the caller presents it), 0 skipped (no picture yet,
   timeout), -1 failed (then present_failed: no more presents). */
static int native_present_draw(Native *n, DhdrSurface *surface, unsigned w, unsigned h, int resize)
{
    DhdrPresentRect r;
    memset(&r, 0, sizeof r);
    NativeRect cut;
    native_present_rect(n, w, h, r.rect, &cut);
    r.cut[0] = cut.left < 0 ? 0 : cut.left;
    r.cut[1] = cut.top < 0 ? 0 : cut.top;
    r.cut[2] = cut.right > (int)w ? (int)w : cut.right;
    r.cut[3] = cut.bottom > (int)h ? (int)h : cut.bottom;
    r.window_w = w; r.window_h = h;
    r.resize = resize;
    int drawn = dhdr_native_present(n->r, surface, &r);
    if (drawn < 0) {
        n->present_failed = 1;
        fprintf(stderr, "gs native: presenting failed; no more pictures are shown\n");
    }
    return drawn;
}

/* DHD_PRESENT_FILTER=point keeps the picture's pixels sharp; linear by default. Then DHD_ASPECT. */
static void native_present_options(Native *n)
{
    const char *present_filter = getenv("DHD_PRESENT_FILTER");
    dhdr_native_options(n->r, present_filter && !strcmp(present_filter, "point") ? DHDR_NATIVE_POINT_FILTER : 0);
    native_aspect_options(n);
}

/* ---- creation ---- */

/* native_init's common half: the Native over exact, the dual-source choice (DHD_WGPU_DUAL_SOURCE=0
   turns it off), the Rust native renderer, the present options and the replacements' device. NULL
   when unavailable (g keeps the exact renderer); *start: the time before Rust built its shaders and
   pipelines. */
static Native *native_create_rust(NativeExact *exact, double *start)
{
    Native *n = native_create(exact);
    if (!n) return NULL;
    /* n->color_pixel: the page word -> pixel map native_write_high places alpha-upload bits with
       (without it every bit lands on pixel 0). Rust builds its own copy of the table for
       native_convert (init.rs inverse_table); C needs only color_pixel from it. */
    static uint32_t inverse[4096];
    native_inverse_table(n, inverse);
    const char *dual = getenv("DHD_WGPU_DUAL_SOURCE");
    n->dual = (dhdr_device_features() & DHDR_FEAT_DUAL_SOURCE) && !(dual && !atoi(dual));
    *start = host_wall_seconds();
    if (!(n->r = dhdr_native_create(n->dual ? DHDR_NATIVE_DUAL : 0)) ||
        !(n->snapshot = calloc(1, sizeof *n->snapshot))) {
        fprintf(stderr, "gs native: WebGPU objects unavailable on this device\n");
        dhdr_native_destroy(n->r);
        return NULL;   /* leaks the shared state: once, at start-up */
    }
    native_present_options(n);
    /* Replacements (x64: a texture pack and the sharp font; the browser: the sharp font): Rust
       samples them with mipmaps, anisotropic when the GS filters, point magnification when it
       doesn't, with clamped or repeating coordinates as the GS has them. */
    n->replace_on = 1;
    n->replace.create = native_replace_create; n->replace.retain = native_replace_retain;
    n->replace.release = native_replace_release; n->replace.context = n;
    return n;
}
