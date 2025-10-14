# generator2_sweep.py
import argparse
import csv
import math
import os
import random
import subprocess
import time
from pathlib import Path

import numpy as np  # 用于给 generator2 内的 numpy 随机数播种

# 关键：导入 generator2.py
from generator2 import generate_taskset, generate_c_file


def make_grid():
    """M=N=1..16; Cr, cont ∈ {0.00,0.10,...,1.00}"""
    M_vals = list(range(1, 17))
    frac_vals = [round(0.20 * i, 2) for i in range(6)]
    for M in M_vals:
        N = M
        for Cr in frac_vals:
            for cont in frac_vals:
                yield (M, N, Cr, cont)


def seed_for(M, N, Cr, cont, run_idx):
    s = (
        (M * 73856093)
        ^ (N * 19349663)
        ^ (int(round(Cr * 100)) * 83492791)
        ^ (int(round(cont * 100)) * 2654435761)
        ^ (run_idx + 1)
    )
    s &= 0xFFFFFFFF
    return 1 if s == 0 else s


def parse_schedulability(stdout_text: str):
    """
    从 generator2 生成的 C 程序输出里提取 schedulability 值
    格式: 'Schedulability (misses/total jobs): 0.123456'
    """
    vals = []
    for line in stdout_text.splitlines():
        if "Schedulability" in line:
            try:
                val = float(line.strip().split()[-1])
                vals.append(val)
            except Exception:
                pass
    return vals


def main():
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Serial sweep for generator2: generate -> compile -> run -> aggregate")
    ap.add_argument("--out", type=Path, default=Path("out_serial2"), help="输出根目录")
    ap.add_argument("--runs", type=int, default=1, help="每个场景重复次数")
    ap.add_argument("--wcet-min", type=int, default=200)
    ap.add_argument("--wcet-max", type=int, default=500)
    ap.add_argument("--raf-max", type=int, default=200)
    ap.add_argument("--fn", type=int, default=10, help="每任务分段数")
    ap.add_argument("--gcc", type=str, default="gcc")
    ap.add_argument("--compile-flags", type=str, default="-O2 -pthread -lm")
    ap.add_argument("--timeout", type=int, default=60, help="单次运行超时（秒）")
    ap.add_argument("--skip-if-done", action="store_true", help="若场景已存在结果则跳过（断点续跑）")

    ap.add_argument("--linuxapi-path", type=Path, default=here / "LinuxAPI")
    ap.add_argument("--tacle-path", type=Path, default=here / "Taclebench")
    ap.add_argument("--linuxapi-c", type=str, default="linuxAPI_lib.c")
    ap.add_argument("--linuxapi-h", type=str, default="linuxAPI_lib.h")
    ap.add_argument("--bench-c", type=str, default="bench_lib.c")
    ap.add_argument("--bench-h", type=str, default="bench_lib.h")

    args = ap.parse_args()

    out_root: Path = args.out
    out_root.mkdir(parents=True, exist_ok=True)

    host_cores = os.cpu_count() or 1
    print(f"[INFO] Host cores: {host_cores}")

    linuxapi_dir = args.linuxapi_path.resolve()
    tacle_dir = args.tacle_path.resolve()
    linuxapi_c = (linuxapi_dir / args.linuxapi_c).resolve()
    bench_c = (tacle_dir / args.bench_c).resolve()

    # ========== 阶段1：生成所有 C 文件 ==========
    print("[STEP 1] Generating all C files...")
    total_cases = 0
    gen_cases = 0

    for (M, N, Cr, cont) in make_grid():
        if M > host_cores:
            continue
        for run_idx in range(args.runs):
            total_cases += 1
            case_dir = out_root / f"M{M}_N{N}" / f"Cr_{Cr:0.2f}" / f"cont_{cont:0.2f}" / f"run_{run_idx:03d}"
            c_path = case_dir / "generated_taskset.c"
            if args.skip_if_done and c_path.exists():
                continue

            case_dir.mkdir(parents=True, exist_ok=True)

            # 为该 run 播种
            sd = seed_for(M, N, Cr, cont, run_idx)
            random.seed(sd)
            np.random.seed(sd)

            taskset = generate_taskset(
                M=M, N=N,
                wcet_min=args.wcet_min, wcet_max=args.wcet_max,
                Cr=Cr, RaF_max=args.raf_max, FN=args.fn,
                contention=cont
            )
            generate_c_file(taskset, str(c_path))
            gen_cases += 1
            print(f"[GEN_OK] {c_path}")

    print(f"[STEP 1 DONE] Generated {gen_cases}/{total_cases} cases.")

    # ========== 阶段2：依次编译并运行 ==========
    print("[STEP 2] Compiling & running sequentially...")
    summary_path = out_root / "summary.csv"
    write_header = not (args.skip_if_done and summary_path.exists())
    summary_f = open(summary_path, "a" if args.skip_if_done else "w", newline="")
    summary_w = csv.writer(summary_f)
    if write_header:
        summary_w.writerow([
            "M","N","Cr","contention",
            "runs","mean_sched","std_sched","min_sched","max_sched"
        ])

    compiled = 0
    ran = 0
    frac_vals = [round(0.20 * i, 2) for i in range(6)]
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

                run_vals = []
                for case_dir in case_dirs:
                    c_path = case_dir / "generated_taskset.c"
                    if not c_path.exists():
                        continue

                    # 编译
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

                    # 运行
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

                    # 保存 stdout -> log.txt
                    with open(case_dir / "run_log.txt", "w") as lf:
                        lf.write(runp.stdout)

                    # 解析 schedulability
                    vals = parse_schedulability(runp.stdout)
                    run_vals.extend(vals)

                if run_vals:
                    n = len(run_vals)
                    mean_val = sum(run_vals) / n
                    var = sum((x - mean_val) ** 2 for x in run_vals) / n
                    std_val = math.sqrt(var)
                    mn = min(run_vals)
                    mx = max(run_vals)

                    summary_w.writerow([M, N, f"{Cr:.2f}", f"{cont:.2f}",
                                        n,
                                        f"{mean_val:.9f}", f"{std_val:.9f}",
                                        f"{mn:.9f}", f"{mx:.9f}"])
                    summary_f.flush()
                    print(f"[OK] M={M} Cr={Cr:.2f} cont={cont:.2f} runs={n} mean_sched={mean_val:.6f}")
                else:
                    summary_w.writerow([M, N, f"{Cr:.2f}", f"{cont:.2f}", 0, "NaN", "NaN", "NaN", "NaN"])
                    summary_f.flush()

    summary_f.close()
    print(f"[DONE] Compiled: {compiled}, Ran: {ran}")
    print(f"[OUTPUT] Summary -> {summary_path.resolve()}")


if __name__ == "__main__":
    main()