# 利用generator.py 批量生成.c代码并依次执行，
import argparse
import csv
import math
import os
import random
from pathlib import Path
import subprocess

import numpy as np  # 用于给 generator 内的 numpy 随机数播种

# 请保证和本脚本同目录，有以下函数：
# - generate_taskset(M, N, wcet_min, wcet_max, Cr, RaF_max, FN, contention) -> dict
# - generate_c_file(taskset, output_path: str) -> None
from generator import generate_taskset, generate_c_file


def make_grid():
    """M=N=1..16; Cr, cont ∈ {0.00,0.20,...,1.00}"""
    M_vals = list(range(1, 17))                        # 1..16
    frac_vals = [round(0.10 * i, 2) for i in range(11)] # 0.00..1.00
    for M in M_vals:
        N = M
        for Cr in frac_vals:
            for cont in frac_vals:
                yield (M, N, Cr, cont)


def seed_for(M, N, Cr, cont, run_idx):
    """确定性 seed：保证可复现；避免 0"""
    s = (
        (M * 73856093)
        ^ (N * 19349663)
        ^ (int(round(Cr * 100)) * 83492791)
        ^ (int(round(cont * 100)) * 2654435761)
        ^ (run_idx + 1)
    )
    s &= 0xFFFFFFFF
    return 1 if s == 0 else s


