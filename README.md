This project provides a Linux-based multicore benchmarking framework built on a task-set generator and explicit contention modeling.  
It reproduces controllable multicore contention scenarios to evaluate schedulability and performance.

The tool includes:
- **Generator** — builds executable task sets using fragments from TacleBench and Linux API libraries.
- **BenchmarkTool** — runs benchmarks in two modes:
  - *Mode 1 (Grid)*: systematic evaluation across M–ρ–α parameter grid.
  - *Mode 2 (Random)*: large-scale statistical analysis across randomized configurations.

Results are saved as CSV files for further visualization (e.g., latency heatmaps, CDFs).

repo/  <br>
├─ LinuxAPI/                  # linuxAPI_lib.h / linuxAPI_lib.c （LinuxAPI fragments）  <br>
├─ Taclebench/                # bench_lib.h / bench_lib.c （Taclebench fragments） <br>
├─ generator  <br>
   ├─ generator1.py           # early version for delay ratio only  <br>
   ├─ generator2.py           # early version for miss rate only <br>
   ├─ generator3.py           # executable task-set generator （C source template contained）<br>
├─ benchmark_tool <br>
   ├─ benchmark_tool1.py        # Mode1 early version for delay ratio only <br>
   ├─ benchmark_tool2.py        # Mode1 early version for miss rate only <br>
   ├─ benchmark_tool3.py        # Mode 1： (Grid Test for performance characteristics) <br>
   ├─ benchmark_tool4.py        # Mode 2： (Random Test / fixed M ) <br>
   ├─ benchmark_tool5.py        # Mode 2： (Random Test / all-random parameters) <br>
└─ README.md    <br>      
