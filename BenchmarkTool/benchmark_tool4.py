#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
benchmark_tool4 (no-plot):
- 通过 --tasks 生成 N 个随机用例（每个用例 Cr∈[0,1]、contention∈[0,1] 独立抽样）
- 每个用例：generator3 -> 编译 -> 运行 -> 读取 delay_mean & global miss rate
- 输出：
  * cases.csv：逐用例记录
  * summary.csv：整体统计
  * histograms.csv：两个直方图的 bin 边界与计数（不画图）
"""

import argparse
import csv
import math
import os
import random
import re
import subprocess
from pathlib import Path

import numpy as np

# 使用 generator3 的同类型生成方案
from generator3 import generate_taskset, generate_c_file

# ---------- 解析工具 ----------

_MISS_RATE_RE = re.compile(
    r"Global\s+miss\s+rate:\s*([0-9]*\.?[0-9]+)\s*%.*misses\s*=\s*(\d+)\s*/\s*jobs\s*=\s*(\d+)",
    re.IGNORECASE
)

def parse_global_miss_rate(stdout_text: str):
    for line in stdout_text.splitlines():
        m = _MISS_RATE_RE.search(line)
        if m:
            try:
                return float(m.group(1)), int(m.group(2)), int(m.group(3))
            except Exception:
                pass
    return None, None, None


def read_delay_ratios(case_dir: Path):
    vals = []
    for p in case_dir.glob("task_*_delays.csv"):
        try:
            with p.open("r", newline="") as f:
                reader = csv.DictReader(f)
                for row in reader:
                    v = row.get("delay_ratio")
                    if v is None:
                        continue
                    try:
                        vals.append(float(v))
                    except Exception:
                        pass
        except Exception:
            pass
    return vals


def stat4(vals):
    if not vals:
        return (float("nan"), float("nan"), float("nan"), float("nan"))
    n = len(vals)
    mean_v = sum(vals) / n
    var_v = sum((x - mean_v) ** 2 for x in vals) / n
    std_v = math.sqrt(var_v)
    return (mean_v, std_v, min(vals), max(vals))


def format4(vals):
    out = []
    for v in vals:
        if v is None or (isinstance(v, float) and math.isnan(v)):
            out.append("NaN")
        else:
            out.append(f"{v:.9f}")
    return tuple(out)


# ---------- 主流程 ----------

def main():
    here = Path(__file__).resolve().parent

    ap = argparse.ArgumentParser(description="Random Cr/Contention cases for generator3 (no plotting).")
    ap.add_argument("--task", "--tasks", dest="tasks", type=int, required=True)
    ap.add_argument("--M", type=int, default=4)
    ap.add_argument("--N", type=int, default=None)
    ap.add_argument("--fn", type=int, default=10)
    ap.add_argument("--wcet-min", type=int, default=200)
    ap.add_argument("--wcet-max", type=int, default=500)
    ap.add_argument("--raf-max", type=int, default=200)
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--decimals", type=int, default=2)
    ap.add_argument("--bins", type=int, default=0)
    ap.add_argument("--delay-range", type=float, nargs=2, default=None)
    ap.add_argument("--miss-range", type=float, nargs=2, default=None)
    ap.add_argument("--gcc", type=str, default="gcc")
    ap.add_argument("--compile-flags", type=str, default="-O2 -pthread -lm")
    ap.add_argument("--timeout", type=int, default=60)
    ap.add_argument("--out", type=Path, default=Path("out_tool4"))
    ap.add_argument("--skip-if-done", action="store_true")
    ap.add_argument("--linuxapi-path", type=Path, default=here / "LinuxAPI")
    ap.add_argument("--tacle-path", type=Path, default=here / "Taclebench")
    ap.add_argument("--linuxapi-c", type=str, default="linuxAPI_lib.c")
    ap.add_argument("--bench-c", type=str, default="bench_lib.c")
    ap.add_argument("--linuxapi-h", type=str, default="linuxAPI_lib.h")
    ap.add_argument("--bench-h", type=str, default="bench_lib.h")

    args = ap.parse_args()
    out_root: Path = args.out
    out_root.mkdir(parents=True, exist_ok=True)

    Ncases = args.tasks
    M = args.M
    N = args.N if args.N is not None else M

    random.seed(args.seed)
    np.random.seed(args.seed)

    cases_csv = out_root / "cases.csv"
    summary_csv = out_root / "summary.csv"
    hist_csv = out_root / "histograms.csv"

    with open(cases_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["case_id", "M", "N", "Cr", "contention",
                    "delay_mean", "miss_rate_percent", "misses", "jobs"])

    per_case_delay_means = []
    per_case_miss_rates  = []
    sum_misses = 0
    sum_jobs   = 0

    for i in range(Ncases):
        Cr = round(random.random(), args.decimals)
        cont = round(random.random(), args.decimals)

        case_dir = out_root / f"case_{i:03d}"
        c_path = case_dir / "generated_taskset.c"

        case_dir.mkdir(parents=True, exist_ok=True)
        taskset = generate_taskset(
            M=M, N=N,
            wcet_min=args.wcet_min, wcet_max=args.wcet_max,
            Cr=Cr, RaF_max=args.raf_max, FN=args.fn,
            contention=cont
        )
        generate_c_file(taskset, str(c_path))

        cmd = (
            f'{args.gcc} {args.compile_flags} '
            f'-I"{args.linuxapi_path}" -I"{args.tacle_path}" '
            f'generated_taskset.c '
            f'"{args.linuxapi_path / args.linuxapi_c}" "{args.tacle_path / args.bench_c}" '
            f'-o taskset.out'
        )
        comp = subprocess.run(cmd, shell=True, cwd=str(case_dir),
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if comp.returncode != 0:
            dmean, miss_rate, misses, jobs = float("nan"), float("nan"), 0, 0
        else:
            try:
                runp = subprocess.run(["./taskset.out"], cwd=str(case_dir),
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      text=True, timeout=args.timeout)
            except subprocess.TimeoutExpired:
                dmean, miss_rate, misses, jobs = float("nan"), float("nan"), 0, 0
            else:
                with open(case_dir / "run_log.txt", "w") as lf:
                    lf.write(runp.stdout)
                dvals = read_delay_ratios(case_dir)
                dmean = (sum(dvals) / len(dvals)) if dvals else float("nan")
                miss_rate, misses, jobs = parse_global_miss_rate(runp.stdout)
                if miss_rate is None:
                    miss_rate, misses, jobs = float("nan"), 0, 0

        with open(cases_csv, "a", newline="") as f:
            w = csv.writer(f)
            w.writerow([i, M, N, f"{Cr:.{args.decimals}f}", f"{cont:.{args.decimals}f}",
                        ("" if math.isnan(dmean) else f"{dmean:.9f}"),
                        ("" if (isinstance(miss_rate, float) and math.isnan(miss_rate)) else f"{miss_rate:.9f}"),
                        misses, jobs])

        if not math.isnan(dmean):
            per_case_delay_means.append(dmean)
        if not (isinstance(miss_rate, float) and math.isnan(miss_rate)):
            per_case_miss_rates.append(miss_rate)
        sum_misses += (misses or 0)
        sum_jobs   += (jobs or 0)

        print(f"[CASE {i:03d}] Cr={Cr:.{args.decimals}f} cont={cont:.{args.decimals}f} "
              f"delay_mean={(f'{dmean:.6f}' if not math.isnan(dmean) else 'NaN')}  "
              f"miss_mean={(f'{miss_rate:.6f}%' if not (isinstance(miss_rate, float) and math.isnan(miss_rate)) else 'NaN')}")

    delay_stats = stat4(per_case_delay_means)
    miss_stats  = stat4(per_case_miss_rates)

    with open(summary_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["cases", "M", "N", "seed",
                    "delay_mean_mean", "delay_mean_std", "delay_mean_min", "delay_mean_max",
                    "miss_mean", "miss_std", "miss_min", "miss_max",
                    "sum_misses", "sum_jobs"])
        w.writerow([Ncases, M, N, args.seed,
                    *format4(delay_stats),
                    *format4(miss_stats),
                    sum_misses, sum_jobs])

    # -------- 输出直方图数据 --------
    def histogram_data(values, bins_arg, range_arg):
        if not values:
            return [], []
        if bins_arg and bins_arg > 0:
            counts, edges = np.histogram(values, bins=bins_arg, range=range_arg)
        else:
            if range_arg:
                counts, edges = np.histogram(values, bins="auto", range=range_arg)
            else:
                counts, edges = np.histogram(values, bins="auto")
        return counts.tolist(), edges.tolist()

    delay_counts, delay_edges = histogram_data(per_case_delay_means, args.bins, tuple(args.delay_range) if args.delay_range else None)
    miss_counts,  miss_edges  = histogram_data(per_case_miss_rates,  args.bins, tuple(args.miss_range)  if args.miss_range  else None)

    with open(hist_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["metric", "bin_left", "bin_right", "count"])
        if delay_counts:
            for j in range(len(delay_counts)):
                w.writerow(["delay_mean", f"{delay_edges[j]:.9f}", f"{delay_edges[j+1]:.9f}", int(delay_counts[j])])
        if miss_counts:
            for j in range(len(miss_counts)):
                w.writerow(["miss_rate", f"{miss_edges[j]:.9f}", f"{miss_edges[j+1]:.9f}", int(miss_counts[j])])

    print(f"\n[OUTPUT]")
    print(f"Per-case table : {cases_csv.resolve()}")
    print(f"Summary table  : {summary_csv.resolve()}")
    print(f"Histograms CSV : {hist_csv.resolve()}")


if __name__ == "__main__":
    main()