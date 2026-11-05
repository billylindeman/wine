# wasm_modules.mk — the wineweb wasm-builtin module flavor (ADR-0014 B1).
#
# Wine's make owns WHAT to link and the exports: each builtin DLL's own
# staticlib archive (its objects, via winebuild --staticlib), winecrt0, and
# the export surface derived from the DLL's own .spec (spec-exports.sh).
# The wineweb side owns WHERE and HOW — the slab placement, the shared-
# state flags, the seam-kept dispositions, the extra in-module pulls — and
# passes them per DLL as make variables in the generated file $(WINEWEB_VARS)
# (wineweb's pure-logic manifest is the authority). This file never
# hardcodes placement.
#
# The wineweb build includes this file from the top-level makefile (a
# configure.ac WINE_APPEND_RULE `-include`) and invokes:
#
#   make -C <wine-build-tree> wineweb-modules \
#        WINEWEB_VARS=<generated per-DLL variables> WINEWEB_OUT=<artifact dir>
#
# A plain `make` (all, clean, a single archive) is unaffected: the variables
# are only consumed when a wineweb target is requested, and every recipe
# fails loud when the generated file is absent rather than linking with
# default placement.
#
# Per-DLL variables consumed from $(WINEWEB_VARS):
#   WINEWEB_DLLS                  — the module DLL names (one word each)
#   WINEWEB_MODULE_STACK          — the per-module shadow-stack size
#   WINEWEB_IMPORT_MEMORY         — the shared linear memory import spec
#   WINEWEB_ALLOW_MULTI_DEF       — the censused-duplicate first-wins flag
#   WINEWEB_NM                    — llvm-nm (the hard-export gate)
#   WINEWEB_<DLL>_GLOBAL_BASE     — the slab slice base (--global-base)
#   WINEWEB_<DLL>_TABLE_BASE      — the module's own fn-ptr slot base
#   WINEWEB_<DLL>_HARD_EXPORTS    — the boot/crossing names, hard --export=
#   WINEWEB_<DLL>_SKIP_EXPORTS    — the seam-kept spec names (comma list)
#   WINEWEB_<DLL>_PRE_OBJECTS     — objects linked BEFORE the archive
#                                   (first-wins seam definitions)
#   WINEWEB_<DLL>_TAIL_OBJECTS    — objects linked after winecrt0
#   WINEWEB_<DLL>_LIBS            — the in-module pulls (archive order)
#   WINEWEB_<DLL>_WRAP_FLAGS      — the allocator wraps (the pool contract)
#   WINEWEB_<DLL>_PREREQS         — extra prerequisites (the pulls, the
#                                   superproject objects, the generated file)

WINEWEB_VARS_OK :=
ifneq ($(strip $(WINEWEB_VARS)),)
include $(WINEWEB_VARS)
WINEWEB_VARS_OK := 1
endif

# The artifact output dir (wineweb's; wine's own build tree stays clean of
# the per-DLL artifacts unless WINEWEB_OUT points into it).
WINEWEB_OUT ?= .

# The spec → export-flags tool (the fork's own .spec machinery).
WINEWEB_SPEC_EXPORTS_SH := $(srcdir)/tools/wineweb/spec-exports.sh

