This project provides a Linux-based multicore benchmarking framework built on a task-set generator and explicit contention modeling.  
It reproduces controllable multicore contention scenarios to evaluate schedulability and performance.

The tool includes:
- **Generator** — builds executable task sets using fragments from TacleBench and Linux API libraries.
- **BenchmarkTool** — runs benchmarks in two modes:
  - *Mode 1 (Grid)*: systematic evaluation across M–ρ–α parameter grid.
  - *Mode 2 (Random)*: large-scale statistical analysis across randomized configurations.

Results are saved as CSV files for further visualization (e.g., latency heatmaps, CDFs).
