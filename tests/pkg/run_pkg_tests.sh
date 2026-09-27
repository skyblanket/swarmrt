#!/bin/bash
# Package manager tests: swarm.json, swarm.lock, `swc add` / `install` /
# `update` / `remove`, and dependency-aware import resolution on both
# `swc build` and `swc run`.
#
# Fully offline: the "remotes" are local bare git repositories created in
# a temp dir and addressed with file:// URLs. Needs git on PATH.
#
# Run from anywhere:  bash tests/pkg/run_pkg_tests.sh   (or `make test-pkg`)

set -u
SWARMRT_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SWC="$SWARMRT_ROOT/bin/swc"

if [ ! -x "$SWC" ]; then
    echo "error: swc not found at $SWC — run 'make swc libswarmrt' first" >&2
    exit 2
fi
if ! command -v git >/dev/null 2>&1; then
    echo "error: git is required for the package tests" >&2
    exit 2
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/sw_pkg_test.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
# The WORK path goes into file:// URLs and manifests; resolve symlinks
# (macOS /var -> /private/var) so paths compare equal.
WORK="$(cd "$WORK" && pwd -P)"

# Deterministic git, isolated from the user's config.
export GIT_CONFIG_GLOBAL="$WORK/gitconfig"
export GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=swtest GIT_AUTHOR_EMAIL=swtest@example.invalid
export GIT_COMMITTER_NAME=swtest GIT_COMMITTER_EMAIL=swtest@example.invalid
git config --global init.defaultBranch main
git config --global advice.detachedHead false

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); echo "PASS  $1"; }
bad()  { FAIL=$((FAIL + 1)); echo "FAIL  $1"; [ -n "${2:-}" ] && sed 's/^/      | /' <<<"$2"; }
check() { # check "<desc>" <command...>  — passes when the command succeeds
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi
}

# --- fake remotes --------------------------------------------------------
# new_pkg <name>: bare remote $WORK/remotes/<name>.git + working copy
# $WORK/wc/<name>. commit_pkg <name> <msg> [tag]: commit + push (+ tag).
new_pkg() {
    git init -q --bare "$WORK/remotes/$1.git"
    git init -q "$WORK/wc/$1"
    git -C "$WORK/wc/$1" remote add origin "file://$WORK/remotes/$1.git"
}
commit_pkg() {
    local name="$1" msg="$2" tag="${3:-}"
    git -C "$WORK/wc/$name" add -A
    git -C "$WORK/wc/$name" commit -q -m "$msg"
    git -C "$WORK/wc/$name" push -q origin main 2>/dev/null
    if [ -n "$tag" ]; then
        git -C "$WORK/wc/$name" tag -f "$tag" >/dev/null
        git -C "$WORK/wc/$name" push -q -f origin "refs/tags/$tag" 2>/dev/null
    fi
}
remote_sha() { git -C "$WORK/remotes/$1.git" rev-parse "$2^{commit}"; }
url() { echo "file://$WORK/remotes/$1.git"; }

# greet: module at the package root.
new_pkg greet
cat > "$WORK/wc/greet/Greet.sw" <<'EOF'
module Greet

fun hello(name) {
    "hello " ++ name
}
EOF
commit_pkg greet "greet v1" v1.0.0
GREET_V1="$(remote_sha greet v1.0.0)"

# strutil: modules under src/ — a transitive dependency of banner.
new_pkg strutil
mkdir -p "$WORK/wc/strutil/src"
cat > "$WORK/wc/strutil/src/Strutil.sw" <<'EOF'
module Strutil

fun wrap(s, c) {
    c ++ s ++ c
}
EOF
commit_pkg strutil "strutil v0.1" v0.1.0
STRUTIL_V01="$(remote_sha strutil v0.1.0)"
echo "# later" >> "$WORK/wc/strutil/src/Strutil.sw"
commit_pkg strutil "strutil main moves on"

# banner: has its own manifest (depends on strutil) and an internal module
# (BannerHelp) that only its own package can see.
new_pkg banner
mkdir -p "$WORK/wc/banner/src"
cat > "$WORK/wc/banner/swarm.json" <<EOF
{"name": "banner", "version": "1.0.0",
 "deps": {"strutil": {"git": "$(url strutil)", "ref": "v0.1.0"}}}
EOF
cat > "$WORK/wc/banner/src/Banner.sw" <<'EOF'
module Banner

import Strutil
import BannerHelp

fun make(s) {
    Strutil.wrap(BannerHelp.upper_ish(s), "*")
}
EOF
cat > "$WORK/wc/banner/src/BannerHelp.sw" <<'EOF'
module BannerHelp

fun upper_ish(s) {
    "[" ++ s ++ "]"
}
EOF
commit_pkg banner "banner v1"
BANNER_V1="$(remote_sha banner main)"

