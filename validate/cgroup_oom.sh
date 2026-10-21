#!/usr/bin/env bash
# cgroup_oom.sh —— 在真实 cgroup v2 限额里跑 oomcheck 的 bounded 模式
#
# 判定的是“限额内不 OOM、peak 不越过 max、退出后 cgroup 记账正常”。
# 拿不到 cgroup 权限时返回 77;设了 REQUIRE_CGROUP=1 则返回失败,
# 不把“没测”伪装成“通过”。
#
# 两种落地方式:
#   direct  —— root 在 /sys/fs/cgroup 下建叶子,把探针 PID 写进
#              cgroup.procs。能精确读叶子的 memory.events/peak。
#   systemd —— systemd-run --scope 临时 scope,探针自己报告
#              /proc/self/cgroup 对应的 max/current/peak/oom_kill。
#
# --expect-oom 是门禁自检:探针申请 2 倍 max 并整块触碰,内核应该
# SIGKILL 它,且叶子 oom_kill 至少为 1。它只在 direct 模式可用。

set -u

PROBE=""
LIVE_MB=64
MAX_MB=192
HIGH_MB=96
SECONDS_RUN=5
EXPECT_OOM=0
SEED=""
KEEP=0

LEAF=""
LEAF_CREATED=0
PROBE_LOG=""
TIMEOUT_FLAG=""
WATCH_PID=""
PROBE_TIMED_OUT=0

say()  { printf 'CGROUP: %s\n' "$*"; }
fail() { printf 'CGROUP: FAIL %s\n' "$*"; g_fail=$((g_fail + 1)); }
g_fail=0

usage() {
    cat >&2 <<EOF
usage: $0 --probe PATH [--live-mb N] [--max-mb N] [--high-mb N]
          [--seconds N] [--seed N] [--expect-oom] [--keep]

  --probe PATH    oomcheck 可执行文件(必须已编译)
  --live-mb N     bounded 模式的存活上限,默认 64
  --max-mb N      memory.max,默认 192(必须大于 live-mb)
  --high-mb N     memory.high,默认 96;0 表示不写
  --seconds N     bounded 运行时长,默认 5
  --seed N        传给探针的随机种子
  --expect-oom    自检:申请 2 倍 max,预期被 SIGKILL
  --keep          保留叶子 cgroup 和探针日志,便于排障

环境:
  REQUIRE_CGROUP=1  77(SKIP)视为失败
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --probe)
            [ $# -ge 2 ] || { usage; exit 2; }
            PROBE=$2; shift 2 ;;
        --live-mb)
            [ $# -ge 2 ] || { usage; exit 2; }
            LIVE_MB=$2; shift 2 ;;
        --max-mb)
            [ $# -ge 2 ] || { usage; exit 2; }
            MAX_MB=$2; shift 2 ;;
        --high-mb)
            [ $# -ge 2 ] || { usage; exit 2; }
            HIGH_MB=$2; shift 2 ;;
        --seconds)
            [ $# -ge 2 ] || { usage; exit 2; }
            SECONDS_RUN=$2; shift 2 ;;
        --seed)
            [ $# -ge 2 ] || { usage; exit 2; }
            SEED=$2; shift 2 ;;
        --expect-oom)
            EXPECT_OOM=1; shift ;;
        --keep)
            KEEP=1; shift ;;
        --help|-h)
            usage; exit 0 ;;
        *)
            printf 'cgroup_oom.sh: unknown option %s\n' "$1" >&2
            usage; exit 2 ;;
    esac
done

is_uint() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

