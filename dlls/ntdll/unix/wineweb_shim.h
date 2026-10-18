/*
 * wineweb_shim.h — the wineweb backing contracts for Wine's unix side on wasm
 * (docs/04 §4 surface S2 + docs/03 §6 VM manager).
 *
 * Ownership: this header declares, and wineweb_shim.c implements, the POSIX
 * surface that Wine's ntdll unix sources expect from a host OS but that wasi
 * does not provide (or does not provide with our semantics). Every backing
 * is one of:
 *
 *   - an in-module Rust contract (`p2_*`, the wineweb-peloader C ABI): calls
 *     within the final wineweb-core module, no import boundary;
 *   - a `wineweb_host_*` import (docs/04 §4): resolved as imports on wasm,
 *     provided natively by the shim test harness (crates/wineweb-host-native);
 *   - pure C (deterministic getrandom, umask bookkeeping).
 *
 * It must never: allocate guest memory outside the VM manager, fabricate
 * success for an operation the platform cannot perform (loud-unsupported
 * instead), or link wasi's emulated signal/mman libraries (our definitions
 * override them at final-module link).
 */
#ifndef __WINEWEB_SHIM_H
#define __WINEWEB_SHIM_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* wineweb_host S2 imports (docs/04 §4). On wasm these are the module's
 * imports; natively the test harness's Rust provides them. Signatures follow
 * the docs/04 §2 width rule: host-width (size_t) for pointers and lengths. */

#ifdef __wasi__
#define WINEWEB_HOST_IMPORT(name) __attribute__((import_module("wineweb_host"), import_name(name)))
#else
#define WINEWEB_HOST_IMPORT(name)
#endif

/* time group — all guest-visible clocks flow through here (determinism mode,
 * docs/04 §8: host can pin them). */
WINEWEB_HOST_IMPORT("query_perf_counter") extern uint64_t wineweb_host_query_perf_counter(void);   /* monotonic ns */
WINEWEB_HOST_IMPORT("query_unix_time") extern uint64_t wineweb_host_query_unix_time(void);      /* seconds */

/* diag group — loud-unsupported reporting (docs/04 §8). */
WINEWEB_HOST_IMPORT("log") extern void wineweb_host_log(unsigned int level, const char *ptr, size_t len);

/* sched group — thread_spawn(entry, ctx) (docs/04 §4). `entry`/`ctx` are
 * opaque guest-width handles passed through to the scheduler; the
 * pthread-to-FEX conversion is the thread-bootstrap contract (docs/03 §7,
 * P2.9). Returns a host thread id or a negative error. */
WINEWEB_HOST_IMPORT("thread_spawn") extern int wineweb_host_thread_spawn(size_t entry, size_t ctx);

/* cpu group — the guest CPU model (docs/02 §8: the FEX HostFeatures baseline,
 * "CPUID-ish ... fixed baseline feature set"). The backing is a fixed table;
 * when FEX's CPUID emulation gets wired (P2.4+), the backing becomes that
 * query and the seam stays. PF_* indices in pf_bits: bit n set means
 * KUSER_SHARED_DATA ProcessorFeatures[n] is TRUE. */
struct wineweb_cpu_model
{
    char vendor[13];                    /* CPUID leaf 0 vendor string */
    char brand[49];                     /* CPUID leaf 0x80000002..04 string */
    unsigned int family, model, stepping; /* CPUID leaf 1 version info */
    uint64_t pf_bits[2];                /* ProcessorFeatures bitmap */
};
extern void wineweb_shim_cpu_model(struct wineweb_cpu_model *out);

/* ------------------------------------------------------------------------- */
/* In-module Rust contracts (wineweb-peloader C ABI, crates/wineweb-peloader).
 * Guest addresses are offsets in the linear memory (docs/01 §5 zero
 * translation); allocations are page-granular and commit-time zero-filled. */

/* Reserve + commit `size` bytes (anonymous). `base` 0 = system pick
 * (top-down, NT default); `protect` is a Win32 PAGE_* constant. Returns 0
 * and writes the guest base, or a negative status. */
extern int p2_vm_alloc(uint64_t base, uint64_t size, uint32_t protect, uint64_t *out_base);
/* Fixed-address commit (MAP_FIXED): commits into an existing reservation, or
 * fails loud if the range is otherwise taken (docs/03 §6). */
extern int p2_vm_alloc_fixed(uint64_t base, uint64_t size, uint32_t protect, uint64_t *out_base);
/* Reservation-only shape (Wine's MEM_RESERVE): tracked range, no pages. */
extern int p2_vm_reserve(uint64_t base, uint64_t size, uint64_t *out_base);
/* Protection update (Wine's mprotect path): bookkeeping only, docs/03 §6. */
extern int p2_vm_protect(uint64_t base, uint64_t size, uint32_t protect);
/* munmap shape: whole-allocation ranges are released (re-mappable), other
 * ranges decommitted (reservation kept, next commit re-zeroes). */
extern int p2_vm_unmap(uint64_t base, uint64_t size);
/* Decommit a page range (contents gone; next commit re-zeroes). */
extern int p2_vm_decommit(uint64_t base, uint64_t size);
/* Release a whole allocation previously returned by p2_vm_alloc. */
extern int p2_vm_free(uint64_t base);

#endif /* __WINEWEB_SHIM_H */
