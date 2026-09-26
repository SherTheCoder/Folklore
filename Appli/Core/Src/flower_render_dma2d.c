#include "main.h"
#include "flower_render.h"
#include <stdio.h>
#include "flower_display.h"
#include "flower_config.h"
#if FLOWER_WARP_MVE
#include <arm_mve.h>
#endif
#include "flower_ctl.h"
#include <math.h>
#include <string.h>

#define DMA2D_BLIT_TIMEOUT_MS   10u

#define MAX_BLITS   (FLOWER_ELEMENT_COUNT * FLOWER_POSE_COUNT)

typedef struct
{
    const uint8_t *src;
    int16_t  x, y;
    int16_t  w, h;
    uint16_t src_stride_px;
    uint8_t  format;
    uint8_t  alpha;
} flower_blit_t;

typedef struct
{
    int16_t x0, y0, x1, y1;
} flower_rect_t;

static DMA2D_HandleTypeDef s_hdma2d;
static flower_blit_t       s_blits[MAX_BLITS];

static flower_rect_t s_fb_dirty[2];
static flower_rect_t s_compose_dirty;

static void rect_clear(flower_rect_t *r)
{
    r->x0 = 0; r->y0 = 0; r->x1 = 0; r->y1 = 0;
}

static int rect_empty(const flower_rect_t *r)
{
    return (r->x1 <= r->x0) || (r->y1 <= r->y0);
}

static void rect_add(flower_rect_t *r, int x, int y, int w, int h)
{
    if ((w <= 0) || (h <= 0))
    {
        return;
    }
    if (rect_empty(r))
    {
        r->x0 = (int16_t)x;         r->y0 = (int16_t)y;
        r->x1 = (int16_t)(x + w);   r->y1 = (int16_t)(y + h);
        return;
    }
    if (x < r->x0)          { r->x0 = (int16_t)x; }
    if (y < r->y0)          { r->y0 = (int16_t)y; }
    if ((x + w) > r->x1)    { r->x1 = (int16_t)(x + w); }
    if ((y + h) > r->y1)    { r->y1 = (int16_t)(y + h); }
}

static void rect_union(flower_rect_t *r, const flower_rect_t *o)
{
    if (!rect_empty(o))
    {
        rect_add(r, o->x0, o->y0, o->x1 - o->x0, o->y1 - o->y0);
    }
}

int flower_render_init(void)
{
    s_hdma2d.Instance = DMA2D;
    s_hdma2d.Init.Mode         = DMA2D_M2M_BLEND;
    s_hdma2d.Init.ColorMode    = DMA2D_OUTPUT_RGB565;
    s_hdma2d.Init.OutputOffset = 0;
    if (HAL_DMA2D_Init(&s_hdma2d) != HAL_OK)
    {
        return -1;
    }

    flower_render_reset();
    return 0;
}

void flower_render_reset(void)
{
    rect_clear(&s_fb_dirty[0]);
    rect_clear(&s_fb_dirty[1]);
    rect_clear(&s_compose_dirty);
}

static void dma2d_failed(HAL_StatusTypeDef st)
{
    if (g_flower_dma2d_err == 0u)
    {
        g_flower_dma2d_code = ((uint32_t)st << 16) | (HAL_DMA2D_GetError(&s_hdma2d) & 0xFFFFu);
    }
    g_flower_dma2d_err++;
}

