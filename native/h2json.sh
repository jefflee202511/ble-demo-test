#!/usr/bin/env bash
# h2json.sh
# Reads a C header and writes the FFI map as JSON (same result as gen_ffi.js).
#
#   ./h2json.sh [header] [json]
#   ./h2json.sh ble_bridge.h ble_ffi.json      (these are the defaults)
#
# What it reads from the header:
#   typedef <ret> (*<name>)(<params>);     -> "callbacks"
#   BLE_API <ret> <name>(<params>);        -> "functions"
#
# "version" is kept from the existing JSON file (a person decides when it changes).
# Works in Git Bash on Windows (only bash, sed, tr and awk are used).
set -eu

HEADER="${1:-ble_bridge.h}"
JSON="${2:-ble_ffi.json}"

if [ ! -f "$HEADER" ]; then
  echo "h2json: header not found: $HEADER" >&2
  exit 2
fi

# Keep the version of the existing JSON file, or start at 1
VERSION=1
if [ -f "$JSON" ]; then
  OLD=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$JSON" | head -n 1)
  if [ -n "$OLD" ]; then VERSION="$OLD"; fi
fi

TMP="$JSON.tmp.$$"
trap 'rm -f "$TMP"' EXIT

# 1. Clean the header and cut it into one statement per line:
#    - remove CR (Windows line ends), // comments and preprocessor lines (#...)
#    - join all lines, remove /* */ comments
#    - split at ';' so that a declaration written over several lines becomes one line
tr -d '\r' < "$HEADER" \
  | sed -e 's@//.*$@@' -e '/^[[:space:]]*#/d' \
  | tr '\n' ' ' \
  | sed -e 's@/\*[^*]*\*\**\([^/*][^*]*\*\**\)*/@ @g' \
  | tr ';' '\n' \
  | awk -v version="$VERSION" '
# ---- JSON type name <-> C type. Add a line here for a new type. ----
BEGIN {
  n_types = 0
  add_type("int",   "int")
  add_type("void",  "void")
  add_type("u32",   "uint32_t")
  add_type("str",   "const char*")
  add_type("bytes", "const uint8_t*")
  n_cb = 0; n_fn = 0; failed = 0
}
function add_type(name, ctype) { n_types++; type_name[n_types] = name; type_c[n_types] = ctype }

function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }

# "const  char *" -> "const char*"
function norm(s) { gsub(/[ \t]+/, " ", s); gsub(/ ?\* ?/, "*", s); return trim(s) }

# position of the last ")" in s (0 if none)
function last_paren(s,   i) { for (i = length(s); i > 0; i--) if (substr(s, i, 1) == ")") return i; return 0 }

# C type -> JSON type name
function json_type(ctype, where,   i, wanted) {
  wanted = norm(ctype)
  for (i = 1; i <= n_cb; i++) if (cb_name[i] == wanted) return wanted
  for (i = 1; i <= n_types; i++) if (type_c[i] == wanted) return type_name[i]
  printf "h2json: unknown C type \"%s\" in %s (add it with add_type in h2json.sh)\n", wanted, where > "/dev/stderr"
  failed = 1
  return "?"
}

# "const char* address, uint32_t len" -> "\"str\", \"u32\""
function json_args(params, where,   n, parts, i, p, out, t) {
  params = trim(params)
  if (params == "" || params == "void") return ""
  n = split(params, parts, ",")
  out = ""
  for (i = 1; i <= n; i++) {
    p = trim(parts[i])
    t = p
    # drop the parameter name (the last word), unless the parameter is only a type
    if (match(p, /[A-Za-z0-9_]+$/) && RSTART > 1) t = substr(p, 1, RSTART - 1)
    out = out (i > 1 ? ", " : "") "\"" json_type(t, where) "\""
  }
  return out
}

{ stmt[NR] = $0 }

END {
  # pass 1: callbacks, so that functions can use their names as types
  for (k = 1; k <= NR; k++) {
    s = stmt[k]
    p = index(s, "typedef ")
    if (p == 0) continue
    s = substr(s, p + 8)
    open = index(s, "(")
    rest = substr(s, open + 1)
    if (open == 0 || trim(rest) !~ /^\*/) continue          # not a function pointer typedef
    close1 = index(rest, ")")
    name = trim(substr(rest, 1, close1 - 1)); sub(/^\*[ \t]*/, "", name)
    after = substr(rest, close1 + 1)
    popen = index(after, "("); pclose = last_paren(after)
    if (popen == 0 || pclose == 0) continue
    n_cb++
    cb_name[n_cb] = name
    cb_ret_c[n_cb] = substr(s, 1, open - 1)
    cb_params[n_cb] = substr(after, popen + 1, pclose - popen - 1)
  }

  # pass 2: functions
  for (k = 1; k <= NR; k++) {
    s = stmt[k]
    p = index(s, "BLE_API ")
    if (p == 0) continue
    s = substr(s, p + 8)
    open = index(s, "("); close1 = last_paren(s)
    if (open == 0 || close1 == 0) continue
    head = trim(substr(s, 1, open - 1))
    if (!match(head, /[A-Za-z0-9_]+$/)) continue
    n_fn++
    fn_name[n_fn] = substr(head, RSTART)
    fn_ret_c[n_fn] = substr(head, 1, RSTART - 1)
    fn_params[n_fn] = substr(s, open + 1, close1 - open - 1)
  }

  if (n_fn == 0) { print "h2json: no BLE_API function found in the header" > "/dev/stderr"; exit 1 }

  # resolve types (after all callbacks are known)
  for (i = 1; i <= n_cb; i++) { cb_ret[i] = json_type(cb_ret_c[i], cb_name[i]); cb_args[i] = json_args(cb_params[i], cb_name[i]) }
  for (i = 1; i <= n_fn; i++) { fn_ret[i] = json_type(fn_ret_c[i], fn_name[i]); fn_args[i] = json_args(fn_params[i], fn_name[i]) }
  if (failed) exit 1

  # ---- write the JSON: one entry per line, names padded so the columns line up ----
  print "{"
  print "  \"version\": " version ","
  print "  \"types\": {"
  for (i = 1; i <= n_types; i++) printf "    \"%s\": \"%s\"%s\n", type_name[i], type_c[i], (i < n_types ? "," : "")
  print "  },"

  width = 0
  for (i = 1; i <= n_cb; i++) if (length(cb_name[i]) > width) width = length(cb_name[i])
  print "  \"callbacks\": {"
  for (i = 1; i <= n_cb; i++)
    printf "    \"%s\":%*s { \"ret\": \"%s\", \"args\": [%s] }%s\n", cb_name[i], width - length(cb_name[i]), "", cb_ret[i], cb_args[i], (i < n_cb ? "," : "")
  print "  },"

  width = 0
  for (i = 1; i <= n_fn; i++) if (length(fn_name[i]) > width) width = length(fn_name[i])
  print "  \"functions\": {"
  for (i = 1; i <= n_fn; i++)
    printf "    \"%s\":%*s { \"ret\": \"%s\", \"args\": [%s] }%s\n", fn_name[i], width - length(fn_name[i]), "", fn_ret[i], fn_args[i], (i < n_fn ? "," : "")
  print "  }"
  print "}"

  printf "h2json: %d functions, %d callbacks\n", n_fn, n_cb > "/dev/stderr"
}
' > "$TMP"

# Replace the JSON file only when everything succeeded
mv "$TMP" "$JSON"
trap - EXIT
echo "h2json: wrote $JSON (version $VERSION)"
