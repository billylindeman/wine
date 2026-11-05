#!/bin/sh
# spec-exports.sh — a wine .spec file's wasm export flags (the wineweb
# wasm-builtin module flavor, ADR-0014 B1).
#
# A wine module (wineweb-<dll>.wasm) exports __wasm_call_ctors plus the
# surface its own .spec declares: the spec IS the DLL's export contract.
# The names become --export-if-defined= flags — lld exports a name only
# when the link actually defines it, so stub entries (no staticlib body)
# and -import forwarders (the backing lives in the provider DLL) drop out
# on their own and are dispositioned in the wineweb seam inventory instead.
#
# The parsing mirrors wine's spec syntax as the wineweb side's reference
# generator (tools/modules/gen-module-exports.mjs) reads it: `@ <conv>
# [-flags] NAME(args) [csym] [# comment]`, plus wine's Win95 ordinal format
# (comctl32/shlwapi/shell32 have no `@` prefix — the leading integer IS the
# export slot). A trailing C-symbol override (`@ stdcall foo(ptr)
# bar_impl`) renames the body: the body's name is exported, the spec name
# resolves via the wiring.
#
# Usage: spec-exports.sh <spec> [--skip=name1,name2]
# Prints one flag per line. --skip names spec-surface entries to EXCLUDE
# (the seam-kept disposition: the shared-table slot keeps the runtime's
# backing body, so the module must not export the name).
set -eu

spec=""
skip=""

for arg in "$@"; do
  case "$arg" in
    --skip=*) skip="${arg#--skip=}" ;;
    *) if [ -z "$spec" ]; then spec="$arg"; else echo "usage: spec-exports.sh <spec> [--skip=n1,n2]" >&2; exit 2; fi ;;
  esac
done
if [ -z "$spec" ]; then echo "usage: spec-exports.sh <spec> [--skip=n1,n2]" >&2; exit 2; fi
if [ ! -f "$spec" ]; then echo "spec-exports.sh: no such spec: $spec" >&2; exit 1; fi

# The skip names, one per line (empty entries dropped).
echo "$skip" | tr ',' '\n' | sed '/^$/d' > "$$.skip" || true

awk -v skipfile="$$.skip" '
  BEGIN { while ((getline line < skipfile) > 0) skip[line] = 1 }
  function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
  {
    line = $0
    sub(/[ \t]#.*/, "", line)              # a trailing # comment is spec syntax
    line = trim(line)
    if (line !~ /^(@|[0-9])/) next         # not an export entry
    n = split(line, w, /[ \t]+/)
    conv = w[2]
    if (conv == "" || conv == "extern" || conv == "stub") next   # no wasm call body
    rest = substr(line, index(line, conv) + length(conv))
    rest = trim(rest)
    while (rest ~ /^-/) {                  # strip the -flags (-import/-noname/…)
      sp = index(rest, " ")
      if (sp == 0) { rest = ""; break }
      rest = trim(substr(rest, sp + 1))
    }
    if (!match(rest, /^[A-Za-z0-9_]+/)) next
    name = substr(rest, RSTART, RLENGTH)
    after = trim(substr(rest, RLENGTH + 1))
    if (after !~ /^\(/) next               # extern/decorated forms without (args)
    cp = index(after, ")")
    if (cp == 0) next
    tail = trim(substr(after, cp + 1))
    sym = name
    if (tail != "") {
      split(tail, t, /[ \t]+/)
      if (index(t[1], ".") == 0 && t[1] ~ /^[A-Za-z0-9_]+$/) sym = t[1]
    }
    if (sym in seen) next
    if (sym in skip) { seen[sym] = 1; next }   # the seam-kept disposition
    seen[sym] = 1
    print "-Wl,--export-if-defined=" sym
  }
' "$spec"

rm -f "$$.skip"