/* The offscreen buffer is ARGB4444 so the rotated flower keeps its alpha. */
static void dma2d_fill(uint16_t *dst, uint32_t stride_px,
                       int x, int y, int w, int h, uint32_t argb, int argb4444)
{
    HAL_StatusTypeDef st;

    if ((w <= 0) || (h <= 0))
    {
        return;
    }
    s_hdma2d.Init.Mode         = DMA2D_R2M;
    s_hdma2d.Init.ColorMode    = argb4444 ? DMA2D_OUTPUT_ARGB4444
                                          : DMA2D_OUTPUT_RGB565;
    s_hdma2d.Init.OutputOffset = stride_px - (uint32_t)w;
    st = HAL_DMA2D_Init(&s_hdma2d);
    if (st == HAL_OK)
    {
        st = HAL_DMA2D_Start(&s_hdma2d, argb,
                             (uint32_t)(dst + (uint32_t)y * stride_px + (uint32_t)x),
                             (uint32_t)w, (uint32_t)h);
    }
    if (st == HAL_OK)
    {
        st = HAL_DMA2D_PollForTransfer(&s_hdma2d, DMA2D_BLIT_TIMEOUT_MS);
    }
    if (st != HAL_OK)
    {
        dma2d_failed(st);
    }
}

static void dma2d_blit(const flower_blit_t *b, uint16_t *dst, uint32_t stride_px,
                       int argb4444)
{
    uint32_t input_alpha;
    HAL_StatusTypeDef st;

    s_hdma2d.Init.Mode         = DMA2D_M2M_BLEND;
    s_hdma2d.Init.ColorMode    = argb4444 ? DMA2D_OUTPUT_ARGB4444
                                          : DMA2D_OUTPUT_RGB565;
    s_hdma2d.Init.OutputOffset = stride_px - (uint32_t)b->w;
    (void)HAL_DMA2D_Init(&s_hdma2d);

    s_hdma2d.LayerCfg[0].InputColorMode = argb4444 ? DMA2D_INPUT_ARGB4444
                                                   : DMA2D_INPUT_RGB565;
    s_hdma2d.LayerCfg[0].InputOffset    = stride_px - (uint32_t)b->w;
    s_hdma2d.LayerCfg[0].AlphaMode      = DMA2D_NO_MODIF_ALPHA;
    s_hdma2d.LayerCfg[0].InputAlpha     = 0xFFu;
    s_hdma2d.LayerCfg[0].RedBlueSwap    = DMA2D_RB_REGULAR;
    s_hdma2d.LayerCfg[0].AlphaInverted  = DMA2D_REGULAR_ALPHA;
    (void)HAL_DMA2D_ConfigLayer(&s_hdma2d, 0);

    if (b->format == FLOWER_TEX_A8)
    {
        input_alpha = ((uint32_t)b->alpha << 24) | FLOWER_GLOW_RGB;
        s_hdma2d.LayerCfg[1].InputColorMode = DMA2D_INPUT_A8;
    }
    else
    {
        input_alpha = b->alpha;
        s_hdma2d.LayerCfg[1].InputColorMode = DMA2D_INPUT_ARGB4444;
    }
    s_hdma2d.LayerCfg[1].InputOffset   = (uint32_t)b->src_stride_px - (uint32_t)b->w;
    s_hdma2d.LayerCfg[1].AlphaMode     = DMA2D_COMBINE_ALPHA;
    s_hdma2d.LayerCfg[1].InputAlpha    = input_alpha;
    s_hdma2d.LayerCfg[1].RedBlueSwap   = DMA2D_RB_REGULAR;
    s_hdma2d.LayerCfg[1].AlphaInverted = DMA2D_REGULAR_ALPHA;
    (void)HAL_DMA2D_ConfigLayer(&s_hdma2d, 1);

    {
        uint32_t dst_addr = (uint32_t)(dst + (uint32_t)b->y * stride_px + (uint32_t)b->x);
        st = HAL_DMA2D_BlendingStart(&s_hdma2d, (uint32_t)b->src, dst_addr, dst_addr,
                                     (uint32_t)b->w, (uint32_t)b->h);
        if (st == HAL_OK)
        {
            st = HAL_DMA2D_PollForTransfer(&s_hdma2d, DMA2D_BLIT_TIMEOUT_MS);
        }
        if (st != HAL_OK)
        {
            dma2d_failed(st);
        }
    }
}

