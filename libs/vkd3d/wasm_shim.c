/*
 * wineweb (fd3dc): the bundled vkd3d compiles against wine's msvcrt
 * headers, whose UCRT routes name CRT helpers the static wine archives
 * do not define as C symbols (they are registry-loud or guarded out of
 * the wasm file.c/math.c bodies). This shim carries the honest backs
 * those references need, so the product link resolves instead of
 * silently importing them (the --allow-undefined env-import trap the
 * smoke caught: the vkd3d refs became `env.*` imports and the module
 * failed instantiation). Per arm, what it claims and why:
 *   - _dclass/_fdclass: wine's own FP_* constants (include/msvcrt/math.h
 *     282-286) via __builtin_fpclassify — real classification, the
 *     hlsl constant-folding callers compare against those.
 *   - _dsign/_fdsign: real signbit.
 *   - _strtof_l: real strtof — the C locale is the only locale on this
 *     runtime (the locale argument is ignored); REACHED on the compile
 *     path (the shader float literals). strtof itself is DEFINED here:
 *     wine's msvcrt strtof lives in the #if _MSVCR_VER>=120 block of
 *     string.c, compiled out at _MSVCR_VER=0 — one honest definition
 *     (float, the double→float narrowing of wine's own _strtof_l)
 *     serves both names. The F5probe diagnosis: the earlier
 *     `extern double strtof` left strtof UNDEFINED in the product link
 *     — the linker resolved it to a trapping signature_mismatch stub
 *     (the FFileno record's suspect, confirmed).
 *   - __stdio_common_vfprintf/vsprintf: the stdio WRITE seam —
 *     REACHED ON THE COMPILE PATH: vkd3d's string-buffer printf is the
 *     preproc's token-emission route (preproc.yy.c:3170
 *     vkd3d_string_buffer_printf(&ctx->buffer, "%s ", text)) and the
 *     hlsl message context rides it too; the earlier succeed-and-
 *     discard arm emptied the preprocessed source (the guest's clean
 *     E_FAIL — the F5probe diagnosis, wasm32-probe-verified) — the arm
 *     now formats for real. vfprintf stays the honest stderr no-op
 *     (the guest has no stderr); the fd3dc census's "flag-gated off"
 *     claim was wrong for the vsprintf arm, right for vfprintf.
 *   - _fileno: SUPERSEDED (ffileno) — the msvcrt file.c __wasm__ arm now
 *     defines it (the same link resolves the vkd3d routes through it); the
 *     shim's own copy was a duplicate strong symbol at the wasm-ld stage.
 *   - _fstat64i32: no file metadata exists on this runtime — zeroed buf,
 *     -1 (the callers check the rc; unreached without #include).
 *   - GetCurrentThreadId: the single guest thread's constant (the
 *     callers are the log prefix and cache keys — the honest shape for
 *     a runtime with one guest thread).
 * wasm-only: the PE flavor keeps its import-lib route and never
 * compiles this TU.
 */
#ifdef __wasm__

#include <stddef.h>
#include <stdarg.h>

/* the real formatter (wasi-libc's; the wine stdio.h route would recurse
 * into this very seam) */
extern int vsnprintf(char *buffer, size_t buffer_count, const char *format, va_list args);

/* wine's msvcrt FILE (the file.c __wasm__ arm's shape, verbatim). */
typedef struct _iobuf
{
  char* _ptr;
  char* _base;
  int   _cnt;
  int   _flag;
  int   _file;
  int   _charbuf;
  int   _bufsiz;
  char* _tmpfname;
} FILE;

/* wine's include/msvcrt/math.h 282-286 values. */
short __cdecl _dclass(double x)
{
    return __builtin_fpclassify(2 /*FP_NAN*/, 1 /*FP_INFINITE*/, -1 /*FP_NORMAL*/,
                                -2 /*FP_SUBNORMAL*/, 0 /*FP_ZERO*/, x);
}

short __cdecl _fdclass(float x)
{
    return __builtin_fpclassify(2, 1, -1, -2, 0, x);
}

int __cdecl _dsign(double x)
{
    return __builtin_signbit(x) ? 1 : 0;
}

int __cdecl _fdsign(float x)
{
    return __builtin_signbit(x) ? 1 : 0;
}

/* wine's msvcrt strtof: the #if _MSVCR_VER>=120 block of string.c is
 * compiled out at _MSVCR_VER=0, so the static archives define no
 * strtof — one honest definition here serves the _strtof_l arm and any
 * other CRT route (float contract; the double→float narrowing is
 * wine's own _strtof_l shape). */
extern double strtod(const char *nptr, char **endptr);
float __cdecl strtof(const char *nptr, char **endptr)
{
    return (float)strtod(nptr, endptr);
}

float __cdecl _strtof_l(const char *nptr, char **endptr, void *locale)
{
    (void)locale; /* the C locale is the only locale on this runtime */
    return strtof(nptr, endptr);
}

int __cdecl __stdio_common_vfprintf(unsigned __int64 options, FILE *stream,
        const char *format, void *locale, void *args)
{
    (void)options; (void)stream; (void)format; (void)locale; (void)args;
    return 0; /* the stdio WRITE seam: the call succeeds and discards */
}

int __cdecl __stdio_common_vsprintf(unsigned __int64 options, char *buffer,
        size_t buffer_count, const char *format, void *locale, void *args)
{
    int ret;
    (void)options; (void)locale;
    if (!buffer || !buffer_count)
        return 0;
    /* REAL formatting: the preproc's token emission and the hlsl
     * message context ride this (the F5probe diagnosis). vsnprintf is
     * the wasi-libc one — same varargs handoff the callers use. */
    ret = vsnprintf(buffer, buffer_count, format, (va_list)args);
    return ret < 0 ? 0 : ret;
}

int __cdecl _fstat64i32(int fd, void *buf)
{
    (void)fd; (void)buf;
    return -1; /* no file metadata on this runtime — the callers check the rc */
}

unsigned long __stdcall GetCurrentThreadId(void)
{
    return 1; /* the single guest thread */
}

#endif /* __wasm__ */
