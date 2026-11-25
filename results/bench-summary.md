| Workload | Chunk | Metric | `rustring` | `cring` | Rust / C |
|---|---|---|---|---|---|
| latency, 64 B write+read | 64 B | p50 ns | 40624 | 38589 | 1.053 |
| latency, 64 B write+read | 64 B | p99 ns | 327286 | 355475 | 0.921 |
| write+read, 1 thread | 16 B | MB/s | 0.7 ± 0.1 | 0.7 ± 0.2 | 0.963 |
| write+read, 1 thread | 64 B | MB/s | 2.8 ± 0.6 | 2.9 ± 0.6 | 0.955 |
| write+read, 1 thread | 256 B | MB/s | 11.5 ± 2.3 | 10.7 ± 1.6 | 1.073 |
| write+read, 1 thread | 1024 B | MB/s | 44.5 ± 7.6 | 41.5 ± 3.1 | 1.073 |
| write+read, 1 thread | 4096 B | MB/s | 168.4 ± 29.1 | 169.9 ± 13.2 | 0.991 |
| producer/consumer, 2 threads | 512 B | MB/s | 17.8 ± 3.4 | 17.9 ± 1.3 | 0.992 |
| producer/consumer, 2 threads | 4096 B | MB/s | 132.0 ± 35.6 | 128.9 ± 8.1 | 1.024 |