/* Returns 0 when the element is off-screen; DMA2D does no clipping itself. */
static int build_blit(flower_blit_t *out, const flower_tex_t *tex,
                      float px, float py, float alpha,
                      int dst_ox, int dst_oy, int dst_w, int dst_h)
{
    int x, y, w, h, sx, sy;
    int a = (int)(alpha * 255.0f + 0.5f);

    if (a <= 0)
    {
        return 0;
    }
    if (a > 255)
    {
        a = 255;
    }

    x = (int)lrintf(px - (float)tex->pivot_x_q4 / 16.0f) - dst_ox;
    y = (int)lrintf(py - (float)tex->pivot_y_q4 / 16.0f) - dst_oy;
    w = (int)tex->w;
    h = (int)tex->h;

    sx = 0;
    sy = 0;
    if (x < 0)          { sx = -x; w -= sx; x = 0; }
    if (y < 0)          { sy = -y; h -= sy; y = 0; }
    if ((x + w) > dst_w) { w = dst_w - x; }
    if ((y + h) > dst_h) { h = dst_h - y; }
    if ((w <= 0) || (h <= 0))
    {
        return 0;
    }

    out->src = flower_atlas_blob + tex->offset +
               ((uint32_t)sy * tex->w + (uint32_t)sx) *
               ((tex->format == FLOWER_TEX_A8) ? 1u : 2u);
    out->x             = (int16_t)x;
    out->y             = (int16_t)y;
    out->w             = (int16_t)w;
    out->h             = (int16_t)h;
    out->src_stride_px = tex->w;
    out->format        = tex->format;
    out->alpha         = (uint8_t)a;
    return 1;
}

static int build_frame(const flower_draw_t *draws, float scale,
                       uint32_t kind_mask,
                       int dst_ox, int dst_oy, int dst_w, int dst_h,
                       flower_rect_t *bounds, int n)
{
    uint32_t i;

    for (i = 0u; i < (uint32_t)FLOWER_ELEMENT_COUNT; i++)
    {
        const flower_element_t *el = &flower_elements[i];
        const flower_draw_t    *d  = &draws[i];
        float pose, blend, px, py;
        int   lo, hi;

        if ((kind_mask & (1u << el->kind)) == 0u)
        {
            continue;
        }
        if (d->alpha <= 0.0f)
        {
            continue;
        }
        if (n > (MAX_BLITS - FLOWER_POSE_COUNT))
        {
            break;
        }

        px = FLOWER_CENTRE_X +
             ((float)el->pivot_fx_q4 / 16.0f - FLOWER_CENTRE_X) * scale + d->dx;
        py = FLOWER_CENTRE_Y +
             ((float)el->pivot_fy_q4 / 16.0f - FLOWER_CENTRE_Y) * scale + d->dy;

#if FLOWER_POSE_COUNT < 2
        pose = 0.0f; lo = 0; hi = 0; blend = 0.0f;
        (void)pose;
#else
        pose = d->pose;
        if (pose < 0.0f)                                  { pose = 0.0f; }
        if (pose > (float)(FLOWER_POSE_COUNT - 1))        { pose = (float)(FLOWER_POSE_COUNT - 1); }
        lo    = (int)pose;
        if (lo > (FLOWER_POSE_COUNT - 2))                 { lo = FLOWER_POSE_COUNT - 2; }
        if (lo < 0)                                       { lo = 0; }
        hi    = lo + 1;
        blend = pose - (float)lo;
#endif

        if (blend <= FLOWER_POSE_SNAP_LO)
        {
            if (build_blit(&s_blits[n], &flower_textures[el->tex[lo]],
                           px, py, d->alpha, dst_ox, dst_oy, dst_w, dst_h))
            {
                rect_add(bounds, s_blits[n].x, s_blits[n].y, s_blits[n].w, s_blits[n].h);
                n++;
            }
        }
        else if (blend >= FLOWER_POSE_SNAP_HI)
        {
            if (build_blit(&s_blits[n], &flower_textures[el->tex[hi]],
                           px, py, d->alpha, dst_ox, dst_oy, dst_w, dst_h))
            {
                rect_add(bounds, s_blits[n].x, s_blits[n].y, s_blits[n].w, s_blits[n].h);
                n++;
            }
        }
        else
        {
            if (build_blit(&s_blits[n], &flower_textures[el->tex[lo]],
                           px, py, d->alpha * (1.0f - blend), dst_ox, dst_oy, dst_w, dst_h))
            {
                rect_add(bounds, s_blits[n].x, s_blits[n].y, s_blits[n].w, s_blits[n].h);
                n++;
            }
            if (build_blit(&s_blits[n], &flower_textures[el->tex[hi]],
                           px, py, d->alpha * blend, dst_ox, dst_oy, dst_w, dst_h))
            {
                rect_add(bounds, s_blits[n].x, s_blits[n].y, s_blits[n].w, s_blits[n].h);
                n++;
            }
        }
    }
    return n;
}

