#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BIN="${ROOT}/checkpoint_batch"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

command -v cc >/dev/null 2>&1 || fail "C compiler cc is required"
make -C "$ROOT" clean all >/dev/null || fail "build failed"
cd "$WORK"

"$BIN" 80 10 0 >normal.log
grep -q 'COMPLETE steps=80' normal.log || fail "normal completion"
test -s batch.checkpoint || fail "checkpoint missing"
pass "periodic checkpoints"

rm -f batch.checkpoint batch.checkpoint.tmp
"$BIN" 2000 2000 1 >signal.log 2>&1 &
pid=$!
sleep 0.08
kill -USR1 "$pid" 2>/dev/null || true
wait "$pid" || fail "SIGUSR1 run"
grep -q '\[checkpoint\] committed' signal.log || fail "early checkpoint missing"
pass "SIGUSR1 deferred checkpoint"

rm -f batch.checkpoint batch.checkpoint.tmp
"$BIN" 2500 2500 1 >repeated.log 2>&1 &
pid=$!
sleep 0.05
for _ in 1 2 3 4 5; do kill -USR1 "$pid" 2>/dev/null || true; sleep 0.01; done
wait "$pid" || fail "repeated-signal run"
"$BIN" 2500 2500 0 >resume.log
grep -q 'valid checkpoint' resume.log || fail "checkpoint invalid after repeated signals"
pass "repeated signals"

cp batch.checkpoint saved.checkpoint
printf 'truncated-write' > batch.checkpoint.tmp
"$BIN" 2500 2500 0 >temp-recovery.log
grep -q 'valid checkpoint' temp-recovery.log || fail "temp file interfered with recovery"
cmp -s batch.checkpoint saved.checkpoint || fail "committed state changed due to orphan temp"
pass "orphaned interrupted temp write ignored"

printf 'not-a-valid-checkpoint' > batch.checkpoint
"$BIN" 30 10 0 >invalid.log
grep -q 'invalid/incompatible checkpoint' invalid.log || fail "invalid file not detected"
grep -q 'COMPLETE steps=30' invalid.log || fail "restart after invalid checkpoint failed"
pass "invalid checkpoint rejected and restarted"

echo "All checkpoint/resume tests passed."
