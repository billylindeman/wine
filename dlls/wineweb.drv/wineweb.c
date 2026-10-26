/*
 * wineweb.drv — the wineweb graphics driver, guest side (docs/07 §1).
 *
 * What owns this: the P3.3 block (docs/spikes/p3-user32.md). This is the
 * third member of Wine's graphics-driver family (winex11.drv, winemac.drv,
 * wineandroid.drv, …) for the browser platform: user32/gdi32 route their
 * window and GDI operations through here, and the driver marshals them to
 * the versioned `wineweb_drv` C ABI v1 (crates/wineweb-drv/src/abi.rs) —
 * the in-process seam to the host half (window table, surfaces, dirty-rect
 * compositor) inside the single wasm module (docs/01 §2).
 *
 * What it must never do: allocate guest memory (all state is static), block
 * (present is an explicit frame push, docs/04 §5), or guess at host state —
 * every window dimension here is bookkeeping for clipping only; the host
 * half's table is authoritative and `surface_write` rejects out-of-bounds
 * rects rather than clipping for us.
 *
 * GDI software path (docs/07 §1 "GDI software path" row): solid fills and
 * text rasterize into RGBA8888 sub-rects handed to `surface_write`. Text is
 * the bundled 8x8 bitmap font (font8x8.h, public domain) — GetGlyphOutline-
 * quality rendering is a later block. Only OPAQUE background mode is
 * implemented: the ABI has no surface read, so transparent text would need
 * a guest-side shadow copy of the host surface (TODO(P4) if a game needs
 * it); anything else is loud.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "font8x8.h"

/* --- the wineweb_drv C ABI v1 (crates/wineweb-drv/src/abi.rs) -------------
 * Resolved at the final-module link: the Rust host half exports these (the
 * P3.4 single-module integration); the P3.3 harness provides contract shims
 * with the same names and statuses. Host-width rule: pointers/lengths are
 * host-size_t (32-bit on core32); coordinates i32; sizes u32; handles u64.
 */
extern int32_t wineweb_drv_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                         const uint8_t *title, size_t title_len,
                                         uint64_t *out_hwnd);
extern int32_t wineweb_drv_destroy_window(uint64_t hwnd);
extern int32_t wineweb_drv_move_window(uint64_t hwnd, int32_t x, int32_t y);
extern int32_t wineweb_drv_resize_window(uint64_t hwnd, uint32_t w, uint32_t h);
extern int32_t wineweb_drv_show_window(uint64_t hwnd, int32_t visible);
extern int32_t wineweb_drv_set_text(uint64_t hwnd, const uint8_t *text, size_t text_len);
extern int32_t wineweb_drv_surface_write(uint64_t hwnd, int32_t x, int32_t y, uint32_t w,
                                         uint32_t h, const uint8_t *pixels);
extern int32_t wineweb_drv_present(void);

/* The S2 diag group (docs/04 §4) — the driver's loud channel. */
__attribute__((import_module("wineweb_host"), import_name("log")))
extern void wineweb_host_log(int32_t level, const uint8_t *msg, uint32_t len);

static void drv_log(const char *msg)
{
  wineweb_host_log(1 /* warn */, (const uint8_t *)msg, (uint32_t)strlen(msg));
}

/* --- driver state ---------------------------------------------------------
 * Guest-window dimensions for clipping (docs/01 §6: no guest memory for
 * host state — this is the guest side's own bookkeeping, host state lives
 * in the wineweb-drv crate).
 */
#define DRV_MAX_WINDOWS 32 /* the dialog's 8 controls + the popups ride the demo's census (S11) */

struct drv_window
{
    uint64_t hwnd;
    uint32_t w, h;
    int used;
};

static struct drv_window drv_windows[DRV_MAX_WINDOWS];

static struct drv_window *drv_find(uint64_t hwnd)
{
    unsigned i;
    for (i = 0; i < DRV_MAX_WINDOWS; i++)
        if (drv_windows[i].used && drv_windows[i].hwnd == hwnd) return &drv_windows[i];
    return NULL;
}

int32_t drv_init(void)
{
    unsigned i;
    for (i = 0; i < DRV_MAX_WINDOWS; i++) drv_windows[i].used = 0;
    return 0;
}

/* --- window management (docs/07 §1 row 1) -------------------------------- */

int32_t drv_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          const uint8_t *title, uint32_t title_len, uint64_t *out_hwnd)
{
    unsigned i;
    int32_t status = wineweb_drv_create_window(x, y, w, h, title, title_len, out_hwnd);
    if (status != 0) return status;
    for (i = 0; i < DRV_MAX_WINDOWS; i++)
    {
        if (!drv_windows[i].used)
        {
            drv_windows[i].hwnd  = *out_hwnd;
            drv_windows[i].w     = w;
            drv_windows[i].h     = h;
            drv_windows[i].used  = 1;
            return 0;
        }
    }
    drv_log("wineweb.drv: window table full");
    return -100;
}

