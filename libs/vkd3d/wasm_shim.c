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
 *     path (the shader float literals).
 *   - __stdio_common_vfprintf/vsprintf: the stdio WRITE seam (the
 *     file.c loud-family contract, ABI-shaped for the UCRT route) —
 *     the vkd3d debug/TRACE callers are flag-gated off on this runtime
 *     (VKD3D_DEBUG unset, served GetEnvironmentVariableA answers 0);
 *     the call succeeds and discards.
 *   - _fileno: the FILE's own _file (the sink slot, the file.c contract).
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

extern double strtof(const char *nptr, char **endptr);
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
    (void)options; (void)format; (void)locale; (void)args;
    if (buffer && buffer_count)
        buffer[0] = 0; /* the wrappers' own termination contract sees "" */
    return 0;
}

int __cdecl _fileno(FILE *file)
{
    return file ? file->_file : -1;
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
