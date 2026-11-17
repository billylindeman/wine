/*
 * SHLWAPI IStream functions
 *
 * Copyright 2002 Jon Griffiths
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include <stdarg.h>
#include <string.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winnls.h"
#define NO_SHLWAPI_REG
#define NO_SHLWAPI_PATH
#include "shlwapi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

#ifdef __wasm__ /* wineweb: the file stream (the P64 serve). shcore is not \
    a built module — the p38 loud arm answered SHCreateStreamOnFileW's \
    S_OK with the stream UNSET and the .lnk parse read garbage (the \
    tracer's "table index is out of bounds" at IPersistStream_fnLoad's \
    IStream_Read). Wine's own shape (shcore/main.c filestreamvtbl): \
    CreateFileW + ReadFile + SetFilePointer through the served builtins; \
    the .lnk parse asks Read/Seek/Stat only — the unasked slots are \
    wine's own refusals (STG_E_INVALIDFUNCTION, the read-mode Write's \
    STG_E_ACCESSDENIED). The module exports the name, so shell32's \
    import binds it at the wire's resolveSeam (a) — shlwapi \
    instantiates first (the P33b order). */
struct wineweb_filestream
{
    IStream IStream_iface;
    LONG refcount;
    HANDLE handle;
};

static inline struct wineweb_filestream *wineweb_filestream_from_IStream( IStream *iface )
{
    return CONTAINING_RECORD( iface, struct wineweb_filestream, IStream_iface );
}

static HRESULT WINAPI wineweb_filestream_QueryInterface( IStream *iface, REFIID riid, void **obj )
{
    struct wineweb_filestream *fs = wineweb_filestream_from_IStream( iface );
    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IStream ))
    {
        *obj = &fs->IStream_iface;
        IStream_AddRef( iface );
        return S_OK;
    }
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI wineweb_filestream_AddRef( IStream *iface )
{
    return InterlockedIncrement( &wineweb_filestream_from_IStream( iface )->refcount );
}

static ULONG WINAPI wineweb_filestream_Release( IStream *iface )
{
    struct wineweb_filestream *fs = wineweb_filestream_from_IStream( iface );
    ULONG ref = InterlockedDecrement( &fs->refcount );
    if (!ref)
    {
        CloseHandle( fs->handle );
        free( fs );
    }
    return ref;
}

static HRESULT WINAPI wineweb_filestream_Read( IStream *iface, void *buff, ULONG len, ULONG *read )
{
    struct wineweb_filestream *fs = wineweb_filestream_from_IStream( iface );
    DWORD got = 0;
    if (!ReadFile( fs->handle, buff, len, &got, NULL ))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (read) *read = got;
    return got == len ? S_OK : S_FALSE;
}

static HRESULT WINAPI wineweb_filestream_Write( IStream *iface, const void *buff, ULONG len, ULONG *written )
{
    return STG_E_ACCESSDENIED;
}

