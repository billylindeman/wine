/*
 * wineweb_internal.h — the internal-proc HostFn marker contract (the
 * rex-swap HostFn-registry block).
 *
 * Every wine function address that flows guest-visible is a **HostFn
 * marker** (the 0x7000_0000 band, ADR-0012's W2 marker-stability
 * contract) — never a funcref index, never a raw address. The guest-VA
 * vs marker branch in the winproc dispatch IS the x86-vs-wasm boundary:
 * a builtin identity reaches the dispatch as a marker and is serviced
 * through the marker machinery (the same HostFn the guest's IAT calls
 * already use).
 *
 * The boot (crates/wineweb-runtime/src/rex/internal.rs) owns the slot
 * table — `INTERNAL_PROCS`, index = slot — mints one stable marker per
 * ("wineweb-internal", symbol) key at boot, and fills
 * `wineweb_internal_procs[]` (the wine-c-visible array below) BEFORE
 * winproc_init/user32_install_callback_table run (the boot stage order:
 * CPU, then USER32). The fork sites read their slots from the array;
 * nothing here is ever a function call into Rust except
 * `wineweb_internal_call` (the dispatch's marker arm — the same shape
 * as the in-module guest-call primitive).
 *
 * This header must stay in lockstep with the Rust table's order and
 * count. The Rust side pins the drift with a build-time test that
 * parses WINWEB_INTERNAL_PROC_COUNT out of this file.
 *
 * Copyright 2026 the wineweb project — LGPL.
 */

#ifndef __WINWEB_INTERNAL_H
#define __WINWEB_INTERNAL_H

#include <stdint.h>

/* The marker band: the rex host-call markers mint from HOST_CALL_MARKER_BASE
 * upward; the internal-proc family seeds FIRST (the boot's order), so the
 * internal slots are exactly [BASE, BASE + WINWEB_INTERNAL_PROC_COUNT) and
 * the IAT/registry markers mint above them. */
#define WINWEB_MARKER_BASE 0x70000000u

/* The internal-proc slot ids — the boot-seeded marker array's indices.
 * Order is the contract with the Rust INTERNAL_PROCS table (the WndProc
 * family mirrors ALL_NTUSER_CLIENT_PROCS's A/W pairs, then the DDE class
 * procs, then the KernelCallbackTable's User32* entries). */
enum wineweb_internal_slot
{
    WINWEB_IP_ScrollBarWndProcA,
    WINWEB_IP_ScrollBarWndProcW,
    WINWEB_IP_MessageWndProcA,
    WINWEB_IP_MessageWndProcW,
    WINWEB_IP_PopupMenuWndProcA,
    WINWEB_IP_PopupMenuWndProcW,
    WINWEB_IP_DesktopWndProcA,
    WINWEB_IP_DesktopWndProcW,
    WINWEB_IP_DefWindowProcA,
    WINWEB_IP_DefWindowProcW,
    WINWEB_IP_IconTitleWndProcA,
    WINWEB_IP_IconTitleWndProcW,
    WINWEB_IP_ButtonWndProcA,
    WINWEB_IP_ButtonWndProcW,
    WINWEB_IP_ComboWndProcA,
    WINWEB_IP_ComboWndProcW,
    WINWEB_IP_ComboLBoxWndProcA,
    WINWEB_IP_ComboLBoxWndProcW,
    WINWEB_IP_DialogWndProcA,
    WINWEB_IP_DialogWndProcW,
    WINWEB_IP_EditWndProcA,
    WINWEB_IP_EditWndProcW,
    WINWEB_IP_ListBoxWndProcA,
    WINWEB_IP_ListBoxWndProcW,
    WINWEB_IP_MDIClientWndProcA,
    WINWEB_IP_MDIClientWndProcW,
    WINWEB_IP_StaticWndProcA,
    WINWEB_IP_StaticWndProcW,
    WINWEB_IP_ImeWndProcA,
    WINWEB_IP_ImeWndProcW,
    WINWEB_IP_GhostWndProcA,
    WINWEB_IP_GhostWndProcW,
    WINWEB_IP_WDML_ClientProc,
    WINWEB_IP_WDML_EventProc,
    WINWEB_IP_WDML_ServerNameProc,
    WINWEB_IP_WDML_ServerConvProc,
    /* The KernelCallbackTable's User32* entries (ALL_USER32_CALLBACKS order). */
    WINWEB_IP_User32CallDispatchCallback,
    WINWEB_IP_User32CallEnumDisplayMonitor,
    WINWEB_IP_User32CallSendAsyncCallback,
    WINWEB_IP_User32CallWinEventHook,
    WINWEB_IP_User32CallWinProc,
    WINWEB_IP_User32CallWindowsHook,
    WINWEB_IP_User32CopyImage,
    WINWEB_IP_User32DrawNonClientButton,
    WINWEB_IP_User32DrawScrollBar,
    WINWEB_IP_User32DrawText,
    WINWEB_IP_User32FreeCachedClipboardData,
    WINWEB_IP_User32ImmProcessKey,
    WINWEB_IP_User32ImmTranslateMessage,
    WINWEB_IP_User32InitBuiltinClasses,
    WINWEB_IP_User32LoadDriver,
    WINWEB_IP_User32LoadImage,
    WINWEB_IP_User32LoadSysMenu,
    WINWEB_IP_User32PostDDEMessage,
    WINWEB_IP_User32RenderSynthesizedFormat,
    WINWEB_IP_User32UnpackDDEMessage,
    WINWEB_IP_User32DragDropEnter,
    WINWEB_IP_User32DragDropLeave,
    WINWEB_IP_User32DragDropDrag,
    WINWEB_IP_User32DragDropDrop,
    WINWEB_IP_User32DragDropPost,

    WINWEB_IP_COUNT
};
/* The literal the rex boot's drift test parses (the lockstep pin);
 * C_ASSERTed against the enum so the two cannot diverge. */
#define WINWEB_INTERNAL_PROC_COUNT 61u
C_ASSERT( WINWEB_IP_COUNT == WINWEB_INTERNAL_PROC_COUNT );

/* The boot-seeded marker array (defined in dlls/user32/winproc.c — the
 * trampolines' TU). Filled by the boot before stage USER32; the sites
 * read their slots from it. Zero until seeded — a site reached before
 * the seed reads 0, the honest "no marker yet" value. */
extern unsigned int wineweb_internal_procs[WINWEB_INTERNAL_PROC_COUNT];

/* The dispatch's marker arm (defined by the rex glue — the same pattern
 * as the in-module guest-call primitive): service the marker's HostFn
 * over the stdcall wndproc frame (hwnd/msg/wparam/lparam — the W3
 * contract: the args ride the guest stack; the seam marshals them).
 * Answers the wndproc's EAX. */
extern unsigned int wineweb_internal_call( unsigned int marker, unsigned int hwnd,
                                           unsigned int msg, unsigned int wparam,
                                           unsigned int lparam );

/* The internal-proc marker test. A builtin identity flows guest-visible
 * as exactly this band; the dispatch routes it through the marker
 * machinery and the `((WNDPROC)proc)` call_indirect dies for builtins. */
static inline int wineweb_is_internal_marker( unsigned int proc )
{
    return proc >= WINWEB_MARKER_BASE &&
           proc - WINWEB_MARKER_BASE < WINWEB_INTERNAL_PROC_COUNT;
}

#endif /* __WINWEB_INTERNAL_H */