# One rule per DLL, from the generated file's own module list. The staticlib
# and winecrt0 are wine's own make targets — a stale archive rebuilds and
# the module relinks (make IS the mtime-aware freshness gate; the C5b
# existence-only false-green class cannot return through this rule).
define WINEMAKE_module_rule
wineweb-$(1).wasm: dlls/$(1)/lib$(1).a libs/winecrt0/libwinecrt0.a \
  $$(WINEWEB_VARS) $$(WINEWEB_$(1)_PREREQS)
	@mkdir -p $$(WINEWEB_OUT)
	@echo '  WINEMAKE  $$@ (the wasm module flavor)'
	@test -n "$(WINEWEB_VARS_OK)" || { echo "ERROR: $$@ needs the generated per-DLL variables file — pass WINEWEB_VARS=<file> (wineweb's gen-wasm-vars writes it)"; exit 1; }
	@test -n "$$(WINEWEB_$(1)_GLOBAL_BASE)" || { echo "ERROR: no WINEWEB_$(1)_GLOBAL_BASE in $$(WINEWEB_VARS) — regenerate the file (wineweb's gen-wasm-vars)"; exit 1; }
	@for n in $$(WINEWEB_$(1)_HARD_EXPORTS); do \
	  found=; \
	  for f in dlls/$(1)/lib$(1).a $$(WINEWEB_$(1)_PRE_OBJECTS) $$(WINEWEB_$(1)_TAIL_OBJECTS); do \
	    if $$(WINEWEB_NM) --defined-only "$$$$f" 2>/dev/null | grep -q " $$$$n$$$$"; then found=1; break; fi; \
	  done; \
	  if [ -z "$$$$found" ]; then \
	    echo "ERROR: hard export '$$$$n' is not defined in dlls/$(1)/lib$(1).a (nor the seam objects) — the crossing surface drifted"; exit 1; \
	  fi; \
	done
	@SPEC_EXPORTS=$$($(WINEWEB_SPEC_EXPORTS_SH) $(srcdir)/dlls/$(1)/$(1).spec $$(if $$(WINEWEB_$(1)_SKIP_EXPORTS),--skip=$$(WINEWEB_$(1)_SKIP_EXPORTS))) || exit 1; \
	$(CC) $(TARGETFLAGS) -nostdlib -o $$(WINEWEB_OUT)/$$@ \
	  -Wl,--no-entry -Wl,--allow-undefined -Wl,--import-table -Wl,--no-stack-first \
	  $$(WINEWEB_ALLOW_MULTI_DEF) \
	  $$(WINEWEB_$(1)_WRAP_FLAGS) \
	  -Wl,--global-base=$$(WINEWEB_$(1)_GLOBAL_BASE) \
	  -Wl,--table-base=$$(WINEWEB_$(1)_TABLE_BASE) \
	  -Wl,-z,stack-size=$$(WINEWEB_MODULE_STACK) \
	  -Xlinker $$(WINEWEB_IMPORT_MEMORY) \
	  $$(foreach n,$$(WINEWEB_$(1)_HARD_EXPORTS),-Wl,--export=$$(n)) \
	  $$$${SPEC_EXPORTS} \
	  $$(WINEWEB_$(1)_PRE_OBJECTS) \
	  dlls/$(1)/lib$(1).a \
	  libs/winecrt0/libwinecrt0.a \
	  $$(WINEWEB_$(1)_TAIL_OBJECTS) \
	  $$(WINEWEB_$(1)_LIBS)
endef

$(foreach d,$(WINEWEB_DLLS),$(eval $(call WINEMAKE_module_rule,$(d))))

# The aggregate: every module in the generated file's list, in one make.
wineweb-modules: $(foreach d,$(WINEWEB_DLLS),wineweb-$(d).wasm)
ifeq ($(strip $(WINEWEB_DLLS)),)
	@echo "ERROR: wineweb-modules requires the generated per-DLL variables file — pass WINEWEB_VARS=<file> (wineweb's gen-wasm-vars writes it)"; exit 1
else
	@echo 'wineweb: all $(words $(WINEWEB_DLLS)) wasm modules are current'
endif
.PHONY: wineweb-modules

# The module artifacts are wineweb's product inputs — wine's own `clean`
# does not know them; this removes them wherever WINEWEB_OUT points.
wineweb-clean:
	@rm -f $(addprefix $(WINEWEB_OUT)/,$(foreach d,$(WINEWEB_DLLS),wineweb-$(d).wasm))
	@echo 'wineweb: module artifacts removed from $(WINEWEB_OUT)'
.PHONY: wineweb-clean