# localpkg: a path dependency (also ships a Std.sw to trip the lib/ warning).
mkdir -p "$WORK/localpkg"
cat > "$WORK/localpkg/Local.sw" <<'EOF'
module Local

fun value() {
    42
}
EOF

# --- the project ---------------------------------------------------------
APP="$WORK/app"
mkdir -p "$APP/sub"
cat > "$APP/main.sw" <<'EOF'
module Main

import Greet
import Banner

fun main() {
    print(Greet.hello("sw"))
    print(Banner.make("x"))
}
EOF
cat > "$APP/sub/other.sw" <<'EOF'
module Other

import Local

fun main() {
    print(Local.value())
}
EOF
cd "$APP"

lock_sha() { # lock_sha <dep> — the sha pinned in swarm.lock (one entry per line)
    grep "^    \"$1\": " "$APP/swarm.lock" | sed -n 's/.*"sha": "\([0-9a-f]*\)".*/\1/p'
}
head_sha() { git -C "$APP/.swarm/deps/$1" rev-parse HEAD; }

# build_run <file> <expected-stdout> <desc>: `swc build` + run, then `swc run`.
build_run() {
    local file="$1" want="$2" desc="$3" out
    if out="$("$SWC" build "$file" -o "$WORK/prog" 2>&1)"; then
        local got; got="$("$WORK/prog" 2>/dev/null)"
        if [ "$got" = "$want" ]; then ok "$desc (swc build)"; else bad "$desc (swc build)" "$got"; fi
    else
        bad "$desc (swc build: compile failed)" "$out"
    fi
    local got; got="$("$SWC" run "$file" 2>&1)"
    if [ "$got" = "$want" ]; then ok "$desc (swc run)"; else bad "$desc (swc run)" "$got"; fi
}

# 1. No manifest → a clear error, nothing created.
out="$("$SWC" add greet "$(url greet)" 2>&1)"
if [ $? -ne 0 ] && grep -q "no swarm.json" <<<"$out" && [ ! -e .swarm ]; then
    ok "add without a manifest is refused"
else bad "add without a manifest is refused" "$out"; fi

cat > swarm.json <<'EOF'
{"name": "app", "version": "0.1.0", "description": "pkg test app"}
EOF

# 2. swc add <git>@<tag>: manifest, lock, checkout.
out="$("$SWC" add greet "$(url greet)@v1.0.0" 2>&1)"
if [ $? -eq 0 ]; then ok "swc add greet url@v1.0.0"; else bad "swc add greet url@v1.0.0" "$out"; fi
check "manifest records the git dep and ref" grep -q '"greet": {"git": "file://.*greet.git", "ref": "v1.0.0"}' swarm.json
check "manifest keeps unmanaged fields" grep -q '"description": "pkg test app"' swarm.json
check "lock pins greet to the tag's commit" test "$(lock_sha greet)" = "$GREET_V1"
check "checkout HEAD is the locked sha" test "$(head_sha greet)" = "$GREET_V1"

# 3. A branch ref + a dependency with its own manifest (transitive).
out="$("$SWC" add banner "$(url banner)@main" 2>&1)"
if [ $? -eq 0 ]; then ok "swc add banner url@main"; else bad "swc add banner url@main" "$out"; fi
check "transitive dep strutil installed" test -f .swarm/deps/strutil/src/Strutil.sw
check "lock pins transitive strutil at its ref" test "$(lock_sha strutil)" = "$STRUTIL_V01"
check "lock records who pulled strutil in" grep -q '"via": "banner"' swarm.lock
check "lock pins banner" test "$(lock_sha banner)" = "$BANNER_V1"

# 4. Programs importing dep modules — build and run agree.
build_run main.sw "$(printf 'hello sw\n*[x]*')" "import from deps (root module, src/ module, package-internal import)"

# 5. The remote moves (new commit on main, tag force-moved): a locked
#    install still checks out exactly the locked commits.
cat > "$WORK/wc/greet/Greet.sw" <<'EOF'
module Greet

fun hello(name) {
    "HOWDY " ++ name
}
EOF
commit_pkg greet "greet changed under the same tag" v1.0.0
sed -i.bak 's/"\[" ++ s ++ "\]"/"<" ++ s ++ ">"/' "$WORK/wc/banner/src/BannerHelp.sw"
rm -f "$WORK/wc/banner/src/BannerHelp.sw.bak"
commit_pkg banner "banner v2"
BANNER_V2="$(remote_sha banner main)"
rm -rf .swarm
cp swarm.lock "$WORK/lock.before"
out="$("$SWC" install 2>&1)"
if [ $? -eq 0 ]; then ok "swc install from lock (fresh .swarm)"; else bad "swc install from lock (fresh .swarm)" "$out"; fi
check "locked install: greet still at the old sha" test "$(head_sha greet)" = "$GREET_V1"
check "locked install: banner still at the old sha" test "$(head_sha banner)" = "$BANNER_V1"
check "locked install leaves swarm.lock unchanged" cmp -s swarm.lock "$WORK/lock.before"
build_run main.sw "$(printf 'hello sw\n*[x]*')" "locked install runs the locked code"
check "swc deps is an alias of install" "$SWC" deps