int32_t drv_destroy_window(uint64_t hwnd)
{
    struct drv_window *win = drv_find(hwnd);
    int32_t status = wineweb_drv_destroy_window(hwnd);
    if (win) win->used = 0;
    return status;
}

int32_t drv_move_window(uint64_t hwnd, int32_t x, int32_t y)
{
    return wineweb_drv_move_window(hwnd, x, y);
}

int32_t drv_resize_window(uint64_t hwnd, uint32_t w, uint32_t h)
{
    struct drv_window *win = drv_find(hwnd);
    int32_t status = wineweb_drv_resize_window(hwnd, w, h);
    if (win && status == 0) { win->w = w; win->h = h; } /* surface reset: guest repaints */
    return status;
}

int32_t drv_show_window(uint64_t hwnd, int32_t visible)
{
    return wineweb_drv_show_window(hwnd, visible);
}

int32_t drv_set_text(uint64_t hwnd, const uint8_t *text, uint32_t len)
{
    return wineweb_drv_set_text(hwnd, text, len);
}

int32_t drv_present(void)
{
    return wineweb_drv_present();
}

/* --- GDI software path (docs/07 §1 row 3) -------------------------------- */

static void put_pixel(uint8_t *row, uint32_t x, uint32_t rgba)
{
    row[x * 4 + 0] = (uint8_t)(rgba >> 16); /* R */
    row[x * 4 + 1] = (uint8_t)(rgba >> 8);  /* G */
    row[x * 4 + 2] = (uint8_t)rgba;         /* B */
    row[x * 4 + 3] = 0xff;                  /* A */
}

/* Solid fill: one clipped row at a time through the static row scratch
 * (docs/01 §6: static state, no dispatch-time allocation). */
int32_t drv_fill_rect(uint64_t hwnd, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t rgba)
{
    static uint8_t row[4096 * 4];
    struct drv_window *win = drv_find(hwnd);
    uint32_t y0, y1, x0, x1, yy;
    int32_t status;

    if (!win) return -1; /* unknown window: the host half is authoritative */
    if (w > 4096) { drv_log("wineweb.drv: fill wider than 4096 loud-unsupported"); return -101; }

    /* clip to the window (surface-local coords; the host rejects OOB) */
    x0 = x < 0 ? 0 : (uint32_t)x;
    y0 = y < 0 ? 0 : (uint32_t)y;
    x1 = x0 + w > win->w ? win->w : x0 + w;
    y1 = y0 + h > win->h ? win->h : y0 + h;
    if (x0 >= x1 || y0 >= y1) return 0; /* nothing visible */

    for (yy = y0; yy < y1; yy++)
    {
        uint32_t xx;
        for (xx = x0; xx < x1; xx++) put_pixel(row, xx - x0, rgba);
        status = wineweb_drv_surface_write(hwnd, (int32_t)x0, (int32_t)yy, x1 - x0, 1, row);
        if (status != 0) return status;
    }
    return 0;
}

/* Text: one 8x8 glyph cell per character through the static cell scratch.
 * Opaque mode paints the full cell (fg where the font bit is set, bg
 * elsewhere); transparent mode is loud-unsupported (see header). */
#define DRV_GLYPH_W 8
#define DRV_GLYPH_H 8

int32_t drv_text_out(uint64_t hwnd, int32_t x, int32_t y, const uint8_t *ascii, uint32_t len,
                     uint32_t fg, uint32_t bg, int32_t opaque)
{
    static uint8_t cell[DRV_GLYPH_W * DRV_GLYPH_H * 4];
    struct drv_window *win = drv_find(hwnd);
    uint32_t i;

    if (!win) return -1;
    if (!opaque)
    {
        drv_log("wineweb.drv: TRANSPARENT bk mode loud-unsupported (TODO(P4))");
        return -102;
    }

    for (i = 0; i < len; i++)
    {
        const char *glyph = font8x8_basic[ascii[i] & 0x7f];
        int32_t gx = x + (int32_t)(i * DRV_GLYPH_W);
        uint32_t row, col;
        int32_t status;

        if (gx + DRV_GLYPH_W <= 0 || gx >= (int32_t)win->w) continue; /* clipped column */

        for (row = 0; row < DRV_GLYPH_H; row++)
        {
            uint8_t bits = (uint8_t)glyph[row];
            for (col = 0; col < DRV_GLYPH_W; col++)
                put_pixel(cell + row * DRV_GLYPH_W * 4, col, (bits >> col) & 1 ? fg : bg);
        }
        status = wineweb_drv_surface_write(hwnd, gx, y, DRV_GLYPH_W, DRV_GLYPH_H, cell);
        if (status != 0) return status;
    }
    return 0;
}
