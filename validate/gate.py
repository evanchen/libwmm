#!/usr/bin/env python3
"""gate.py —— 生产验证编排与门禁

每个被测量进程都独立启动(会话/进程组隔离、独立超时),避免基准
之间通过分配器残留状态互相污染。判定分成硬门禁与只报告两类:

  硬门禁  bench 退出码与 alloc_fail、server p999 尾延迟上限、
          soak/forkstress/oomcheck 的 PASS 行、cgroup 结果。
  只报告  吞吐比、p99 比、p9999/max;--strict-perf 可把吞吐比
          也纳入硬门禁。

cgroup 不可用时记 SKIP;只有 --require-cgroup 才把它当失败。
任何编排错误(缺二进制、参数非法、报告写不出)退出 2,与门禁失败
(退出 1)区分开。
"""

import argparse
import csv
import json
import os
import platform
import re
import shlex
import signal
import socket
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

SCHEMA_VERSION = 1

BENCH_FIELDS = [
    "impl", "scenario", "ops", "seconds", "ops_per_sec", "p50_ns",
    "p99_ns", "rss_kb", "p999_ns", "p9999_ns", "max_ns", "reqs",
    "req_per_sec", "alloc_fail",
]
BENCH_SCENARIOS = ["seq", "mt", "mixed", "large", "server"]
BENCH_IMPLS = ["wmm", "glibc"]

# --strict-perf 使用的最低吞吐比(wmm / glibc)。第一次基线不知道
# 单 arena 在多线程下的真实位置,阈值取保守值;后续按报告收紧。
STRICT_MIN_RATIO = {
    "seq": 0.60,
    "mt": 0.20,
    "mixed": 0.60,
    "large": 0.60,
    "server": 0.40,
}

SOAK_FIELDS = [
    "elapsed_s", "phase", "live_bytes", "active_bytes", "frag_bytes",
    "high_water_live", "live_objects", "rss_kb", "cached_objs", "ops",
    "guard_events", "leak_events", "oom_events", "purge_events",
    "other_events",
]


class GateError(Exception):
    """编排错误:退出码 2,不是被测代码失败。"""


class Gates:
    def __init__(self):
        self.items = []

    def add(self, name, status, detail=""):
        status = status.upper()
        if status not in ("PASS", "FAIL", "SKIP"):
            raise GateError(f"bad gate status {status}")
        self.items.append({"name": name, "status": status,
                           "detail": detail})
        print(f"{status:4s} {name} {detail}".rstrip(), flush=True)

    def failed(self):
        return any(g["status"] == "FAIL" for g in self.items)


def now_iso():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def host_info():
    info = {
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
    }
    try:
        info["cpu_count"] = os.cpu_count()
    except Exception:
        info["cpu_count"] = None
    try:
        info["uname"] = " ".join(os.uname())
    except Exception:
        pass
    return info


def ensure_executable(path, label):
    p = Path(path).expanduser().resolve()
    if not p.is_file():
        raise GateError(f"{label} not found: {p}")
    if not os.access(p, os.X_OK):
        raise GateError(f"{label} is not executable: {p}")
    return str(p)


def open_log(path):
    try:
        return open(path, "a", encoding="utf-8")
    except OSError as exc:
        raise GateError(f"cannot open log {path}: {exc}")