# 6. swc update re-resolves refs to the new commits.
out="$("$SWC" update 2>&1)"
if [ $? -eq 0 ]; then ok "swc update"; else bad "swc update" "$out"; fi
check "update: banner moved to the new main" test "$(lock_sha banner)" = "$BANNER_V2"
check "update: greet follows the moved tag" test "$(head_sha greet)" = "$(remote_sha greet v1.0.0)"
build_run main.sw "$(printf 'HOWDY sw\n*<x>*')" "updated deps"

# 7. Conflicting refs for one name → a clear error; manifest unchanged.
cp swarm.json "$WORK/manifest.before"
out="$("$SWC" add strutil "$(url strutil)@main" 2>&1)"
if [ $? -ne 0 ] && grep -q "dependency conflict for 'strutil'" <<<"$out"; then
    ok "conflicting refs for one dependency are refused"
else bad "conflicting refs for one dependency are refused" "$out"; fi
check "failed add leaves swarm.json unchanged" cmp -s swarm.json "$WORK/manifest.before"

# 8. Bad ref and unreachable remote.
out="$("$SWC" add nope "$(url greet)@no-such-tag" 2>&1)"
if [ $? -ne 0 ] && grep -q "bad ref 'no-such-tag'" <<<"$out"; then ok "bad ref is a clear error"
else bad "bad ref is a clear error" "$out"; fi
out="$("$SWC" add gone "file://$WORK/remotes/does-not-exist.git" 2>&1)"
if [ $? -ne 0 ] && grep -q "could not fetch 'gone'" <<<"$out"; then ok "unreachable remote is a clear error"
else bad "unreachable remote is a clear error" "$out"; fi
check "failed adds leave swarm.json unchanged" cmp -s swarm.json "$WORK/manifest.before"
check "failed adds leave no checkout behind" test ! -e .swarm/deps/nope -a ! -e .swarm/deps/gone

# 9. A locked commit that vanished from the remote.
cp swarm.lock "$WORK/lock.good"
sed -i.bak "s/$(lock_sha greet)/0000000000000000000000000000000000000001/" swarm.lock
rm -f swarm.lock.bak
rm -rf .swarm/deps/greet
out="$("$SWC" install 2>&1)"
if [ $? -ne 0 ] && grep -q "locked commit 0000000000000000000000000000000000000001 of 'greet' is not in" <<<"$out"; then
    ok "missing locked commit is a clear error"
else bad "missing locked commit is a clear error" "$out"; fi
cp "$WORK/lock.good" swarm.lock
check "install recovers with the good lock" "$SWC" install

# 10. Path dependency, from a subdirectory; lib/ shadow warning.
cat > "$WORK/localpkg/Std.sw" <<'EOF'
module Std

fun nothing() {
    0
}
EOF
cd "$APP/sub"
out="$("$SWC" add localpkg --path ../../localpkg 2>&1)"
if [ $? -eq 0 ]; then ok "swc add --path (from a subdirectory)"; else bad "swc add --path (from a subdirectory)" "$out"; fi
if grep -q "warning: dependency 'localpkg' provides Std.sw, which shadows" <<<"$out"; then
    ok "shadowing a lib/ module warns"
else bad "shadowing a lib/ module warns" "$out"; fi
rm -f "$WORK/localpkg/Std.sw"
cd "$APP"

# A module provided by two packages is ambiguous, not a silent pick.
mkdir -p "$WORK/dup_a" "$WORK/dup_b"
printf 'module Dup\n\nfun f() {\n    1\n}\n' > "$WORK/dup_a/Dup.sw"
printf 'module Dup\n\nfun f() {\n    2\n}\n' > "$WORK/dup_b/Dup.sw"
printf 'module Main\n\nimport Dup\n\nfun main() {\n    print(Dup.f())\n}\n' > "$APP/use_dup.sw"
"$SWC" add dup_a --path ../dup_a >/dev/null 2>&1
"$SWC" add dup_b --path ../dup_b >/dev/null 2>&1
out="$("$SWC" build use_dup.sw -o "$WORK/prog" 2>&1)"
if [ $? -ne 0 ] && grep -q "import 'Dup' is ambiguous" <<<"$out"; then ok "module in two packages is ambiguous (swc build)"
else bad "module in two packages is ambiguous (swc build)" "$out"; fi
out="$("$SWC" run use_dup.sw 2>&1)"
if [ $? -ne 0 ] && grep -q "import 'Dup' is ambiguous" <<<"$out"; then ok "module in two packages is ambiguous (swc run)"
else bad "module in two packages is ambiguous (swc run)" "$out"; fi
"$SWC" remove dup_a >/dev/null 2>&1
"$SWC" remove dup_b >/dev/null 2>&1
rm -f "$APP/use_dup.sw"
check "path dep is a symlink to the package" test -L .swarm/deps/localpkg
check "lock records the path dep" grep -q '"localpkg": {"path": ' swarm.lock
build_run sub/other.sw "42" "path dep, project root found from a subdirectory"

