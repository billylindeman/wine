/*
 * wineweb_driver.c — the wineweb.drv wasm driver adapter (P3.8, the notepad
 * capstone; docs/spikes/p3-notepad-demo.md T2).
 *
 * What owns this: the P3.8 block. What it must never do: re-implement GDI or
 * USER semantics (the DIB engine and the window code stay win32u's — this
 * file only bridges the `struct user_driver_funcs` contract to the
 * `wineweb_drv` C ABI v1 host half), allocate guest memory outside the
 * surface DIBs win32u owns, or block (present is an explicit frame push).
 *
 * Scope (the notepad surface): window create/destroy/text/show, the window
 * surface (32bpp top-down DIB the DIB engine draws into, flushed to the host
 * compositor as RGBA sub-rects), and the font8x8 bitmap font backend
 * (wineweb_font_backend). Everything else is a loud stub — named, logged
 * once, and returning the failure value; the spike doc's residue table lists
 * the ones the T-tier paths actually reach.
 *
 * Byte-order note: the DIB engine's 32bpp format is 0x00RRGGBB in a little-
 * endian DWORD (bytes B,G,R,0); the compositor is RGBA8888. The flush pass
 * swaps channels and forces alpha 255 over the dirty rect — the one place
 * the two formats meet.
 */

#pragma makedep unix

#pragma makedep unix

#include "win32u_private.h"
#include "ntgdi_private.h"
#include "ntuser_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wineweb);

/* --- the wineweb_drv C ABI v1 (resolved at the final-module link) --------- */

extern int32_t wineweb_drv_create_window(int32_t x, int32_t y, uint32_t w, uint32_t h,
                                         const uint8_t *title, size_t title_len,
                                         uint64_t *out_hwnd);
extern int32_t wineweb_drv_destroy_window(uint64_t hwnd);
extern int32_t wineweb_drv_show_window(uint64_t hwnd, int32_t visible);
extern int32_t wineweb_drv_set_text(uint64_t hwnd, const uint8_t *text, size_t text_len);
extern int32_t wineweb_drv_surface_write(uint64_t hwnd, int32_t x, int32_t y, uint32_t w,
                                         uint32_t h, const uint8_t *pixels);
extern int32_t wineweb_drv_present(void);

/* --- guest-hwnd -> drv-handle mapping ------------------------------------- */

#define WWEB_MAX_WINDOWS 32 /* the dialog's 8 controls + the popups ride the demo's census (S11) */
static struct { HWND hwnd; uint64_t drv; struct window_surface *surface; uint32_t w, h; } wweb_windows[WWEB_MAX_WINDOWS];

static BOOL wweb_map(HWND hwnd, uint64_t *drv)
{
    unsigned i;
    for (i = 0; i < WWEB_MAX_WINDOWS; i++)
        if (wweb_windows[i].hwnd == hwnd) { *drv = wweb_windows[i].drv; return TRUE; }
    return FALSE;
}

static BOOL wweb_add(HWND hwnd, uint64_t drv, UINT w, UINT h)
{
    unsigned i;
    for (i = 0; i < WWEB_MAX_WINDOWS; i++)
        if (!wweb_windows[i].hwnd) { wweb_windows[i].hwnd = hwnd; wweb_windows[i].drv = drv; wweb_windows[i].w = w; wweb_windows[i].h = h; return TRUE; }
    return FALSE;
}

static void wweb_remove(HWND hwnd)
{
    unsigned i;
    for (i = 0; i < WWEB_MAX_WINDOWS; i++)
        if (wweb_windows[i].hwnd == hwnd) { wweb_windows[i].hwnd = 0; wweb_windows[i].drv = 0; wweb_windows[i].surface = NULL; wweb_windows[i].w = 0; wweb_windows[i].h = 0; }
}

/* P4.14: the D3D9 present path needs the window's own surface — recorded by
 * the WindowPosChanged driver entry (win32u's delivery of the surface). */
static void wweb_surface_note( HWND hwnd, struct window_surface *surface )
{
    unsigned i;
    for (i = 0; i < WWEB_MAX_WINDOWS; i++)
        if (wweb_windows[i].hwnd == hwnd) wweb_windows[i].surface = surface;
}