def kill_group(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except (ProcessLookupError, PermissionError, OSError):
        try:
            proc.kill()
        except OSError:
            pass


def run_process(cmd, log_path, timeout_s, env=None, cwd=None):
    """独立会话运行;超时杀整个进程组。返回 (rc, output, timed_out)。"""
    log = open_log(log_path)
    try:
        log.write("$ " + " ".join(shlex.quote(x) for x in cmd) + "\n")
        log.flush()
        try:
            proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                start_new_session=True, env=env, cwd=cwd,
            )
        except OSError as exc:
            log.write(f"spawn failed: {exc}\n")
            log.close()
            raise GateError(f"cannot start {cmd[0]}: {exc}")

        timed_out = False
        try:
            out, _ = proc.communicate(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            timed_out = True
            kill_group(proc)
            try:
                out, _ = proc.communicate(timeout=20)
            except subprocess.TimeoutExpired:
                out = b""
        text = (out or b"").decode("utf-8", errors="replace")
        log.write(text)
        if not text.endswith("\n"):
            log.write("\n")
        log.write(f"# rc={proc.returncode} timeout={int(timed_out)}\n\n")
        log.flush()
        return proc.returncode, text, timed_out
    finally:
        if not log.closed:
            log.close()


def parse_bench_row(text, impl, scenario):
    for line in reversed(text.splitlines()):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(",")
        if fields[0] == "impl":
            continue
        if len(fields) != len(BENCH_FIELDS):
            if len(fields) == 8:
                raise GateError(
                    "bench CSV has 8 columns; rebuild bench with the "
                    "server/tail-latency fields")
            continue
        try:
            row = {"impl": fields[0], "scenario": fields[1]}
            for name, raw in zip(BENCH_FIELDS[2:], fields[2:]):
                if name in ("seconds", "ops_per_sec", "req_per_sec"):
                    row[name] = float(raw)
                else:
                    row[name] = int(raw)
        except ValueError:
            continue
        if row["impl"] != impl or row["scenario"] != scenario:
            continue
        return row
    return None


def merge_bench_rows(rows):
    merged = {"impl": rows[0]["impl"], "scenario": rows[0]["scenario"]}
    numeric = [f for f in BENCH_FIELDS if f not in ("impl", "scenario")]
    for name in numeric:
        vals = sorted(r[name] for r in rows)
        merged[name] = vals[len(vals) // 2]
    merged["alloc_fail"] = max(r["alloc_fail"] for r in rows)
    return merged


def fmt_bench_field(name, value):
    if name in ("impl", "scenario"):
        return str(value)
    if name == "seconds":
        return f"{value:.6f}"
    if name in ("ops_per_sec", "req_per_sec"):
        return f"{value:.0f}"
    return str(int(value))


def write_bench_csv(path, rows):
    try:
        with open(path, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(BENCH_FIELDS)
            for row in rows:
                w.writerow([fmt_bench_field(n, row[n]) for n in BENCH_FIELDS])
    except OSError as exc:
        raise GateError(f"cannot write {path}: {exc}")


def last_result_line(text, needle):
    found = ""
    for line in text.splitlines():
        if needle in line:
            found = line.strip()
    return found


def parse_kv(line, key):
    m = re.search(r"(?:^|\s)" + re.escape(key) + r"=([^\s]+)", line)
    return m.group(1) if m else None


def parse_soak_csv(path):
    if not os.path.isfile(path):
        return None, []
    rows = []
    try:
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            for parts in csv.reader(f):
                if not parts:
                    continue
                if parts[0] == SOAK_FIELDS[0]:
                    continue
                if len(parts) < len(SOAK_FIELDS):
                    continue
                try:
                    row = {"phase": parts[1]}
                    for name, raw in zip(SOAK_FIELDS, parts):
                        if name != "phase":
                            row[name] = float(raw)
                except ValueError:
                    continue
                rows.append(row)
    except OSError:
        return None, []
    return (rows[-1] if rows else None), rows


def check_range(name, value, lo, hi):
    if value < lo or value > hi:
        raise GateError(f"{name} must be in [{lo}, {hi}]")


def run_bench(args, out_dir, threads, env, gates, bench_log):
    rows_by_key = {}
    for scenario in BENCH_SCENARIOS:
        for impl in BENCH_IMPLS:
            key = (impl, scenario)
            ok_rows = []
            rcs = []
            parse_error = ""
            for run_idx in range(args.repeats):
                cmd = [args.bench_bin, impl, "--scenario", scenario,
                       "--threads", str(threads),
                       "--requests", str(args.bench_requests),
                       "--no-header"] + args.bench_extra
                print(f"[gate] bench {impl}/{scenario} "
                      f"run {run_idx + 1}/{args.repeats}", flush=True)
                rc, text, timed_out = run_process(
                    cmd, bench_log, 300, env=env)
                rcs.append(rc)
                row = None
                if not timed_out and rc == 0:
                    try:
                        row = parse_bench_row(text, impl, scenario)
                    except GateError as exc:
                        parse_error = str(exc)
                if row is not None:
                    ok_rows.append(row)
                elif not parse_error:
                    tail = text.strip().splitlines()[-1:] or ["<no output>"]
                    parse_error = f"no parsable {scenario} row: {tail[0]}"

            if len(ok_rows) == args.repeats:
                gates.add(f"bench:{impl}:{scenario}:exit", "PASS",
                          f"runs={args.repeats}")
            else:
                detail = (f"runs={args.repeats} rc={rcs}"
                          + (f" ({parse_error})" if parse_error else ""))
                gates.add(f"bench:{impl}:{scenario}:exit", "FAIL", detail)

            if ok_rows:
                merged = merge_bench_rows(ok_rows)
                rows_by_key[key] = merged
                if merged["alloc_fail"] == 0:
                    gates.add(f"bench:{impl}:{scenario}:alloc_fail",
                              "PASS", "alloc_fail=0")
                else:
                    gates.add(f"bench:{impl}:{scenario}:alloc_fail",
                              "FAIL",
                              f"alloc_fail={merged['alloc_fail']}")
            else:
                gates.add(f"bench:{impl}:{scenario}:alloc_fail", "FAIL",
                          "no successful run")

    return rows_by_key


def report_bench_comparison(rows_by_key, args, gates):
    comparison = {}
    for scenario in BENCH_SCENARIOS:
        wmm = rows_by_key.get(("wmm", scenario))
        glibc = rows_by_key.get(("glibc", scenario))
        if not wmm or not glibc:
            continue
        w_ops = wmm["ops_per_sec"]
        g_ops = glibc["ops_per_sec"]
        w_req = wmm["req_per_sec"]
        g_req = glibc["req_per_sec"]
        entry = {
            "wmm_ops_per_sec": w_ops,
            "glibc_ops_per_sec": g_ops,
            "ops_ratio": (w_ops / g_ops) if g_ops > 0 else None,
            "wmm_p50_ns": wmm["p50_ns"],
            "glibc_p50_ns": glibc["p50_ns"],
            "wmm_p99_ns": wmm["p99_ns"],
            "glibc_p99_ns": glibc["p99_ns"],
            "wmm_p999_ns": wmm["p999_ns"],
            "glibc_p999_ns": glibc["p999_ns"],
            "wmm_p9999_ns": wmm["p9999_ns"],
            "glibc_p9999_ns": glibc["p9999_ns"],
            "wmm_max_ns": wmm["max_ns"],
            "glibc_max_ns": glibc["max_ns"],
        }
        if scenario == "server":
            entry["wmm_req_per_sec"] = w_req
            entry["glibc_req_per_sec"] = g_req
            entry["req_ratio"] = (w_req / g_req) if g_req > 0 else None
        comparison[scenario] = entry

    server = rows_by_key.get(("wmm", "server"))
    glibc_server = rows_by_key.get(("glibc", "server"))
    if server and glibc_server and server["p999_ns"] >= 0:
        budget_ns = args.p999_budget_ms * 1e6
        limit_ns = max(budget_ns,
                       args.p999_ratio * glibc_server["p999_ns"])
        used = server["p999_ns"]
        detail = (f"wmm_p999_ns={used} limit_ns={limit_ns:.0f} "
                  f"glibc_p999_ns={glibc_server['p999_ns']} "
                  f"budget_ms={args.p999_budget_ms:g} "
                  f"ratio={args.p999_ratio:g}")
        gates.add("bench:server:p999",
                  "PASS" if used <= limit_ns else "FAIL", detail)
    else:
        gates.add("bench:server:p999", "FAIL",
                  "wmm or glibc server row missing")

    if args.strict_perf:
        for scenario in BENCH_SCENARIOS:
            entry = comparison.get(scenario)
            if not entry:
                gates.add(f"strict:{scenario}:throughput", "FAIL",
                          "comparison row missing")
                continue
            if scenario == "server":
                ratio = entry.get("req_ratio")
                metric = "req_per_sec"
            else:
                ratio = entry.get("ops_ratio")
                metric = "ops_per_sec"
            need = STRICT_MIN_RATIO[scenario]
            shown = "n/a" if ratio is None else f"{ratio:.3f}"
            detail = f"{metric} ratio={shown} min={need:.3f}"
            gates.add(f"strict:{scenario}:throughput",
                      "PASS" if ratio is not None and ratio >= need
                      else "FAIL", detail)

    return comparison


def run_soak(args, out_dir, env, gates):
    log_path = out_dir / "soak.log"
    csv_path = out_dir / "soak.csv"
    try:
        if csv_path.exists():
            csv_path.unlink()
    except OSError as exc:
        raise GateError(f"cannot remove old {csv_path}: {exc}")

    cmd = [args.soak_bin, "--seconds", str(args.soak_seconds),
           "--threads", str(args.soak_threads), "--csv", str(csv_path)] \
        + args.soak_extra
    print(f"[gate] soak {args.soak_seconds}s threads={args.soak_threads}",
          flush=True)
    rc, text, timed_out = run_process(
        cmd, log_path, args.soak_seconds + 120, env=env)
    line = last_result_line(text, "SOAK_RESULT:")
    if timed_out:
        gates.add("soak:exit", "FAIL",
                  f"timed out after {args.soak_seconds + 120}s")
    else:
        gates.add("soak:exit", "PASS" if rc == 0 else "FAIL", f"rc={rc}")
    gates.add("soak:result",
              "PASS" if "SOAK_RESULT: PASS" in line else "FAIL",
              line or "no SOAK_RESULT line")

    last, all_rows = parse_soak_csv(csv_path)
    gates.add("soak:csv",
              "PASS" if last is not None else "FAIL",
              f"rows={len(all_rows)}" if last is not None
              else f"missing or unparsable {csv_path}")
    summary = {"result_line": line, "rows": len(all_rows),
               "last": last}
    if all_rows:
        summary["peak_rss_kb"] = max(int(r["rss_kb"]) for r in all_rows)
        summary["max_live_bytes"] = max(int(r["live_bytes"])
                                        for r in all_rows)
        summary["max_cached_objs"] = max(int(r["cached_objs"])
                                          for r in all_rows)
        summary["total_guard_events"] = int(all_rows[-1]["guard_events"])
        summary["total_oom_events"] = int(all_rows[-1]["oom_events"])
        summary["total_other_events"] = int(all_rows[-1]["other_events"])
    return summary


def run_fork(args, out_dir, env, gates, guard):
    if guard:
        seconds = args.fork_guard_seconds
        log_path = out_dir / "forkstress_guard.log"
        cmd = [args.fork_bin, "--seconds", str(seconds), "--guard"] \
            + args.fork_extra
        label = "forkstress:guard"
    else:
        seconds = args.fork_seconds
        log_path = out_dir / "forkstress.log"
        cmd = [args.fork_bin, "--seconds", str(seconds)] + args.fork_extra
        label = "forkstress"

    print(f"[gate] {label} {seconds}s", flush=True)
    rc, text, timed_out = run_process(
        cmd, log_path, seconds + 90, env=env)
    line = last_result_line(text, "FORKSTRESS_RESULT:")
    if timed_out:
        gates.add(f"{label}:exit", "FAIL",
                  f"timed out after {seconds + 90}s")
    else:
        gates.add(f"{label}:exit", "PASS" if rc == 0 else "FAIL", f"rc={rc}")
    gates.add(f"{label}:result",
              "PASS" if "FORKSTRESS_RESULT: PASS" in line else "FAIL",
              line or "no FORKSTRESS_RESULT line")
    return {"result_line": line}


def last_oom_error(text):
    for line in reversed(text.splitlines()):
        s = line.strip()
        if s.startswith("oomcheck:"):
            return s
    return ""


def run_oom(args, out_dir, env, gates):
    log_path = out_dir / "oom.log"
    cmd = [args.oom_bin, "--window-fill", "--abort-child"] \
        + args.oom_extra
    print("[gate] oomcheck window fill + abort child", flush=True)
    rc, text, timed_out = run_process(cmd, log_path, 180, env=env)
    result = last_result_line(text, "OOM_RESULT:")
    abort = last_result_line(text, "OOM_ABORT:")

    if timed_out:
        gates.add("oom:exit", "FAIL", "timed out after 180s")
    else:
        gates.add("oom:exit", "PASS" if rc == 0 else "FAIL", f"rc={rc}")
    detail = result or "no OOM_RESULT line"
    if "OOM_RESULT: PASS" not in result:
        err = last_oom_error(text)
        if err:
            detail += f" | {err}"
    gates.add("oom:result",
              "PASS" if "OOM_RESULT: PASS" in result else "FAIL",
              detail)
    gates.add("oom:abort",
              "PASS" if "OOM_ABORT: PASS" in abort else "FAIL",
              abort or "no OOM_ABORT line")
    return {"result_line": result, "abort_line": abort}


def run_cgroup(args, out_dir, oom_bin, env, gates):
    if args.no_cgroup:
        gates.add("cgroup", "SKIP", "--no-cgroup")
        return {"status": "SKIP", "reason": "--no-cgroup"}

    script = Path(args.cgroup_script).expanduser().resolve()
    if not script.is_file():
        raise GateError(f"cgroup script not found: {script}")

    log_path = out_dir / "cgroup.log"
    cmd = ["bash", str(script), "--probe", oom_bin,
           "--live-mb", str(args.cgroup_live_mb),
           "--max-mb", str(args.cgroup_max_mb),
           "--high-mb", str(args.cgroup_high_mb),
           "--seconds", str(args.cgroup_seconds)] + args.cgroup_extra
    run_env = dict(env)
    if args.require_cgroup:
        run_env["REQUIRE_CGROUP"] = "1"

    print("[gate] cgroup v2 limit", flush=True)
    rc, text, timed_out = run_process(cmd, log_path, 240, env=run_env)
    line = last_result_line(text, "CGROUP_RESULT:")
    if not line:
        line = last_result_line(text, "CGROUP: SKIP") or \
               last_result_line(text, "CGROUP: FAIL") or ""
    if timed_out:
        gates.add("cgroup", "FAIL", "timed out after 240s")
        return {"status": "FAIL", "result_line": "timeout"}
    if rc == 77 and not args.require_cgroup:
        gates.add("cgroup", "SKIP", line or "cgroup unavailable")
        return {"status": "SKIP", "result_line": line}
    if rc == 0 and "CGROUP_RESULT: PASS" in line:
        gates.add("cgroup", "PASS", line)
        return {"status": "PASS", "result_line": line}
    gates.add("cgroup", "FAIL", line or f"rc={rc}, no result line")
    return {"status": "FAIL", "result_line": line, "rc": rc}


def write_report(out_dir, report, gates):
    result = "FAIL" if gates.failed() else "PASS"
    report["result"] = result
    report["gates"] = gates.items

    json_path = out_dir / "report.json"
    try:
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2, sort_keys=False, ensure_ascii=False)
            f.write("\n")
    except OSError as exc:
        raise GateError(f"cannot write {json_path}: {exc}")

    txt_path = out_dir / "report.txt"
    try:
        with open(txt_path, "w", encoding="utf-8") as f:
            for g in gates.items:
                detail = f" {g['detail']}" if g["detail"] else ""
                f.write(f"{g['status']:4s} {g['name']}{detail}\n")
            f.write(f"RESULT: {result}\n")
    except OSError as exc:
        raise GateError(f"cannot write {txt_path}: {exc}")

    print(f"\nRESULT: {result}")
    print(f"report: {json_path}")
    print(f"report: {txt_path}")
    return result


def build_arg_parser():
    here = Path(__file__).resolve().parent
    root = here.parent

    p = argparse.ArgumentParser(
        description="libwmm production validation gate")
    p.add_argument("--bench-bin", required=True)
    p.add_argument("--soak-bin", required=True)
    p.add_argument("--fork-bin", required=True)
    p.add_argument("--oom-bin", required=True)
    p.add_argument("--out-dir", default=str(root / "build" / "validate"))
    p.add_argument("--cgroup-script", default=str(here / "cgroup_oom.sh"))

    p.add_argument("--bench-threads", type=int, default=None)
    p.add_argument("--bench-requests", type=int, default=200000)
    p.add_argument("--bench-args", default="",
                   help="extra arguments for every bench process")
    p.add_argument("--repeats", type=int, default=1)
    p.add_argument("--p999-budget-ms", type=float, default=10.0)
    p.add_argument("--p999-ratio", type=float, default=10.0)
    p.add_argument("--strict-perf", action="store_true")

    p.add_argument("--soak-seconds", type=int, default=60)
    p.add_argument("--soak-threads", type=int, default=4)
    p.add_argument("--soak-args", default="")

    p.add_argument("--fork-seconds", type=int, default=10)
    p.add_argument("--fork-guard-seconds", type=int, default=3)
    p.add_argument("--fork-args", default="")

    p.add_argument("--oom-args", default="")

    p.add_argument("--no-cgroup", action="store_true")
    p.add_argument("--require-cgroup", action="store_true")
    p.add_argument("--cgroup-live-mb", type=int, default=64)
    p.add_argument("--cgroup-max-mb", type=int, default=192)
    p.add_argument("--cgroup-high-mb", type=int, default=96)
    p.add_argument("--cgroup-seconds", type=int, default=5)
    p.add_argument("--cgroup-args", default="")
    return p


def main(argv):
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    try:
        check_range("--bench-requests", args.bench_requests, 64, 20000000)
        check_range("--repeats", args.repeats, 1, 9)
        check_range("--soak-seconds", args.soak_seconds, 1, 86400)
        check_range("--soak-threads", args.soak_threads, 1, 64)
        check_range("--fork-seconds", args.fork_seconds, 1, 86400)
        check_range("--fork-guard-seconds",
                      args.fork_guard_seconds, 1, 86400)
        check_range("--cgroup-live-mb", args.cgroup_live_mb, 1, 32768)
        check_range("--cgroup-max-mb", args.cgroup_max_mb, 1, 32768)
        check_range("--cgroup-high-mb", args.cgroup_high_mb, 0, 32768)
        check_range("--cgroup-seconds", args.cgroup_seconds, 1, 3600)
        if args.cgroup_high_mb > args.cgroup_max_mb:
            raise GateError("--cgroup-high-mb must not exceed --cgroup-max-mb")
        if args.cgroup_live_mb >= args.cgroup_max_mb:
            raise GateError("--cgroup-live-mb must be below --cgroup-max-mb")
        if args.p999_budget_ms <= 0 or args.p999_ratio <= 0:
            raise GateError("p999 budget and ratio must be positive")
        if args.no_cgroup and args.require_cgroup:
            raise GateError("--no-cgroup conflicts with --require-cgroup")

        threads = args.bench_threads
        if threads is None:
            threads = min(8, os.cpu_count() or 1)
        check_range("--bench-threads", threads, 1, 64)

        args.bench_extra = shlex.split(args.bench_args)
        args.soak_extra = shlex.split(args.soak_args)
        args.fork_extra = shlex.split(args.fork_args)
        args.oom_extra = shlex.split(args.oom_args)
        args.cgroup_extra = shlex.split(args.cgroup_args)

        args.bench_bin = ensure_executable(args.bench_bin, "bench binary")
        args.soak_bin = ensure_executable(args.soak_bin, "soak binary")
        args.fork_bin = ensure_executable(args.fork_bin, "forkstress binary")
        args.oom_bin = ensure_executable(args.oom_bin, "oomcheck binary")

        out_dir = Path(args.out_dir).expanduser().resolve()
        try:
            out_dir.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            raise GateError(f"cannot create {out_dir}: {exc}")
    except GateError as exc:
        print(f"gate: {exc}", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env.setdefault("LC_ALL", "C")
    gates = Gates()
    bench_log = out_dir / "bench.log"
    try:
        if bench_log.exists():
            bench_log.unlink()
    except OSError as exc:
        print(f"gate: cannot remove old {bench_log}: {exc}", file=sys.stderr)
        return 2

    started = now_iso()
    report = {
        "schema": SCHEMA_VERSION,
        "generated_at": started,
        "host": host_info(),
        "params": {
            "bench_bin": args.bench_bin,
            "soak_bin": args.soak_bin,
            "fork_bin": args.fork_bin,
            "oom_bin": args.oom_bin,
            "out_dir": str(out_dir),
            "bench_threads": threads,
            "bench_requests": args.bench_requests,
            "repeats": args.repeats,
            "p999_budget_ms": args.p999_budget_ms,
            "p999_ratio": args.p999_ratio,
            "strict_perf": args.strict_perf,
            "soak_seconds": args.soak_seconds,
            "soak_threads": args.soak_threads,
            "fork_seconds": args.fork_seconds,
            "fork_guard_seconds": args.fork_guard_seconds,
            "cgroup": not args.no_cgroup,
            "require_cgroup": args.require_cgroup,
            "cgroup_live_mb": args.cgroup_live_mb,
            "cgroup_max_mb": args.cgroup_max_mb,
            "cgroup_high_mb": args.cgroup_high_mb,
            "cgroup_seconds": args.cgroup_seconds,
        },
    }

    try:
        rows_by_key = run_bench(args, out_dir, threads, env, gates,
                                str(bench_log))
        bench_rows = [rows_by_key[k] for k in
                      [(i, s) for s in BENCH_SCENARIOS for i in BENCH_IMPLS]
                      if k in rows_by_key]
        write_bench_csv(out_dir / "bench.csv", bench_rows)
        report["bench"] = {
            "rows": bench_rows,
            "comparison": report_bench_comparison(rows_by_key, args, gates),
        }

        report["soak"] = run_soak(args, out_dir, env, gates)
        report["forkstress"] = {
            "normal": run_fork(args, out_dir, env, gates, guard=False),
            "guard": run_fork(args, out_dir, env, gates, guard=True),
        }
        report["oomcheck"] = run_oom(args, out_dir, env, gates)
        report["cgroup"] = run_cgroup(args, out_dir, args.oom_bin, env,
                                      gates)
    except GateError as exc:
        print(f"gate: {exc}", file=sys.stderr)
        return 2

    try:
        result = write_report(out_dir, report, gates)
    except GateError as exc:
        print(f"gate: {exc}", file=sys.stderr)
        return 2
    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        print("gate: interrupted", file=sys.stderr)
        sys.exit(2)