for pair in "live-mb:$LIVE_MB" "max-mb:$MAX_MB" "high-mb:$HIGH_MB" \
            "seconds:$SECONDS_RUN"; do
    name=${pair%%:*}
    val=${pair#*:}
    is_uint "$val" || { printf 'cgroup_oom.sh: --%s must be an integer\n' \
                        "$name" >&2; exit 2; }
done
[ -n "$PROBE" ] && [ -x "$PROBE" ] || {
    printf 'cgroup_oom.sh: --probe must name an executable\n' >&2
    exit 2
}
[ "$MAX_MB" -gt 0 ] || { printf 'cgroup_oom.sh: --max-mb must be > 0\n' >&2; exit 2; }
[ "$HIGH_MB" -le "$MAX_MB" ] || {
    printf 'cgroup_oom.sh: --high-mb must not exceed --max-mb\n' >&2
    exit 2
}
if [ "$EXPECT_OOM" -eq 0 ]; then
    [ "$LIVE_MB" -lt "$MAX_MB" ] || {
        printf 'cgroup_oom.sh: --live-mb must be below --max-mb\n' >&2
        exit 2
    }
else
    [ $((MAX_MB * 2)) -le 32768 ] || {
        printf 'cgroup_oom.sh: expect-oom needs --max-mb <= 16384\n' >&2
        exit 2
    }
fi

PROBE_LOG=$(mktemp "${TMPDIR:-/tmp}/libwmm-cgroup.XXXXXX") || exit 2
TIMEOUT_FLAG="${PROBE_LOG}.timeout"

cleanup() {
    if [ -n "$WATCH_PID" ]; then
        kill -KILL "$WATCH_PID" 2>/dev/null
        WATCH_PID=""
    fi
    if [ "$LEAF_CREATED" -eq 1 ] && [ -d "$LEAF" ]; then
        if [ -r "$LEAF/cgroup.procs" ]; then
            while read -r p; do
                [ -n "$p" ] && kill -KILL "$p" 2>/dev/null
            done < "$LEAF/cgroup.procs"
        fi
        if [ "$KEEP" -eq 0 ]; then
            for _ in 1 2 3 4 5 6 7 8 9 10; do
                rmdir "$LEAF" 2>/dev/null && break
                sleep 0.05
            done
            [ -d "$LEAF" ] && say "warning: cgroup leaf $LEAF is still present"
        else
            say "keeping leaf $LEAF"
        fi
    fi
    if [ "$KEEP" -eq 0 ] && [ -n "$PROBE_LOG" ] && [ -f "$PROBE_LOG" ]; then
        rm -f "$PROBE_LOG" "$TIMEOUT_FLAG"
    elif [ -n "$PROBE_LOG" ]; then
        say "probe log kept at $PROBE_LOG"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

skip() {
    say "SKIP $*"
    if [ "${REQUIRE_CGROUP:-0}" = "1" ]; then
        say "REQUIRE_CGROUP=1, treating SKIP as failure"
        exit 1
    fi
    exit 77
}

read_file() {
    [ -r "$1" ] || { printf 'na'; return; }
    tr -d '\n' < "$1"
}

events_value() {
    local file=$1 key=$2
    [ -r "$file" ] || { printf 'na'; return; }
    awk -v k="$key" '$1 == k { print $2; found = 1; exit }
                      END { if (!found) print "na" }' "$file"
}

# 探针在叶子内后台运行。cgroup.procs 写失败时 sh 以 125 退出、不会
# exec 探针,所以 125 就是“没进 cgroup”的确定信号;其他退出码说明
# 至少已经完成迁移。正常模式再用探针自己打印的 path 做交叉验证。
run_in_leaf() {
    local watchdog=$1
    shift
    local pid rc

    : > "$PROBE_LOG"
    rm -f "$TIMEOUT_FLAG"
    PROBE_TIMED_OUT=0

    sh -c 'echo $$ > "$1/cgroup.procs" || exit 125; shift; exec "$@"' \
        sh "$LEAF" "$PROBE" "$@" >"$PROBE_LOG" 2>&1 &
    pid=$!

    ( sleep "$watchdog"
      : > "$TIMEOUT_FLAG"
      kill -KILL "$pid" 2>/dev/null ) &
    WATCH_PID=$!

    wait "$pid"
    rc=$?

    kill -KILL "$WATCH_PID" 2>/dev/null
    wait "$WATCH_PID" 2>/dev/null
    WATCH_PID=""

    [ -f "$TIMEOUT_FLAG" ] && PROBE_TIMED_OUT=1

    cat "$PROBE_LOG"
    return "$rc"
}

# 返回 0 表示测试确实跑过(结果看 g_fail),1 表示 direct 模式不可用。
run_direct() {
    local root=/sys/fs/cgroup
    local maxb=$((MAX_MB * 1024 * 1024))
    local highb=$((HIGH_MB * 1024 * 1024))
    local why=""

    [ -d "$root" ] || { say "no $root"; return 1; }
    [ -f "$root/cgroup.controllers" ] || {
        say "cgroup v2 not mounted at $root"; return 1; }
    if ! grep -qw memory "$root/cgroup.controllers" 2>/dev/null; then
        say "memory controller not available at $root"
        return 1
    fi
    if ! grep -qw memory "$root/cgroup.subtree_control" 2>/dev/null; then
        if ! printf '+memory\n' > "$root/cgroup.subtree_control" 2>/dev/null; then
            say "cannot enable memory in $root/cgroup.subtree_control"
            return 1
        fi
    fi

    LEAF="$root/libwmm-oom-$$"
    if ! mkdir "$LEAF" 2>/dev/null; then
        LEAF="$root/libwmm-oom-$$-$RANDOM"
        if ! mkdir "$LEAF" 2>/dev/null; then
            say "cannot create a cgroup leaf under $root"
            LEAF=""
            return 1
        fi
    fi
    LEAF_CREATED=1

    if ! printf '%s\n' "$maxb" > "$LEAF/memory.max" 2>/dev/null; then
        why="cannot write $LEAF/memory.max"
        say "$why"
        return 1
    fi
    if [ "$HIGH_MB" -gt 0 ]; then
        if ! printf '%s\n' "$highb" > "$LEAF/memory.high" 2>/dev/null; then
            say "warning: cannot write memory.high (continuing)"
        fi
    fi
    if ! printf '0\n' > "$LEAF/memory.swap.max" 2>/dev/null; then
        say "warning: cannot set memory.swap.max=0"
    fi
    if ! printf '1\n' > "$LEAF/memory.oom.group" 2>/dev/null; then
        say "warning: cannot set memory.oom.group=1"
    fi

    say "mode=direct leaf=$LEAF max=${MAX_MB}M high=${HIGH_MB}M " \
        "live=${LIVE_MB}M seconds=${SECONDS_RUN} expect_oom=$EXPECT_OOM"

    local rc=0
    if [ "$EXPECT_OOM" -eq 1 ]; then
        # 申请 2 倍 max,整块触碰;不设 duration,靠内核在触碰过程中杀进程。
        local eargs=(--bounded --live-mb $((MAX_MB * 2)) --seconds 30)
        [ -n "$SEED" ] && eargs+=(--seed "$SEED")
        run_in_leaf 60 "${eargs[@]}"
        rc=$?
    else
        local args=(--bounded --live-mb "$LIVE_MB" --seconds "$SECONDS_RUN")
        [ -n "$SEED" ] && args+=(--seed "$SEED")
        run_in_leaf $((SECONDS_RUN + 60)) "${args[@]}"
        rc=$?
    fi

    local events="$LEAF/memory.events"
    local kills peak cur maxv
    kills=$(events_value "$events" oom_kill)
    peak=$(read_file "$LEAF/memory.peak")
    cur=$(read_file "$LEAF/memory.current")
    maxv=$(read_file "$LEAF/memory.max")

    say "mode=direct max=$maxv current=$cur peak=$peak oom_kill=$kills"

    if [ "$rc" -eq 125 ]; then
        fail "probe could not be placed into $LEAF (cgroup.procs write failed)"
    fi
    if [ "$PROBE_TIMED_OUT" -eq 1 ]; then
        fail "probe was still running when the watchdog fired"
    fi

    if [ "$EXPECT_OOM" -eq 1 ]; then
        if [ "$rc" -ne 137 ]; then
            fail "expect-oom: probe did not die from SIGKILL (rc=$rc)"
        fi
        if [ "$kills" = "na" ]; then
            fail "expect-oom: memory.events has no oom_kill"
        elif [ "$kills" -lt 1 ]; then
            fail "expect-oom: oom_kill=$kills, expected at least 1"
        fi
    else
        if [ "$rc" -ne 0 ]; then
            fail "probe exited rc=$rc"
        fi
        if ! grep -q 'OOM_RESULT: PASS' "$PROBE_LOG"; then
            fail "probe did not report OOM_RESULT: PASS"
        fi

        local got want
        got=$(sed -n 's/^CGROUP: path=\([^ ]*\).*/\1/p' "$PROBE_LOG" | head -n1)
        want=${LEAF#/sys/fs/cgroup}
        if [ "$got" != "$want" ]; then
            fail "probe cgroup path='$got', expected '$want'"
        fi

        if [ "$kills" = "na" ]; then
            fail "memory.events is unreadable or has no oom_kill"
        elif [ "$kills" -ne 0 ]; then
            fail "oom_kill=$kills in a limit that should not be reached"
        fi
        if [ "$maxv" != "$maxb" ]; then
            fail "memory.max=$maxv, expected $maxb"
        fi
        if [ "$peak" != "na" ] && is_uint "$peak"; then
            if [ "$peak" -gt "$maxb" ]; then
                fail "memory.peak=$peak exceeded memory.max=$maxb"
            fi
        else
            say "memory.peak unavailable; peak gate not applied"
        fi
    fi

    return 0
}

# 返回 0 表示测试确实跑过(结果看 g_fail),1 表示 systemd 方式不可用。
run_systemd() {
    command -v systemd-run >/dev/null 2>&1 || {
        say "systemd-run not found"; return 1; }
    if [ "$EXPECT_OOM" -eq 1 ]; then
        say "expect-oom needs direct cgroup mode (systemd scope events "
        return 1
    fi

    local props=(-p "MemoryMax=${MAX_MB}M" -p "MemorySwapMax=0")
    [ "$HIGH_MB" -gt 0 ] && props+=(-p "MemoryHigh=${HIGH_MB}M")

    local args=(--bounded --live-mb "$LIVE_MB" --seconds "$SECONDS_RUN")
    [ -n "$SEED" ] && args+=(--seed "$SEED")

    say "mode=systemd max=${MAX_MB}M high=${HIGH_MB}M " \
        "live=${LIVE_MB}M seconds=${SECONDS_RUN}"

    local out rc
    out=$(timeout --signal=KILL $((SECONDS_RUN + 90)) \
          systemd-run --scope --quiet --wait --collect "${props[@]}" \
          -- "$PROBE" "${args[@]}" 2>&1)
    rc=$?
    printf '%s\n' "$out"

    if ! printf '%s\n' "$out" | grep -q '^CGROUP: '; then
        if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
            fail "probe under systemd-run did not finish (rc=$rc)"
            return 0
        fi
        say "systemd-run did not start the probe under a scoped cgroup"
        return 1
    fi

    local maxv cur peak kills
    maxv=$(printf '%s\n' "$out" | sed -n 's/.* max=\([^ ]*\).*/\1/p' | tail -n1)
    cur=$(printf '%s\n' "$out" | sed -n 's/.* current=\([^ ]*\).*/\1/p' | tail -n1)
    peak=$(printf '%s\n' "$out" | sed -n 's/.* peak=\([^ ]*\).*/\1/p' | tail -n1)
    kills=$(printf '%s\n' "$out" | sed -n 's/.* oom_kill=\([^ ]*\).*/\1/p' | tail -n1)

    say "mode=systemd max=$maxv current=$cur peak=$peak oom_kill=$kills"

    if [ "$rc" -ne 0 ]; then
        fail "probe exited rc=$rc"
    fi
    if ! printf '%s\n' "$out" | grep -q 'OOM_RESULT: PASS'; then
        fail "probe did not report OOM_RESULT: PASS"
    fi
    if [ "$maxv" = "max" ] || [ "$maxv" = "na" ] || [ -z "$maxv" ]; then
        fail "MemoryMax was not applied (max=$maxv)"
    fi
    if [ "$kills" = "na" ] || [ -z "$kills" ]; then
        fail "probe could not read oom_kill"
    elif [ "$kills" -ne 0 ]; then
        fail "oom_kill=$kills in a limit that should not be reached"
    fi
    if is_uint "$peak" && is_uint "$maxv"; then
        if [ "$peak" -gt "$maxv" ]; then
            fail "memory.peak=$peak exceeded memory.max=$maxv"
        fi
    else
        say "memory.peak unavailable; peak gate not applied"
    fi
    return 0
}

if run_direct; then
    :
elif run_systemd; then
    :
else
    skip "cgroup v2 with memory controller is not writable and " \
         "systemd-run is unavailable"
fi

if [ "$g_fail" -eq 0 ]; then
    printf 'CGROUP_RESULT: PASS failures=0\n'
    exit 0
fi

printf 'CGROUP_RESULT: FAIL failures=%d\n' "$g_fail"
exit 1