/* Inverse-mapped rotation, per-row DDA in 16.16 fixed point. */
static void warp_compose_to_fb(uint16_t *fb, const uint16_t *compose,
                               float rot_deg, flower_rect_t *bounds)
{
    const float rad = rot_deg * 3.14159265358979f / 180.0f;
    const float cr  = cosf(rad);
    const float sr  = sinf(rad);

    const float cxs[4] = { (float)FLOWER_COMPOSE_X0,
                           (float)(FLOWER_COMPOSE_X0 + FLOWER_COMPOSE_W),
                           (float)FLOWER_COMPOSE_X0,
                           (float)(FLOWER_COMPOSE_X0 + FLOWER_COMPOSE_W) };
    const float cys[4] = { (float)FLOWER_COMPOSE_Y0,
                           (float)FLOWER_COMPOSE_Y0,
                           (float)(FLOWER_COMPOSE_Y0 + FLOWER_COMPOSE_H),
                           (float)(FLOWER_COMPOSE_Y0 + FLOWER_COMPOSE_H) };
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    int x0, x1, y0, y1, x, y, k, tx, ty;

    const float a =  cr;
    const float b =  sr;
    const int32_t du_dx = (int32_t)lrintf(a * 65536.0f);
    const int32_t dv_dx = (int32_t)lrintf(-b * 65536.0f);

    for (k = 0; k < 4; k++)
    {
        float dx = cxs[k] - FLOWER_CENTRE_X;
        float dy = cys[k] - FLOWER_CENTRE_Y;
        float rx = FLOWER_CENTRE_X + dx * cr - dy * sr;
        float ry = FLOWER_CENTRE_Y + dx * sr + dy * cr;
        if (rx < minx) { minx = rx; }
        if (rx > maxx) { maxx = rx; }
        if (ry < miny) { miny = ry; }
        if (ry > maxy) { maxy = ry; }
    }

    x0 = (int)floorf(minx) - FLOWER_FB_ORIGIN_X - 1;
    x1 = (int)ceilf(maxx)  - FLOWER_FB_ORIGIN_X + 1;
    y0 = (int)floorf(miny) - FLOWER_FB_ORIGIN_Y - 1;
    y1 = (int)ceilf(maxy)  - FLOWER_FB_ORIGIN_Y + 1;

    if (x0 < 0)                      { x0 = 0; }
    if (y0 < 0)                      { y0 = 0; }
    if (x1 > (int)FLOWER_FB_W)       { x1 = (int)FLOWER_FB_W; }
    if (y1 > (int)FLOWER_FB_H)       { y1 = (int)FLOWER_FB_H; }
    if ((x1 <= x0) || (y1 <= y0))
    {
        rect_clear(bounds);
        return;
    }

    for (ty = y0; ty < y1; ty += FLOWER_WARP_TILE)
    {
    for (tx = x0; tx < x1; tx += FLOWER_WARP_TILE)
    {
    const int tx1 = (tx + FLOWER_WARP_TILE < x1) ? (tx + FLOWER_WARP_TILE) : x1;
    const int ty1 = (ty + FLOWER_WARP_TILE < y1) ? (ty + FLOWER_WARP_TILE) : y1;

    for (y = ty; y < ty1; y++)
    {
        float  fxp = (float)(tx + FLOWER_FB_ORIGIN_X) - FLOWER_CENTRE_X;
        float  fyp = (float)(y  + FLOWER_FB_ORIGIN_Y) - FLOWER_CENTRE_Y;
        int32_t u = (int32_t)lrintf((( a * fxp + b * fyp) + FLOWER_CENTRE_X
                                     - (float)FLOWER_COMPOSE_X0) * 65536.0f);
        int32_t v = (int32_t)lrintf(((-b * fxp + a * fyp) + FLOWER_CENTRE_Y
                                     - (float)FLOWER_COMPOSE_Y0) * 65536.0f);
        uint16_t *row = fb + (uint32_t)y * FLOWER_FB_W + (uint32_t)tx;

#if FLOWER_WARP_MVE
        {
            static const int32_t k0123[4] = { 0, 1, 2, 3 };
            const int32x4_t vk  = vld1q_s32(k0123);
            int32x4_t vu = vaddq_s32(vdupq_n_s32(u), vmulq_n_s32(vk, du_dx));
            int32x4_t vv = vaddq_s32(vdupq_n_s32(v), vmulq_n_s32(vk, dv_dx));
            const int32x4_t vdu = vdupq_n_s32(du_dx * 4);
            const int32x4_t vdv = vdupq_n_s32(dv_dx * 4);
            int32_t n = tx1 - tx;

            while (n > 0)
            {
                mve_pred16_t tp = vctp32q((uint32_t)n);
                int32x4_t ix = vshrq_n_s32(vu, 16);
                int32x4_t iy = vshrq_n_s32(vv, 16);
                mve_pred16_t p = vcmpgeq_m_n_s32(ix, 0, tp);
                uint32x4_t off, src, dst, av, ia, sr4, sg4, sb4, dr, dg, db;

                p = vcmpltq_m_n_s32(ix, FLOWER_COMPOSE_W - 1, p);
                p = vcmpgeq_m_n_s32(iy, 0, p);
                p = vcmpltq_m_n_s32(iy, FLOWER_COMPOSE_H - 1, p);

                off = vreinterpretq_u32_s32(vmlaq_n_s32(ix, iy, FLOWER_COMPOSE_W));
                src = vldrhq_gather_shifted_offset_z_u32(compose, off, p);
                dst = vldrhq_z_u32(row, tp);

                av = vandq_u32(vshrq_n_u32(src, 12), vdupq_n_u32(0x0Fu));
                ia = vsubq_u32(vdupq_n_u32(15u), av);

                sr4 = vandq_u32(vshrq_n_u32(src, 8), vdupq_n_u32(0x0Fu));
                sg4 = vandq_u32(vshrq_n_u32(src, 4), vdupq_n_u32(0x0Fu));
                sb4 = vandq_u32(src,                 vdupq_n_u32(0x0Fu));
                sr4 = vorrq_u32(vshlq_n_u32(sr4, 1), vshrq_n_u32(sr4, 3));
                sg4 = vorrq_u32(vshlq_n_u32(sg4, 2), vshrq_n_u32(sg4, 2));
                sb4 = vorrq_u32(vshlq_n_u32(sb4, 1), vshrq_n_u32(sb4, 3));

                dr = vandq_u32(vshrq_n_u32(dst, 11), vdupq_n_u32(0x1Fu));
                dg = vandq_u32(vshrq_n_u32(dst, 5),  vdupq_n_u32(0x3Fu));
                db = vandq_u32(dst,                  vdupq_n_u32(0x1Fu));

                sr4 = vshrq_n_u32(vmulq_n_u32(vaddq_u32(vmulq_u32(sr4, av),
                                                        vmulq_u32(dr, ia)), 4370u), 16);
                sg4 = vshrq_n_u32(vmulq_n_u32(vaddq_u32(vmulq_u32(sg4, av),
                                                        vmulq_u32(dg, ia)), 4370u), 16);
                sb4 = vshrq_n_u32(vmulq_n_u32(vaddq_u32(vmulq_u32(sb4, av),
                                                        vmulq_u32(db, ia)), 4370u), 16);

                dst = vorrq_u32(vorrq_u32(vshlq_n_u32(sr4, 11),
                                          vshlq_n_u32(sg4, 5)), sb4);
                vstrhq_p_u32(row, dst, tp);

                row += 4;
                n   -= 4;
                vu = vaddq_s32(vu, vdu);
                vv = vaddq_s32(vv, vdv);
            }
            (void)x;
        }
#else
        for (x = tx; x < tx1; x++)
        {
            int ix = u >> 16;
            int iy = v >> 16;
            uint32_t src = ((ix < 0) || (iy < 0) ||
                            (ix >= FLOWER_COMPOSE_W) || (iy >= FLOWER_COMPOSE_H))
                           ? 0u
                           : compose[(uint32_t)iy * FLOWER_COMPOSE_W + (uint32_t)ix];
            uint32_t av = (src >> 12) & 0x0Fu;

            if (av != 0u)
            {
                uint32_t ia  = 15u - av;
                uint32_t dst = *row;
                uint32_t sr4 = (src >> 8) & 0x0Fu;
                uint32_t sg4 = (src >> 4) & 0x0Fu;
                uint32_t sb4 =  src       & 0x0Fu;

                sr4 = (sr4 << 1) | (sr4 >> 3);
                sg4 = (sg4 << 2) | (sg4 >> 2);
                sb4 = (sb4 << 1) | (sb4 >> 3);

                sr4 = (sr4 * av + ((dst >> 11) & 0x1Fu) * ia) / 15u;
                sg4 = (sg4 * av + ((dst >> 5)  & 0x3Fu) * ia) / 15u;
                sb4 = (sb4 * av + ( dst        & 0x1Fu) * ia) / 15u;

                *row = (uint16_t)((sr4 << 11) | (sg4 << 5) | sb4);
            }
            row++;
            u += du_dx;
            v += dv_dx;
        }
#endif
    }
    }
    }

    rect_clear(bounds);
    rect_add(bounds, x0, y0, x1 - x0, y1 - y0);
}

