/*
 * wineweb_font.c — the wasm font backend (P3.8 nonclient tranche).
 *
 * What owns this: the P3.8 nonclient block. Wine's font machinery
 * (font.c) selects and renders glyphs through `struct font_backend_funcs`;
 * on wasm there is no freetype, so this provides the backend: a fixed 8x8
 * cell bitmap face (font8x8, public domain) registered under the family
 * name the default sysparams request — "Tahoma", the "MS Shell Dlg"
 * substitute wine itself falls back to. The guest's DrawTextW/TextOut then
 * rides wine's own text engine (metrics, clipping, DrawTextW layout) with
 * the bitmap backend beneath — no driver-side text hacks.
 *
 * What it must never do: allocate guest memory beyond the font objects
 * wine itself owns, or fake glyph data for codepoints the 8x8 table
 * doesn't carry (non-ASCII maps to '?', the bitmap-font .notdef shape).
 */

#pragma makedep unix

#include "ntgdi_private.h"

#include "../wineweb.drv/font8x8.h"

WINE_DEFAULT_DEBUG_CHANNEL(wineweb);

/* The 8x8 cell: 8 rows, 1bpp, DWORD-aligned stride (the GGO_BITMAP layout),
 * MSB-first. font8x8 stores one byte per row, LSB-first. */
#define WWEB_CELL_W 8
#define WWEB_CELL_H 8
#define WWEB_CELL_PITCH 4  /* ((8 + 31) >> 5) << 2 */

static const struct bitmap_font_size wweb_font_size =
{
    .width  = WWEB_CELL_W,
    .height = WWEB_CELL_H,
    .size   = WWEB_CELL_H,
    .x_ppem = WWEB_CELL_W,
    .y_ppem = WWEB_CELL_H,
    .internal_leading = 0,
};

struct wweb_font_data
{
    int cell_w, cell_h; /* the face metrics this gdi_font renders with */
};

static void wweb_load_fonts(void)
{
    static const WCHAR tahomaW[] = {'T','a','h','o','m','a',0};
    static const WCHAR regularW[] = {'R','e','g','u','l','a','r',0};
    static const WCHAR fullW[] = {'T','a','h','o','m','a',' ','(','8','x','8',')',0};
    FONTSIGNATURE fs;

    memset( &fs, 0, sizeof(fs) );
    fs.fsCsb[0] = FS_LATIN1;
    if (!add_gdi_face( tahomaW, NULL, regularW, fullW, NULL, NULL, 0, 0, fs,
                       NTM_REGULAR, FW_NORMAL, 0x00010000, 0, &wweb_font_size ))
        ERR( "wineweb: the 8x8 font face failed to register\n" );
}

static BOOL wweb_load_font( struct gdi_font *font )
{
    struct wweb_font_data *data;

    if (!(data = calloc( 1, sizeof(*data) ))) return FALSE;
    data->cell_w = wweb_font_size.width;
    data->cell_h = wweb_font_size.height;
    font->private = data;
    font->ppem = wweb_font_size.y_ppem;
    return TRUE;
}

static void wweb_destroy_font( struct gdi_font *font )
{
    free( font->private );
    font->private = NULL;
}

static BOOL wweb_set_bitmap_text_metrics( struct gdi_font *font )
{
    TEXTMETRICW *tm;

    if (font->otm.otmSize) return TRUE;  /* already set */
    font->otm.otmSize = offsetof( OUTLINETEXTMETRICW, otmFiller );

    tm = &font->otm.otmTextMetrics;
    tm->tmHeight           = WWEB_CELL_H;
    tm->tmAscent           = 7;              /* the glyph rows sit above the baseline row */
    tm->tmDescent          = 1;
    tm->tmInternalLeading  = 0;
    tm->tmExternalLeading  = 0;
    tm->tmAveCharWidth     = WWEB_CELL_W;
    tm->tmMaxCharWidth     = WWEB_CELL_W;
    tm->tmWeight           = FW_NORMAL;
    tm->tmOverhang         = 0;
    tm->tmDigitizedAspectX = 96;
    tm->tmDigitizedAspectY = 96;
    tm->tmFirstChar        = 0;
    tm->tmLastChar         = 127;            /* the ASCII subset the table carries */
    tm->tmDefaultChar      = '?';
    tm->tmBreakChar        = ' ';
    tm->tmItalic           = 0;
    tm->tmUnderlined       = font->lf.lfUnderline ? 0xff : 0;
    tm->tmStruckOut        = font->lf.lfStrikeOut ? 0xff : 0;
    /* The 8x8 cell is fixed pitch; the low bits are 0 exactly like
     * freetype's fixed-width faces (TMPF_FIXED_PITCH is set iff variable). */
    tm->tmPitchAndFamily   = FF_MODERN;
    tm->tmCharSet          = font->charset;
    return TRUE;
}