static struct window_surface *wineweb_d3d9_window_surface( HWND hwnd )
{
    unsigned i;
    for (i = 0; i < WWEB_MAX_WINDOWS; i++)
        if (wweb_windows[i].hwnd == hwnd && wweb_windows[i].surface &&
            wweb_windows[i].surface != &dummy_surface)
            return wweb_windows[i].surface;
    return NULL;
}

/* --- the compositor canvas size (P4.15b) -----------------------------------
 *
 * The display-device family's single entry is the compositor canvas
 * (sysparams.c's wweb_lock_display_devices queries this under __wasm__).
 * The size comes from the host half (wineweb_drv_screen_size, exported by
 * the harness drvshim next to the other wineweb_drv_* entries) — the
 * presentation half's geometry, never window semantics. The symbol resolves
 * at the final-module link (only the p38/p414 harnesses link this archive);
 * a harness that does not export it fails the link loudly instead of
 * silently mismatching the compositor. Returns 0 on success; a non-zero
 * return means the host half did not answer (the display lock then fails —
 * the honest refusal, never a 0x0 monitor). */
int wineweb_screen_size( unsigned int *w, unsigned int *h )
{
    extern int32_t wineweb_drv_screen_size(uint32_t *w, uint32_t *h);
    uint32_t hw = 0, hh = 0;

    *w = 0;
    *h = 0;
    if (wineweb_drv_screen_size( &hw, &hh ) != 0) return -1;
    /* The sane-geometry clamp: the petabyte-DIB shape (a garbage rect riding
     * into a surface allocation) cannot come from the compositor's own size.
     * 32768 is the same ceiling the DIB surface guard uses. */
    if (!hw || !hh || hw > 0x8000 || hh > 0x8000) return -1;
    *w = hw;
    *h = hh;
    return 0;
}

/* --- the window surface ---------------------------------------------------- */

struct wweb_surface
{
    struct window_surface header;
    HWND hwnd;         /* the guest window — the drv handle resolves at flush */
    uint64_t drv;      /* the host half's handle */
    uint32_t stride;   /* bytes per row of the DIB */
};

static void wweb_surface_set_clip( struct window_surface *surface, const RECT *rects, UINT count );
static BOOL wweb_surface_flush( struct window_surface *surface, const RECT *rect,
                                const RECT *dirty, const BITMAPINFO *color_info,
                                const void *color_bits, BOOL shape_changed,
                                const BITMAPINFO *shape_info, const void *shape_bits );
static void wweb_surface_destroy( struct window_surface *surface );

static struct window_surface_funcs wweb_surface_funcs = {
    .size = sizeof(struct wweb_surface),
    .set_clip = wweb_surface_set_clip,
    .flush = wweb_surface_flush,
    .destroy = wweb_surface_destroy,
};

static void wweb_surface_set_clip( struct window_surface *surface, const RECT *rects, UINT count )
{
    /* the compositor clips against the window rect itself; no host clip list */
}