static HRESULT WINAPI wineweb_filestream_Seek( IStream *iface, LARGE_INTEGER move, DWORD origin,
                                               ULARGE_INTEGER *newpos )
{
    struct wineweb_filestream *fs = wineweb_filestream_from_IStream( iface );
    DWORD method;
    LONG low;
    switch (origin)
    {
        case STREAM_SEEK_SET: method = FILE_BEGIN; break;
        case STREAM_SEEK_CUR: method = FILE_CURRENT; break;
        case STREAM_SEEK_END: method = FILE_END; break;
        default: return STG_E_INVALIDFUNCTION;
    }
    if (move.QuadPart >> 32) return STG_E_INVALIDFUNCTION; /* the parse's .lnk spans stay low */
    low = SetFilePointer( fs->handle, move.LowPart, NULL, method );
    if (low == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
        return HRESULT_FROM_WIN32( GetLastError() );
    if (newpos) newpos->QuadPart = (ULONGLONG)low;
    return S_OK;
}

static HRESULT WINAPI wineweb_filestream_Stat( IStream *iface, STATSTG *stat, DWORD flag )
{
    struct wineweb_filestream *fs = wineweb_filestream_from_IStream( iface );
    DWORD size = GetFileSize( fs->handle, NULL );
    if (size == INVALID_FILE_SIZE) return HRESULT_FROM_WIN32( GetLastError() );
    memset( stat, 0, sizeof(*stat) );
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = size;
    return S_OK;
}

static HRESULT WINAPI wineweb_filestream_invalid( IStream *iface ) { return STG_E_INVALIDFUNCTION; }

static const IStreamVtbl wineweb_filestream_vtbl =
{
    wineweb_filestream_QueryInterface,
    wineweb_filestream_AddRef,
    wineweb_filestream_Release,
    wineweb_filestream_Read,
    wineweb_filestream_Write,
    wineweb_filestream_Seek,
    (void *)wineweb_filestream_invalid, /* SetSize */
    (void *)wineweb_filestream_invalid, /* CopyTo */
    (void *)wineweb_filestream_invalid, /* Commit */
    (void *)wineweb_filestream_invalid, /* Revert */
    (void *)wineweb_filestream_invalid, /* LockRegion */
    (void *)wineweb_filestream_invalid, /* UnlockRegion */
    wineweb_filestream_Stat,
    (void *)wineweb_filestream_invalid, /* Clone */
};

HRESULT WINAPI SHCreateStreamOnFileW( const WCHAR *path, DWORD mode, IStream **stream )
{
    struct wineweb_filestream *fs;
    DWORD access, share, disposition;

    if (!path || !stream) return E_INVALIDARG;
    if ((mode & (STGM_CONVERT | STGM_DELETEONRELEASE | STGM_TRANSACTED)) != 0) return E_INVALIDARG;

    switch (mode & 0xf)
    {
        case STGM_WRITE: case STGM_READWRITE: access = GENERIC_READ | GENERIC_WRITE; break;
        case STGM_READ: access = GENERIC_READ; break;
        default: return E_INVALIDARG;
    }
    switch (mode & 0xf0)
    {
        case 0: case STGM_SHARE_DENY_NONE: share = FILE_SHARE_READ | FILE_SHARE_WRITE; break;
        case STGM_SHARE_DENY_READ: share = FILE_SHARE_WRITE; break;
        case STGM_SHARE_DENY_WRITE: share = FILE_SHARE_READ; break;
        case STGM_SHARE_EXCLUSIVE: share = 0; break;
        default: return E_INVALIDARG;
    }
    disposition = (mode & STGM_CREATE) ? CREATE_ALWAYS : OPEN_EXISTING;

    fs = malloc( sizeof(*fs) );
    if (!fs) return E_OUTOFMEMORY;
    fs->IStream_iface.lpVtbl = &wineweb_filestream_vtbl;
    fs->refcount = 1;
    fs->handle = CreateFileW( path, access, share, NULL, disposition, 0, 0 );
    if (fs->handle == INVALID_HANDLE_VALUE)
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        free( fs );
        return hr;
    }
    *stream = &fs->IStream_iface;
    return S_OK;
}
#endif


/*************************************************************************
 * @       [SHLWAPI.166]
 *
 * Determine if a stream has 0 length.
 *
 * PARAMS
 *  lpStream [I] IStream object
 *
 * RETURNS
 *  TRUE:  If the stream has 0 length
 *  FALSE: Otherwise.
 */
BOOL WINAPI SHIsEmptyStream(IStream *lpStream)
{
  STATSTG statstg;
  BOOL bRet = TRUE;

  TRACE("(%p)\n", lpStream);

  memset(&statstg, 0, sizeof(statstg));

  if(SUCCEEDED(IStream_Stat(lpStream, &statstg, 1)))
  {
    if(statstg.cbSize.QuadPart)
      bRet = FALSE; /* Non-Zero */
  }
  else
  {
    DWORD dummy, read_len;

    /* Try to read from the stream */
    if (SUCCEEDED(IStream_Read(lpStream, &dummy, sizeof(dummy), &read_len)) && read_len == sizeof(dummy))
    {
      LARGE_INTEGER zero;
      zero.QuadPart = 0;

      IStream_Seek(lpStream, zero, 0, NULL);
      bRet = FALSE; /* Non-Zero */
    }
  }
  return bRet;
}