/* The ASCII subset of the table, '0'-'9' packed MSB-first into DWORD rows. */
static void wweb_pack_glyph( UINT ch, BYTE *buf )
{
    const char *rows = font8x8_basic[ch & 0x7f];
    unsigned row, col;

    memset( buf, 0, WWEB_CELL_PITCH * WWEB_CELL_H );
    for (row = 0; row < WWEB_CELL_H; row++)
        for (col = 0; col < WWEB_CELL_W; col++)
            if ((rows[row] >> col) & 1)
                buf[row * WWEB_CELL_PITCH + col / 8] |= 0x80 >> (col % 8);
}

static UINT wweb_get_glyph_outline( struct gdi_font *font, UINT glyph, UINT format,
                                    GLYPHMETRICS *lpgm, ABC *abc, UINT buflen, void *buf,
                                    const MAT2 *lpmat, BOOL tategaki )
{
    UINT needed = WWEB_CELL_PITCH * WWEB_CELL_H;

    if (format != GGO_METRICS && format != GGO_BITMAP)
    {
        static int once;
        if (!once++) WARN( "wineweb: glyph format %08x loud-unsupported (want GGO_METRICS/GGO_BITMAP)\n", format );
        return GDI_ERROR;
    }

    /* The cell: every glyph occupies the full 8x8 box, ink or not. */
    lpgm->gmBlackBoxX        = WWEB_CELL_W;
    lpgm->gmBlackBoxY        = WWEB_CELL_H;
    lpgm->gmptGlyphOrigin.x  = 0;
    lpgm->gmptGlyphOrigin.y  = 7;   /* the bitmap's top row sits ascent-1 above the origin */
    lpgm->gmCellIncX         = WWEB_CELL_W;
    lpgm->gmCellIncY         = 0;
    abc->abcA = 0;
    abc->abcB = WWEB_CELL_W;
    abc->abcC = 0;

    if (format == GGO_METRICS) return 1;
    if (!buf || !buflen) return needed;
    if (needed > buflen) return GDI_ERROR;

    wweb_pack_glyph( glyph, buf );
    return needed;
}

static UINT wweb_get_aa_flags( struct gdi_font *font, UINT aa_flags, BOOL antialias_fakes )
{
    return GGO_BITMAP;  /* monochrome cells: no antialiasing surface exists */
}

static BOOL wweb_get_glyph_index( struct gdi_font *font, UINT *glyph, BOOL use_encoding )
{
    /* The encoding IS the glyph index (the ASCII subset); anything outside
     * it maps to the .notdef shape. */
    if (*glyph > 0x7f) *glyph = '?';
    return TRUE;
}

static UINT wweb_get_default_glyph( struct gdi_font *font )
{
    return '?';
}

static UINT wweb_get_font_data( struct gdi_font *font, UINT table, UINT offset,
                                void *buf, UINT count )
{
    return 0;  /* no font tables: the engine's optional lookups fail honestly */
}

static BOOL wweb_get_char_width_info( struct gdi_font *font, struct char_width_info *info )
{
    return FALSE;  /* the engine's callers fall back to the abc metrics */
}

static UINT wweb_get_kerning_pairs( struct gdi_font *font, KERNINGPAIR **pair )
{
    if (pair) *pair = NULL;
    return 0;
}

static UINT wweb_get_unicode_ranges( struct gdi_font *font, GLYPHSET *glyphset )
{
    return 0;  /* an empty range list: the engine fills cbThis/cRanges itself */
}

static BOOL wweb_set_outline_text_metrics( struct gdi_font *font )
{
    return FALSE;  /* a bitmap face: the engine falls back to the cell metrics */
}

static BOOL wweb_enum_family_fallbacks( UINT pitch_and_family, int index, WCHAR buffer[LF_FACESIZE] )
{
    return FALSE;  /* no fallback families beyond the registered one */
}

static const struct font_backend_funcs wineweb_font_backend =
{
    wweb_load_fonts,
    wweb_enum_family_fallbacks,
    NULL,                       /* add_font: no file-based faces on wasm */
    NULL,                       /* add_mem_font: the 8x8 cell needs none */
    wweb_load_font,
    wweb_get_font_data,
    wweb_get_aa_flags,
    wweb_get_glyph_index,
    wweb_get_default_glyph,
    wweb_get_glyph_outline,
    wweb_get_unicode_ranges,
    wweb_get_char_width_info,
    wweb_set_outline_text_metrics,
    wweb_set_bitmap_text_metrics,
    wweb_get_kerning_pairs,
    wweb_destroy_font,
};

const struct font_backend_funcs *init_wineweb_font_lib(void)
{
    return &wineweb_font_backend;
}