static BOOL wweb_surface_flush( struct window_surface *surface, const RECT *rect,
                                const RECT *dirty, const BITMAPINFO *color_info,
                                const void *color_bits, BOOL shape_changed,
                                const BITMAPINFO *shape_info, const void *shape_bits )
{
    struct wweb_surface *s = (struct wweb_surface *)surface;
    UINT surface_w = surface->rect.right - surface->rect.left;
    LONG x, y, x_end, y_end;
    uint8_t *dst;
    uint32_t len;

#ifdef __wasm__ /* wineweb: WWFL probe */
    ERR( "WWFL: flush surf %dx%d dirty %s bounds %s\n", surface_w,
         surface->rect.bottom - surface->rect.top, wine_dbgstr_rect( dirty ), wine_dbgstr_rect( &surface->bounds ) );
#endif

    /* The drv handle resolves here, not at surface creation: the create walk
     * builds the window surface BEFORE WM_NCCREATE/WM_CREATE/pCreateWindow
     * (window.c's initial apply_window_pos), so the drv window does not exist
     * yet when the adapter is asked for the surface. Returning FALSE keeps
     * the dirty bounds — the next flush (the first is the GetMessageW wait
     * path's flush_window_surfaces, long after the create walk completed)
     * publishes the initial content. */
    if (!wweb_map( s->hwnd, &s->drv )) return FALSE;
    if (dirty->left < 0 || dirty->top < 0 || surface_w == 0) return FALSE;
    x_end = min( dirty->right, surface->rect.right );
    y_end = min( dirty->bottom, surface->rect.bottom );
    /* The surface rect is rounded up to a 128 multiple (get_surface_rect's
     * resize-avoidance) while the drv window is the exact window rect: clip
     * the flush to what the drv surface can hold (wineweb_drv_surface_write
     * rejects sub-rects beyond it). */
    {
        unsigned i;
        for (i = 0; i < WWEB_MAX_WINDOWS; i++)
            if (wweb_windows[i].hwnd == s->hwnd)
            {
                if (x_end > (LONG)wweb_windows[i].w) x_end = (LONG)wweb_windows[i].w;
                if (y_end > (LONG)wweb_windows[i].h) y_end = (LONG)wweb_windows[i].h;
                break;
            }
    }
    if (x_end <= dirty->left || y_end <= dirty->top) return TRUE; /* nothing visible */
    len = (uint32_t)(x_end - dirty->left) * 4;
    if (!len || !(dst = malloc( len ))) return FALSE;

    for (y = dirty->top; y < y_end; y++)
    {
        const uint8_t *src = (const uint8_t *)color_bits + (size_t)y * s->stride
                             + (size_t)dirty->left * 4;
        for (x = dirty->left; x < x_end; x++)
        {
            dst[(x - dirty->left) * 4 + 0] = src[(x - dirty->left) * 4 + 2]; /* R */
            dst[(x - dirty->left) * 4 + 1] = src[(x - dirty->left) * 4 + 1]; /* G */
            dst[(x - dirty->left) * 4 + 2] = src[(x - dirty->left) * 4 + 0]; /* B */
            dst[(x - dirty->left) * 4 + 3] = 0xff;                           /* A */
        }
        wineweb_drv_surface_write( s->drv, dirty->left, y, x_end - dirty->left, 1, dst );
    }
    free( dst );
    wineweb_drv_present();
    return TRUE;
}

static void wweb_surface_destroy( struct window_surface *surface )
{
}

static BOOL wweb_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *rect,
                                      struct window_surface **surface )
{
    struct
    {
        BITMAPINFO info;
        RGBQUAD colors[255];
    } bmi;
    struct window_surface *surf;
    struct wweb_surface *ws;
    UINT width = rect->right - rect->left, height = rect->bottom - rect->top;

    /* The sane-geometry guard (P4.15b): a garbage rect (the DC-family
     * failure shape — a 0x3440xxxx-wide rect rode into a petabyte DIB and
     * exhausted the host pool) is refused here, loudly, instead of being
     * allocated. 32768 is the same ceiling wineweb_screen_size clamps the
     * compositor canvas to. */
    if (!width || !height || width > 0x8000 || height > 0x8000)
    {
        static int once;
        if (!once++) WARN( "wineweb.drv: refusing absurd surface rect %dx%d\n", width, height );
        return FALSE;
    }

    memset( &bmi, 0, sizeof(bmi) );
    bmi.info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.info.bmiHeader.biWidth       = width;
    bmi.info.bmiHeader.biHeight      = -(LONG)height; /* top-down */
    bmi.info.bmiHeader.biPlanes      = 1;
    bmi.info.bmiHeader.biBitCount    = 32;
    bmi.info.bmiHeader.biCompression = BI_RGB;
    bmi.info.bmiHeader.biSizeImage   = (DWORD)width * height * 4;

    if (!(surf = window_surface_create( &wweb_surface_funcs, hwnd, rect, &bmi.info, 0 ))) return FALSE;
    ws = (struct wweb_surface *)surf;
    ws->hwnd = hwnd;
    /* The drv handle resolves at flush (the surface is built before
     * pCreateWindow on the create walk); take it now when it exists. */
    wweb_map( hwnd, &ws->drv );
    ws->stride = width * 4;
    /* The initial DIB content is the window's own state (win32u's 0xff init =
     * the COLOR_WINDOW+1 white the class brush would paint): mark it dirty so
     * the first flush publishes it — the T2 initial paint rides win32u's own
     * flush machinery (flush_window_surfaces on the GetMessageW wait path),
     * no fabricated pixels. */
    window_surface_lock( surf );
    surf->bounds = surf->rect;
    window_surface_unlock( surf );
    *surface = surf;
    return TRUE;
}