void flower_render_frame(const flower_draw_t *draws,
                         float scale, float flower_scale, float rot_deg,
                         uint32_t *compose_us, uint32_t *warp_us)
{
    uint16_t *fb      = flower_display_back_buffer();
    uint16_t *compose = flower_display_compose_buffer();
    int rotating = (fabsf(rot_deg) > FLOWER_WARP_IDENTITY_ROT_EPS);
    flower_rect_t bounds;
    flower_rect_t static_bounds;
    flower_rect_t clear_rect;
    uint32_t t0, t1, t2;
    uint32_t back = flower_display_back_index();
    int n, i;

    t0 = flower_cycles();

    if (!rotating)
    {
        rect_clear(&bounds);
        n = build_frame(draws, scale, FLOWER_KMASK_STATIC,
                        FLOWER_FB_ORIGIN_X, FLOWER_FB_ORIGIN_Y,
                        (int)FLOWER_FB_W, (int)FLOWER_FB_H, &bounds, 0);
        n = build_frame(draws, flower_scale, FLOWER_KMASK_FLOWER,
                        FLOWER_FB_ORIGIN_X, FLOWER_FB_ORIGIN_Y,
                        (int)FLOWER_FB_W, (int)FLOWER_FB_H, &bounds, n);

        clear_rect = s_fb_dirty[back];
        if (!rect_empty(&clear_rect))
        {
            dma2d_fill(fb, FLOWER_FB_W, clear_rect.x0, clear_rect.y0,
                       clear_rect.x1 - clear_rect.x0,
                       clear_rect.y1 - clear_rect.y0, 0x00000000u, 0);
        }

        for (i = 0; i < n; i++)
        {
            dma2d_blit(&s_blits[i], fb, FLOWER_FB_W, 0);
        }

        s_fb_dirty[back] = bounds;
        t1 = flower_cycles();
        t2 = t1;
    }
    else
    {
        clear_rect = s_fb_dirty[back];
        if (!rect_empty(&clear_rect))
        {
            dma2d_fill(fb, FLOWER_FB_W, clear_rect.x0, clear_rect.y0,
                       clear_rect.x1 - clear_rect.x0,
                       clear_rect.y1 - clear_rect.y0, 0x00000000u, 0);
        }

        rect_clear(&static_bounds);
        n = build_frame(draws, scale, FLOWER_KMASK_STATIC,
                        FLOWER_FB_ORIGIN_X, FLOWER_FB_ORIGIN_Y,
                        (int)FLOWER_FB_W, (int)FLOWER_FB_H, &static_bounds, 0);
        for (i = 0; i < n; i++)
        {
            dma2d_blit(&s_blits[i], fb, FLOWER_FB_W, 0);
        }

        rect_clear(&bounds);
        n = build_frame(draws, flower_scale, FLOWER_KMASK_FLOWER,
                        FLOWER_COMPOSE_X0, FLOWER_COMPOSE_Y0,
                        (int)FLOWER_COMPOSE_W, (int)FLOWER_COMPOSE_H,
                        &bounds, 0);

        clear_rect = s_compose_dirty;
        rect_union(&clear_rect, &bounds);
        if (!rect_empty(&clear_rect))
        {
            dma2d_fill(compose, FLOWER_COMPOSE_W, clear_rect.x0, clear_rect.y0,
                       clear_rect.x1 - clear_rect.x0,
                       clear_rect.y1 - clear_rect.y0, 0x00000000u, 1);
        }

        for (i = 0; i < n; i++)
        {
            dma2d_blit(&s_blits[i], compose, FLOWER_COMPOSE_W, 1);
        }
        s_compose_dirty = bounds;

        t1 = flower_cycles();

#if FLOWER_WARP_PROFILE
        {
        uint32_t ta, tb, tc;
        ta = flower_cycles();
#endif
#if FLOWER_ENABLE_DCACHE
        SCB_InvalidateDCache_by_Addr((void *)compose, (int32_t)FLOWER_COMPOSE_BYTES);
#endif
#if FLOWER_WARP_PROFILE
        tb = flower_cycles();
#endif

#if FLOWER_WARP_PROFILE
        tc = flower_cycles();
#endif
        warp_compose_to_fb(fb, compose, rot_deg, &bounds);
        rect_union(&bounds, &static_bounds);
        s_fb_dirty[back] = bounds;

        t2 = flower_cycles();
#if FLOWER_WARP_PROFILE
        printf("  warp parts: inval=%luus clear=%luus pixels=%luus "
               "(%dx%d dest)\r\n",
               (unsigned long)((tb - ta) / flower_cycles_per_us()),
               (unsigned long)((tc - tb) / flower_cycles_per_us()),
               (unsigned long)((t2 - tc) / flower_cycles_per_us()),
               bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
        }
#endif
    }

    if (compose_us != NULL) { *compose_us = (t1 - t0) / flower_cycles_per_us(); }
    if (warp_us    != NULL) { *warp_us    = (t2 - t1) / flower_cycles_per_us(); }
}