# 11. swc remove.
out="$("$SWC" remove localpkg 2>&1)"
if [ $? -eq 0 ]; then ok "swc remove localpkg"; else bad "swc remove localpkg" "$out"; fi
check "remove: gone from swarm.json" bash -c "! grep -q localpkg swarm.json"
check "remove: gone from swarm.lock" bash -c "! grep -q localpkg swarm.lock"
check "remove: checkout pruned" test ! -e .swarm/deps/localpkg
check "remove: the path package itself is untouched" test -f "$WORK/localpkg/Local.sw"
out="$("$SWC" build sub/other.sw -o "$WORK/prog" 2>&1)"
if [ $? -ne 0 ] && grep -q "cannot resolve import 'Local'.*swc install" <<<"$out"; then
    ok "import of a removed dep fails with an install hint"
else bad "import of a removed dep fails with an install hint" "$out"; fi
out="$("$SWC" remove localpkg 2>&1)"
if [ $? -ne 0 ] && grep -q "not a dependency" <<<"$out"; then ok "removing an unknown dep is an error"
else bad "removing an unknown dep is an error" "$out"; fi

# 12. Injection / validation: nothing reaches git as an option or a shell.
CANARY="$WORK/pwned"
cp swarm.json "$WORK/manifest.before"
refuse() { # refuse "<desc>" <swc args...>
    local desc="$1"; shift
    local o; o="$("$SWC" "$@" 2>&1)"
    if [ $? -ne 0 ] && [ ! -e "$CANARY" ]; then ok "$desc"; else bad "$desc" "$o"; fi
}
refuse "name with shell metacharacters"   add 'x;touch pwned' "$(url greet)"
refuse "name with a path separator"       add '../evil' "$(url greet)"
refuse "name starting with '-'"           add -- "$(url greet)"
refuse "uppercase name"                   add Greet "$(url greet)"
refuse "URL that is a git option"         add evil "--upload-pack=touch $CANARY"
refuse "URL with -o (ssh option)"         add evil "-oProxyCommand=touch $CANARY"
refuse "ext:: remote helper URL"          add evil "ext::sh -c touch% $CANARY"
refuse "ref that is a git option"         add evil "$(url greet)@--upload-pack=touch"
refuse "ref with shell metacharacters"    add evil "$(url greet)@v1;touch"
check "refused adds leave swarm.json unchanged" cmp -s swarm.json "$WORK/manifest.before"
# The same values written straight into the manifest are refused by install.
for bad_dep in \
    '"x;touch pwned": {"git": "URL"}' \
    '"evil": {"git": "--upload-pack=touch CANARY"}' \
    '"evil": {"git": "ext::sh -c touch% CANARY"}' \
    '"evil": {"git": "URL", "ref": "--output=CANARY"}' \
    '"evil": {"git": "URL", "path": "."}'; do
    dep="${bad_dep//URL/$(url greet)}"; dep="${dep//CANARY/$CANARY}"
    printf '{"name": "app", "version": "0.1.0", "deps": {%s}}\n' "$dep" > swarm.json
    refuse "manifest entry refused: $bad_dep" install
done
cp "$WORK/manifest.before" swarm.json
check "no injected command ever ran" test ! -e "$CANARY"

# 13. Missing git → a clear error.
rm -rf .swarm/deps/greet
out="$(PATH=/nonexistent "$SWC" install 2>&1)"
if [ $? -ne 0 ] && grep -q "git not found in PATH" <<<"$out"; then ok "missing git is a clear error"
else bad "missing git is a clear error" "$out"; fi
check "install restores after git returns" "$SWC" install

# 14. .swarm/ is ignored in this repo.
check ".swarm/ is in .gitignore" grep -qx '.swarm/' "$SWARMRT_ROOT/.gitignore"

echo
if [ "$FAIL" -eq 0 ]; then
    echo "all pkg tests passed — $PASS checks"
    exit 0
fi
echo "pkg tests: $FAIL of $((PASS + FAIL)) checks FAILED"
exit 1