/* --- the user_driver_funcs vtable ------------------------------------------ */

static BOOL wweb_create_drv_window( HWND hwnd )
{
    struct window_rects rects;
    RECT rect;
    uint64_t drv = 0;
    uint8_t title_utf8[128];
    /* The guest title is UTF-16; the compositor's titles are bytes. Notepad's
     * title is ASCII ("Notepad") — transcode the ASCII subset, loud-fail the
     * rest (the spike doc names the gap; P4 widens it). */
    WCHAR buf[128];
    int i, len;

    /* Idempotent (the x11 create_win_data shape): the yield-slice recovery
     * re-runs a create walk that was unwound mid-flight, and the walk's
     * pCreateWindow tail then double-maps the hwnd — each duplicate burned a
     * host-map slot forever (the S11 dialog wall: the 16-slot map filled with
     * the frame/edit/desktop dups before the dialog's fifth control, and the
     * control create failed ERROR_INVALID_INDEX). */
    if (wweb_map( hwnd, &drv )) return TRUE;

    /* The screen space: the drv window's x/y are the compositor's placement
     * (the screen). COORDS_CLIENT expresses the window rect relative to the
     * window's own client origin — the outer rect came out (-4,-60) and the
     * whole nonclient area hung off-screen (the P3.8 nonclient tranche). */
    if (!get_window_rects( hwnd, COORDS_SCREEN, &rects, get_thread_dpi() )) return FALSE;
    rect = rects.window;
    len = NtUserInternalGetWindowText( hwnd, buf, 128 );
    for (i = 0; i < len && i < 127; i++)
        title_utf8[i] = (buf[i] < 0x80) ? (uint8_t)buf[i] : '?';
    title_utf8[i] = 0;

    if (wineweb_drv_create_window( rect.left, rect.top, rect.right - rect.left,
                                   rect.bottom - rect.top, title_utf8, i, &drv ) != 0) return FALSE;
    if (!wweb_add( hwnd, drv, rect.right - rect.left, rect.bottom - rect.top ))
        { wineweb_drv_destroy_window( drv ); return FALSE; }
    /* The create-time visibility truth: pCreateWindow is the LAST driver call
     * of the create walk, and a window created with WS_VISIBLE never takes
     * another pos walk before the message loop (show_window's SW_SHOW case
     * `goto done`s on was_visible, so pShowWindow never fires — the p38 T2
     * wall: the notepad frame sat vis=0 in the drv table). win32u's own style
     * state decides; later flips ride pWindowPosChanged below. */
    if (NtUserGetWindowLongW( hwnd, GWL_STYLE ) & WS_VISIBLE)
        wineweb_drv_show_window( drv, 1 );
    return TRUE;
}

static BOOL wweb_CreateWindow( HWND hwnd )
{
    struct window_rects rects;
    RECT rect;

    if (!get_window_rects( hwnd, COORDS_SCREEN, &rects, get_thread_dpi() )) return FALSE;
    rect = rects.window;
    /* A 0-size create has no host object yet — the x11 shape: X11DRV_CreateWindow
     * returns TRUE unconditionally and the X window materializes on the first
     * pos walk (create_win_data). The menu popup is created exactly so
     * (init_popup's `0, 0, 0, 0` — menu.c:3356) and is sized by show_popup's
     * SetWindowPos (menu.c:3130); failing the create here (the S4 wall) killed
     * init_popup before any paint. The first wweb_WindowPosChanged with a real
     * rect creates the host object; the flush resolves the handle then. */
    if (rect.right == rect.left || rect.bottom == rect.top) return TRUE;
    return wweb_create_drv_window( hwnd );
}

static void wweb_DestroyWindow( HWND hwnd )
{
    uint64_t drv;
    if (wweb_map( hwnd, &drv )) { wineweb_drv_destroy_window( drv ); wweb_remove( hwnd ); }
}

/* The desktop's rects are staged here (the x11drv shape — X11DRV_SetDesktop-
 * Window set_window_pos's the virtual screen when the rects are
 * uninitialized): the guest's boot calls pSetDesktopWindow once the desktop
 * exists, and without it every desktop-level point walk gates out on the
 * zero rects (the S4 popup probe: get_window_children_from_point on the
 * desktop — the #32768 popup is a desktop CHILD owned by the frame, so the
 * frame-level walk never sees it). */
