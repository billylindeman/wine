/*
 * wineweb (fd3dc): the wasm staticlib flavor has no import libs — the
 * vkd3d_D3D* names d3dcompiler's compiler.c declares ride wined3d's spec
 * forwarder in the PE build (dlls/wined3d/wined3d.spec:
 * "@ stdcall vkd3d_D3DCompile2VKD3D(...) D3DCompile2VKD3D"). The bundled
 * vkd3d-utils exports the same functions under their plain names; this
 * shim forwards the three names the staticlib flavor leaves undefined,
 * so the product link resolves. wasm-only: the PE flavor keeps its
 * import-lib route and never compiles this TU.
 */
#ifdef __wasm__

#include <vkd3d_utils.h>

/* The asmscanner (asmshader.yy.o) references stderr/stdin — wine's
 * corecrt_wstdio.h hops those through __acrt_iob_func. The fork's
 * msvcrt file.c __wasm__ arm owns the streams (the static zeroed
 * FILE[3]); forward the hop, with no iob beyond the standard three
 * (file.c's own contract). The _iobuf shape is file.c's, verbatim. */
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

extern FILE *__iob_func(void);

FILE *__cdecl __acrt_iob_func(unsigned index)
{
    if (index >= 3) return NULL;
    return &__iob_func()[index];
}

HRESULT WINAPI vkd3d_D3DPreprocess(const void *data, SIZE_T size, const char *filename,
        const D3D_SHADER_MACRO *defines, ID3DInclude *include,
        ID3DBlob **shader, ID3DBlob **error_messages)
{
    return D3DPreprocess(data, size, filename, defines, include, shader, error_messages);
}

HRESULT WINAPI vkd3d_D3DCompile2VKD3D(const void *data, SIZE_T data_size, const char *filename,
        const D3D_SHADER_MACRO *defines, ID3DInclude *include, const char *entrypoint,
        const char *target, UINT flags, UINT effect_flags, UINT secondary_flags,
        const void *secondary_data, SIZE_T secondary_data_size, ID3DBlob **shader,
        ID3DBlob **error_messages, unsigned int compiler_version)
{
    return D3DCompile2VKD3D(data, data_size, filename, defines, include, entrypoint, target,
            flags, effect_flags, secondary_flags, secondary_data, secondary_data_size,
            shader, error_messages, compiler_version);
}

HRESULT WINAPI vkd3d_D3DReflectVKD3D(const void *data,
        SIZE_T data_size, REFIID iid, void **reflection, unsigned int version)
{
    return D3DReflectVKD3D(data, data_size, iid, reflection, version);
}

#endif /* __wasm__ */