def parse_delay_ratios(case_dir: Path):
    """读取该 run 产生的所有 task_*_delays.csv，并返回 delay_ratio 列合并值"""
    vals = []
    for p in case_dir.glob("task_*_delays.csv"):
        with p.open("r", newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                try:
                    vals.append(float(row["delay_ratio"]))
                except Exception:
                    pass
    return vals


def main():
    here = Path(__file__).resolve().parent

    ap = argparse.ArgumentParser(description="Serial sweep: generate -> compile -> run -> aggregate")
    ap.add_argument("--out", type=Path, default=Path("out_serial"), help="输出根目录")
    ap.add_argument("--runs", type=int, default=1, help="每个场景重复次数（测试设为1，正式可设为100）")
    ap.add_argument("--wcet-min", type=int, default=200)
    ap.add_argument("--wcet-max", type=int, default=500)
    ap.add_argument("--raf-max", type=int, default=200)
    ap.add_argument("--fn", type=int, default=10, help="每任务分段数")
    ap.add_argument("--gcc", type=str, default="gcc")
    ap.add_argument("--compile-flags", type=str, default="-O2 -pthread -lm")
    ap.add_argument("--timeout", type=int, default=60, help="单次运行超时（秒）")
    ap.add_argument("--skip-if-done", action="store_true", help="若场景已存在结果则跳过（断点续跑）")

    # 关键：两套库路径（默认就是你截图里的相对目录）
    ap.add_argument("--linuxapi-path", type=Path, default=here / "LinuxAPI",
                    help="包含 linuxAPI_lib.h / linuxAPI_lib.c 的目录")
    ap.add_argument("--tacle-path", type=Path, default=here / "Taclebench",
                    help="包含 bench_lib.h / bench_lib.c 的目录")
    # 若文件名有差异，可改名：
    ap.add_argument("--linuxapi-c", type=str, default="linuxAPI_lib.c")
    ap.add_argument("--linuxapi-h", type=str, default="linuxAPI_lib.h")
    ap.add_argument("--bench-c", type=str, default="bench_lib.c")
    ap.add_argument("--bench-h", type=str, default="bench_lib.h")

    args = ap.parse_args()

    out_root: Path = args.out
    out_root.mkdir(parents=True, exist_ok=True)

    host_cores = os.cpu_count() or 1
    print(f"[INFO] Host cores: {host_cores}")

    # 绝对路径，避免 cwd 变化导致相对路径失效
    linuxapi_dir = args.linuxapi_path.resolve()
    tacle_dir    = args.tacle_path.resolve()
    linuxapi_c   = (linuxapi_dir / args.linuxapi_c).resolve()
    bench_c      = (tacle_dir / args.bench_c).resolve()

    # 友好提示，防止路径写错
    for pth, lab in [(linuxapi_dir, "LinuxAPI dir"), (tacle_dir, "Taclebench dir"),
                     (linuxapi_c, "linuxAPI_lib.c"), (bench_c, "bench_lib.c"),
                     (linuxapi_dir / args.linuxapi_h, "linuxAPI_lib.h"),
                     (tacle_dir / args.bench_h, "bench_lib.h")]:
        if not pth.exists():
            print(f"[WARN] Not found: {lab} -> {pth}")

    # ========== 阶段1：生成所有 C 文件 ==========
    print("[STEP 1] Generating all C files...")
    total_cases = 0
    gen_cases = 0

    for (M, N, Cr, cont) in make_grid():
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

            # 为该 run 播种（random + numpy）
            sd = seed_for(M, N, Cr, cont, run_idx)
            random.seed(sd)
            np.random.seed(sd)

            # 生成任务集并写 C
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

    # ========== 阶段2：依次编译并运行 ==========
    print("[STEP 2] Compiling & running sequentially...")
    summary_path = out_root / "summary.csv"
    write_header = not (args.skip_if_done and summary_path.exists())
    summary_f = open(summary_path, "a" if args.skip_if_done else "w", newline="")
    summary_w = csv.writer(summary_f)
    if write_header:
        summary_w.writerow([
            "M","N","Cr","contention",
            "runs","mean_of_means","std_of_means","min_of_means","max_of_means"
        ])

    done_keys = set()
    if args.skip_if_done and summary_path.exists():
        with open(summary_path, "r", newline="") as sf:
            rd = csv.DictReader(sf)
            for row in rd:
                key = (int(row["M"]), int(row["N"]), row["Cr"], row["contention"])
                done_keys.add(key)

    compiled = 0
    ran = 0
    frac_vals = [round(0.10 * i, 2) for i in range(11)]
    for M in range(1, 17):
        N = M
        if M > host_cores:
            continue
        for Cr in frac_vals:
            for cont in frac_vals:
                key = (M, N, f"{Cr:.2f}", f"{cont:.2f}")
                if args.skip_if_done and key in done_keys:
                    continue

                case_dirs = [
                    out_root / f"M{M}_N{N}" / f"Cr_{Cr:0.2f}" / f"cont_{cont:0.2f}" / f"run_{i:03d}"
                    for i in range(args.runs)
                ]

                run_means = []
                for case_dir in case_dirs:
                    c_path = case_dir / "generated_taskset.c"
                    if not c_path.exists():
                        print(f"[MISS] {c_path} not found, skip this run.")
                        continue

                    # 编译（cwd=case_dir，只传文件名；包含两个库目录与两份源码的绝对路径）
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

                    # 解析
                    vals = parse_delay_ratios(case_dir)
                    if not vals:
                        print(f"[NO_VALUES] {case_dir}")
                        continue
                    run_means.append(sum(vals) / len(vals))

                # 聚合该场景
                if run_means:
                    n = len(run_means)
                    mean_of_means = sum(run_means) / n
                    var = sum((x - mean_of_means) ** 2 for x in run_means) / n
                    std = math.sqrt(var)
                    mn = min(run_means)
                    mx = max(run_means)

                    summary_w.writerow([
                        M, N, f"{Cr:.2f}", f"{cont:.2f}",
                        n,
                        f"{mean_of_means:.9f}", f"{std:.9f}",
                        f"{mn:.9f}", f"{mx:.9f}"
                    ])
                    summary_f.flush()
                    print(f"[OK] M={M} Cr={Cr:.2f} cont={cont:.2f} runs={n} mean={mean_of_means:.6f}")
                else:
                    summary_w.writerow([M, N, f"{Cr:.2f}", f"{cont:.2f}", 0, "NaN", "NaN", "NaN", "NaN"])
                    summary_f.flush()

    summary_f.close()
    print(f"[DONE] Compiled: {compiled}, Ran: {ran}")
    print(f"[OUTPUT] Summary -> {summary_path.resolve()}")
    print("Tip: 先用 --runs 1 验证流程无误，再改成 --runs 100 批量跑。")


if __name__ == "__main__":
    main()