static void wweb_SetDesktopWindow( HWND hwnd )
{
    unsigned int w = 0, h = 0;
    int st = wineweb_screen_size( &w, &h );
    if (st != 0) return;
    /* The raw server request (the x11drv shape — NtUserSetWindowPos would
     * route a detached desktop window to send_message(WM_WINE_SETWINDOWPOS),
     * refused: the desktop has no owning thread). */
    SERVER_START_REQ( set_window_pos )
    {
        RECT rect = { 0, 0, (LONG)w, (LONG)h };
        req->handle    = wine_server_user_handle( hwnd );
        req->previous  = 0;
        req->swp_flags = SWP_NOZORDER;
        req->window    = wine_server_rectangle( rect );
        req->client    = req->window;
        wine_server_call( req );
    }
    SERVER_END_REQ;
}

static void wweb_SetWindowText( HWND hwnd, LPCWSTR text )
{
    uint64_t drv;
    uint8_t title_utf8[128];
    int i, len = min( lstrlenW( text ), 127 );
    for (i = 0; i < len; i++)
        title_utf8[i] = (text[i] < 0x80) ? (uint8_t)text[i] : '?';
    title_utf8[i] = 0;
    if (wweb_map( hwnd, &drv )) wineweb_drv_set_text( drv, title_utf8, i );
}

static UINT wweb_ShowWindow( HWND hwnd, INT cmd, RECT *rect, UINT swp )
{
    uint64_t drv;
    BOOL visible = (cmd != SW_HIDE) && (NtUserGetWindowLongW( hwnd, GWL_STYLE ) & WS_VISIBLE);
    if (wweb_map( hwnd, &drv )) wineweb_drv_show_window( drv, visible ? 1 : 0 );
    return 0; /* no extra SetWindowPos flags; the walk continues client-side */
}

/* --- loud stubs ------------------------------------------------------------ */

#define LOUD_STUB(ret, name, args, args_call, def) \
    static ret nodrv_##name args \
    { \
        static int once; \
        if (!once++) WARN( "wineweb.drv: %s reached (loud stub)\n", #name ); \
        return def; \
    }

LOUD_STUB( BOOL,    ProcessEvents, (DWORD flags), (flags), FALSE )
LOUD_STUB( LRESULT, WindowMessage, (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp), (hwnd, msg, wp, lp), 0 )
LOUD_STUB( void,    SetDesktopWindow, (HWND hwnd), (hwnd), )
LOUD_STUB( void,    SetWindowStyle, (HWND hwnd, INT offset, STYLESTRUCT *style), (hwnd, offset, style), )
LOUD_STUB( BOOL,    SystemParametersInfo, (UINT action, UINT param, void *data, UINT flags), (action, param, data, flags), FALSE )
LOUD_STUB( BOOL,    GetCursorPos, (POINT *pt), (pt), FALSE )
LOUD_STUB( BOOL,    SetCursorPos, (INT x, INT y), (x, y), FALSE )
LOUD_STUB( void,    SetCursor, (HWND hwnd, HCURSOR handle), (hwnd, handle), )
LOUD_STUB( void,    Beep, (void), (), )
LOUD_STUB( void,    ThreadDetach, (void), (), )
LOUD_STUB( BOOL,    CreateDesktop, (const WCHAR *name, UINT w, UINT h), (name, w, h), FALSE )
LOUD_STUB( BOOL,    GetWindowStyleMasks, (HWND hwnd, UINT style, UINT ex_style, UINT *style_mask, UINT *ex_style_mask), (hwnd, style, ex_style, style_mask, ex_style_mask), FALSE )
/* The pos-walk surface gate (get_window_surface's `needs_surface`): win32u
 * asks the driver to accept the position walk before building the window
 * surface. The compositor clips against the window rect itself (the
 * wineweb_surface set_clip is a no-op) — accept, no rect edits. The loud
 * stub's FALSE refused every surface, so no window surface ever existed and
 * wweb_surface_flush never ran (the p38 T2 wall 2). */
static BOOL wweb_WindowPosChanging( HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects )
{
    return TRUE;
}

/* P4.14: record the window's surface for the D3D9 present path (the loud
 * stub it replaced carried no state). Everything else about the position
 * walk stays client-side, as before.
 *
 * Plus the visibility route (the p38 T2 wall): a driver learns map/unmap
 * from the pos walk — the X11 shape (X11DRV_WindowPosChanged reads the style
 * and maps). pShowWindow only fires on the min/max placement path (show_window
 * calls it solely when window_min_maximize filled newPos), so the plain
 * show/hide rides here: win32u's own WS_VISIBLE state decides, idempotently. */
static void wweb_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner, UINT swp_flags,
                                   const struct window_rects *rects, struct window_surface *surface )
{
    uint64_t drv;
    struct window_rects cur;
    BOOL visible = (NtUserGetWindowLongW( hwnd, GWL_STYLE ) & WS_VISIBLE) != 0;

    if (surface && surface != &dummy_surface) wweb_surface_note( hwnd, surface );
    /* The desktop never gets a host object: its rects are staged by
     * wweb_SetDesktopWindow and its "pixels" are the compositor screen
     * itself (the drv model's backdrop), not a window. */
    if (is_desktop_window( hwnd )) return;
    /* The lazy create (x11's create_win_data-on-first-walk shape): a 0-size
     * create (the menu popup) skipped the host object; the first walk with a
     * real rect materializes it — the surface flush resolves the handle then
     * (the wweb_surface_flush contract). Map-less windows are rare (only the
     * deferred creates), so the probe rides behind the map check. */
    if (!wweb_map( hwnd, &drv ) && get_window_rects( hwnd, COORDS_SCREEN, &cur, get_thread_dpi() )
        && cur.window.right != cur.window.left && cur.window.bottom != cur.window.top)
        wweb_create_drv_window( hwnd );
    if (wweb_map( hwnd, &drv )) wineweb_drv_show_window( drv, visible ? 1 : 0 );
}

