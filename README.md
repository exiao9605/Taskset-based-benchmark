This project provides a Linux-based multicore benchmarking framework built on a task-set generator and explicit contention modeling.  
It reproduces controllable multicore contention scenarios to evaluate schedulability and performance.

The tool includes:
- **Generator** — builds executable task sets using fragments from TacleBench and Linux API libraries.
- **BenchmarkTool** — runs benchmarks in two modes:
  - *Mode 1 (Grid)*: systematic evaluation across M–ρ–α parameter grid.
  - *Mode 2 (Random)*: large-scale statistical analysis across randomized configurations.

Results are saved as CSV files for further visualization (e.g., latency heatmaps, CDFs).

repo/
├─ LinuxAPI/                  # linuxAPI_lib.h / linuxAPI_lib.c （LinuxAPI fragments）
├─ Taclebench/                # bench_lib.h / bench_lib.c （Taclebench fragments）
├─ generator
   ├─ generator1.py           # early version for delay ratio only
   ├─ generator2.py           # early version for miss rate only
   ├─ generator3.py           # executable task-set generator （C source template contained）
├─ benchmark_tool
   ├─ benchmark_tool1.py        # Mode1 early version for delay ratio only
   ├─ benchmark_tool2.py        # Mode1 early version for miss rate only
   ├─ benchmark_tool3.py        # Mode 1： (Grid Test for performance characteristics)
   ├─ benchmark_tool4.py        # Mode 2： (Random Test / fixed M )
   ├─ benchmark_tool5.py        # Mode 2： (Random Test / all-random parameters)
└─ README.md          
