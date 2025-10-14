#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Sweep Cr/cont, generate -> compile -> run -> aggregate (delay & miss) for generator3
- Generation pipeline mirrors benchmark_tool.py / benchmark_tool2.py
- Reading logic combines:
  * delay_ratio from task_*_delays.csv
  * global miss rate (%) from program stdout line: "Global miss rate: XX.XX% (misses=... / jobs=...)"
"""

import argparse
import csv
import math
import os
import random
import re
import subprocess
from pathlib import Path

import numpy as np  # seed numpy inside generator3

# generator3: must provide generate_taskset(...) and generate_c_file(...)
from generator3 import generate_taskset, generate_c_file  # <-- 适配你的 generator3

# ----------------------------
# grid & reproducible seeding
# ----------------------------

def make_grid(step: float):
    """
    Grid: M=N=1..16; Cr, cont ∈ {0.00, step, 2*step, ... , 1.00}
    Default step is configurable via --step (e.g., 0.10 to mimic tool1; 0.20 to mimic tool2)
    """
    M_vals = list(range(1, 17))
    frac_vals = [round(step * i, 2) for i in range(int(round(1.0 / step)) + 1)]
    for M in M_vals:
        N = M
        for Cr in frac_vals:
            for cont in frac_vals:
                yield (M, N, Cr, cont)


def seed_for(M, N, Cr, cont, run_idx):
    """Deterministic seed (same scheme as tool1/2)."""
    s = (
        (M * 73856093)
        ^ (N * 19349663)
        ^ (int(round(Cr * 100)) * 83492791)
        ^ (int(round(cont * 100)) * 2654435761)
        ^ (run_idx + 1)
    )
    s &= 0xFFFFFFFF
    return 1 if s == 0 else s

# ----------------------------
# parsers: delay & miss rate
# ----------------------------

def parse_delay_ratios(case_dir: Path):
    """
    Read all task_*_delays.csv -> collect "delay_ratio" -> return list[float]
    """
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


_miss_rate_re = re.compile(
    r"Global\s+miss\s+rate:\s*([0-9]*\.?[0-9]+)\s*%.*misses\s*=\s*(\d+)\s*/\s*jobs\s*=\s*(\d+)",
    re.IGNORECASE
)

def parse_global_miss_rate(stdout_text: str):
    """
    Parse 'Global miss rate: XX.XX%  (misses=... / jobs=...)'
    Returns (miss_rate_percent: float, misses: int, jobs: int) or (None, None, None) if not found
    """
    for line in stdout_text.splitlines():
        m = _miss_rate_re.search(line)
        if m:
            try:
                rate = float(m.group(1))
                misses = int(m.group(2))
                jobs = int(m.group(3))
                return rate, misses, jobs
            except Exception:
                continue
    return None, None, None

# ----------------------------
# main pipeline
# ----------------------------

def main():
    here = Path(__file__).resolve().parent

    ap = argparse.ArgumentParser(
        description="Serial sweep for generator3: generate -> compile -> run -> aggregate (delay & miss)"
    )
    # --- output & sweep ---
    ap.add_argument("--out", type=Path, default=Path("out_serial3"), help="输出根目录")
    ap.add_argument("--runs", type=int, default=1, help="每个场景重复次数（建议先 1 验证，再增大）")
    ap.add_argument("--step", type=float, default=0.10, help="Cr/cont 步长，0.10 模拟 tool1，0.20 模拟 tool2")
    # --- task gen args (forwarded to generator3) ---
    ap.add_argument("--wcet-min", type=int, default=200)
    ap.add_argument("--wcet-max", type=int, default=500)
    ap.add_argument("--raf-max", type=int, default=200)
    ap.add_argument("--fn", type=int, default=10, help="每任务分段数")
    # --- compile/run ---
    ap.add_argument("--gcc", type=str, default="gcc")
    ap.add_argument("--compile-flags", type=str, default="-O2 -pthread -lm")
    ap.add_argument("--timeout", type=int, default=60, help="单次运行超时（秒）")
    ap.add_argument("--skip-if-done", action="store_true", help="若场景已存在结果则跳过（断点续跑）")
    # --- library paths (same style as tool1/2) ---
    ap.add_argument("--linuxapi-path", type=Path, default=here / "LinuxAPI",
                    help="包含 linuxAPI_lib.h / linuxAPI_lib.c 的目录")
    ap.add_argument("--tacle-path", type=Path, default=here / "Taclebench",
                    help="包含 bench_lib.h / bench_lib.c 的目录")
    ap.add_argument("--linuxapi-c", type=str, default="linuxAPI_lib.c")
    ap.add_argument("--linuxapi-h", type=str, default="linuxAPI_lib.h")
    ap.add_argument("--bench-c", type=str, default="bench_lib.c")
    ap.add_argument("--bench-h", type=str, default="bench_lib.h")

    args = ap.parse_args()

    out_root: Path = args.out
    out_root.mkdir(parents=True, exist_ok=True)

    host_cores = os.cpu_count() or 1
    print(f"[INFO] Host cores: {host_cores}")

    # absolute paths for includes & .c
    linuxapi_dir = args.linuxapi_path.resolve()
    tacle_dir    = args.tacle_path.resolve()
    linuxapi_c   = (linuxapi_dir / args.linuxapi_c).resolve()
    bench_c      = (tacle_dir / args.bench_c).resolve()

    # sanity hints
    for pth, lab in [(linuxapi_dir, "LinuxAPI dir"), (tacle_dir, "Taclebench dir"),
                     (linuxapi_c, "linuxAPI_lib.c"), (bench_c, "bench_lib.c"),
                     (linuxapi_dir / args.linuxapi_h, "linuxAPI_lib.h"),
                     (tacle_dir / args.bench_h, "bench_lib.h")]:
        if not pth.exists():
            print(f"[WARN] Not found: {lab} -> {pth}")

    # -----------------------
    # Step 1: generate C
    # -----------------------
    print("[STEP 1] Generating all C files...")
    total_cases = 0
    gen_cases = 0

    for (M, N, Cr, cont) in make_grid(args.step):
        if M > host_cores:
            print(f"[WARN] Skip M={M} (exceeds host cores {host_cores})")
            continue
        for run_idx in range(args.runs):
            total_cases += 1
            case_dir = out_root / f"M{M}_N{N}" / f"Cr_{Cr:0.2f}" / f"cont_{cont:0.2f}" / f"run_{run_idx:03d}"
            c_path = case_dir / "generated_taskset.c"

            if args.skip_if_done and c_path.exists():
                continue

            case_dir.mkdir(parents=True, exist_ok=True)

            # deterministic seed (random + numpy)
            sd = seed_for(M, N, Cr, cont, run_idx)
            random.seed(sd)
            np.random.seed(sd)

            # generate
            taskset = generate_taskset(
                M=M, N=N,
                wcet_min=args.wcet_min, wcet_max=args.wcet_max,
                Cr=Cr, RaF_max=args.raf_max, FN=args.fn,
                contention=cont
            )
            generate_c_file(taskset, str(c_path))
            gen_cases += 1
            print(f"[GEN_OK] {c_path}")

    print(f"[STEP 1 DONE] Generated {gen_cases}/{total_cases} cases (some may be skipped).")

    # -----------------------
    # Step 2: compile & run
    #   -> read delay & miss
    #   -> aggregate per (M,N,Cr,cont)
    # -----------------------
    print("[STEP 2] Compiling & running sequentially...")

    summary_path = out_root / "summary.csv"
    write_header = not (args.skip_if_done and summary_path.exists())
    summary_f = open(summary_path, "a" if args.skip_if_done else "w", newline="")
    summary_w = csv.writer(summary_f)

    if write_header:
        summary_w.writerow([
            "M","N","Cr","contention",
            "runs",
            # delay stats (per-run mean of delay ratios)
            "delay_mean_of_means","delay_std_of_means","delay_min_of_means","delay_max_of_means",
            # miss stats (global miss rate % per run)
            "miss_mean","miss_std","miss_min","miss_max",
            # optional: sum of misses/jobs across runs (not averaged)
            "sum_misses","sum_jobs"
        ])

    compiled = 0
    ran = 0
    frac_vals = [round(args.step * i, 2) for i in range(int(round(1.0 / args.step)) + 1)]

    for M in range(1, 17):
        N = M
        if M > host_cores:
            continue
        for Cr in frac_vals:
            for cont in frac_vals:
                case_dirs = [
                    out_root / f"M{M}_N{N}" / f"Cr_{Cr:0.2f}" / f"cont_{cont:0.2f}" / f"run_{i:03d}"
                    for i in range(args.runs)
                ]

                per_run_delay_means = []
                per_run_miss_rates  = []
                sum_misses = 0
                sum_jobs   = 0

                for case_dir in case_dirs:
                    c_path = case_dir / "generated_taskset.c"
                    if not c_path.exists():
                        print(f"[MISS] {c_path} not found, skip this run.")
                        continue

                    # compile
                    cmd = (
                        f'{args.gcc} {args.compile_flags} '
                        f'-I"{linuxapi_dir}" -I"{tacle_dir}" '
                        f'generated_taskset.c '
                        f'"{linuxapi_c}" "{bench_c}" '
                        f'-o taskset.out'
                    )
                    comp = subprocess.run(
                        cmd, shell=True, cwd=str(case_dir),
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
                    )
                    compiled += 1
                    if comp.returncode != 0:
                        print(f"[COMPILE_FAIL] {case_dir}\n{comp.stdout}")
                        continue

                    # run
                    try:
                        runp = subprocess.run(
                            ["./taskset.out"],
                            cwd=str(case_dir),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, timeout=args.timeout
                        )
                    except subprocess.TimeoutExpired:
                        print(f"[RUN_TIMEOUT] {case_dir}")
                        continue

                    ran += 1
                    if runp.returncode != 0:
                        print(f"[RUN_FAIL] {case_dir}\n{runp.stdout}")
                        continue

                    # save stdout
                    with open(case_dir / "run_log.txt", "w") as lf:
                        lf.write(runp.stdout)

                    # parse delay
                    dvals = parse_delay_ratios(case_dir)
                    if dvals:
                        per_run_delay_means.append(sum(dvals) / len(dvals))

                    # parse miss
                    miss_rate, misses, jobs = parse_global_miss_rate(runp.stdout)
                    if miss_rate is not None:
                        per_run_miss_rates.append(miss_rate)
                    if misses is not None and jobs is not None:
                        sum_misses += misses
                        sum_jobs   += jobs

                # aggregate this (M,N,Cr,cont)
                def agg_stats(vals):
                    if not vals:
                        return ("NaN","NaN","NaN","NaN")
                    n = len(vals)
                    mean_v = sum(vals) / n
                    var = sum((x - mean_v) ** 2 for x in vals) / n
                    std_v = math.sqrt(var)
                    return (f"{mean_v:.9f}", f"{std_v:.9f}", f"{min(vals):.9f}", f"{max(vals):.9f}")

                delay_stats = agg_stats(per_run_delay_means)
                miss_stats  = agg_stats(per_run_miss_rates)

                summary_w.writerow([
                    M, N, f"{Cr:.2f}", f"{cont:.2f}",
                    len(per_run_delay_means) if per_run_delay_means or per_run_miss_rates else 0,
                    *delay_stats,
                    *miss_stats,
                    str(sum_misses), str(sum_jobs)
                ])
                summary_f.flush()

                # console friendly report
                # 例： [OK] M=4 Cr=0.30 cont=0.20 runs=5 delay_mean=1.234567 miss_mean=12.345%
                delay_mean_disp = delay_stats[0] if delay_stats[0] != "NaN" else "NaN"
                miss_mean_disp  = miss_stats[0]  if miss_stats[0]  != "NaN" else "NaN"
                print(f"[OK] M={M} Cr={Cr:.2f} cont={cont:.2f} runs={len(per_run_delay_means)} "
                      f"delay_mean={delay_mean_disp} miss_mean={miss_mean_disp}%")

    summary_f.close()
    print(f"[DONE] Compiled: {compiled}, Ran: {ran}")
    print(f"[OUTPUT] Summary -> {summary_path.resolve()}")
    print("Tip: 先用 --runs 1 + 小步长验证，再扩大 runs 与网格密度。")
    

if __name__ == "__main__":
    main()