/* --- the dxwgpu↔drv present seam (P4.14, docs/spikes/p4-d3d9-attach.md) ----
 *
 * The p414 driver calls this after the GPU half presents: the D3D9 swapchain
 * frame (RGBA8888 top-down, w*h*4) is written into the window's own DIB and
 * flushed — the DIB is the one window truth, so a GDI overlay drawn after
 * (the composition-order leg) composes over the frame through the normal
 * flush path. Loud-unsupported when the hwnd was never surface-registered
 * through this adapter. */
int32_t wineweb_d3d9_present_frame( HWND hwnd, const uint8_t *rgba, UINT w, UINT h )
{
    struct window_surface *surface = wineweb_d3d9_window_surface( hwnd );
    RECT rect;

    if (!surface) return -1;
    rect.left = 0;
    rect.top = 0;
    rect.right = (LONG)w;
    rect.bottom = (LONG)h;
    return wineweb_surface_present_frame( surface, &rect, rgba ) ? 0 : -2;
}

static const struct user_driver_funcs wineweb_user_driver =
{
    .pCreateWindow = wweb_CreateWindow,
    .pDestroyWindow = wweb_DestroyWindow,
    .pCreateWindowSurface = wweb_CreateWindowSurface,
    .pSetWindowText = wweb_SetWindowText,
    .pShowWindow = wweb_ShowWindow,
    .pProcessEvents = nodrv_ProcessEvents,
    .pWindowMessage = nodrv_WindowMessage,
    .pSetDesktopWindow = wweb_SetDesktopWindow,
    .pSetWindowStyle = nodrv_SetWindowStyle,
    .pSystemParametersInfo = nodrv_SystemParametersInfo,
    .pGetCursorPos = nodrv_GetCursorPos,
    .pSetCursorPos = nodrv_SetCursorPos,
    .pSetCursor = nodrv_SetCursor,
    .pBeep = nodrv_Beep,
    .pThreadDetach = nodrv_ThreadDetach,
    .pCreateDesktop = nodrv_CreateDesktop,
    .pGetWindowStyleMasks = nodrv_GetWindowStyleMasks,
    .pWindowPosChanged = wweb_WindowPosChanged,
    .pWindowPosChanging = wweb_WindowPosChanging,
};

const struct user_driver_funcs *wineweb_user_driver_funcs(void)
{
    return &wineweb_user_driver;
}
