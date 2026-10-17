# TIDE: fast hot-set migration for tiered memory

This tree is Linux v5.15 with TIDE, a memory-tiering policy and migration backend for systems with a
fast tier (local DRAM) and a slow tier (e.g. CXL memory exposed as a CPU-less NUMA node).

TIDE consists of:

- **Hotness measurement by Captured Idle Time (CIT).** The NUMA-balancing scanner marks pages; the
  time from marking to the next access (the hint fault) is the page's CIT. A page is hot if its CIT is
  below a threshold. Huge pages use the same measurement with a threshold scaled by page size.
- **Dynamic CIT statistic collection (DCSC).** Per-process CIT samples calibrate the hot threshold and
  the promotion rate limit.
- **Two-round candidate filtering.** A page judged hot is first recorded as a candidate; it is promoted
  only if the next scan judges it hot again.
- **Adaptive scan volume.** The amount of memory scanned per period grows when scanning keeps finding
  hot slow-tier pages and shrinks when it does not, so hot-page discovery scales with a hot-set change.
- **Asynchronous promotion.** Faulting threads do not wait for copies: promotions are queued per node
  and executed in transactions by promotion executors.
- **DSA-first copy backend.** Promotion transactions are copied by an Intel Data Streaming Accelerator
  (DSA) work queue when a DSA slot is free and by CPU otherwise.

## Building

```sh
make x86_64_defconfig tide.config   # or merge kernel/configs/tide.config into your own config
make -j"$(nproc)"
```

`kernel/configs/tide.config` lists the options TIDE needs (NUMA balancing, THP, migration, idxd
dmaengine support, dax kmem for CXL memory).

## Running

1. Expose the slow memory as a NUMA node (e.g. `daxctl reconfigure-device --mode=system-ram`) and
   boot with `cpuset_allow_movable_only=1` if that node is onlined as movable-only.
2. Enable demotion and tiering:
   ```sh
   echo 1 > /sys/kernel/mm/numa/demotion_enabled
   echo 2 > /proc/sys/kernel/numa_balancing
   ```
3. For DSA copying, configure a dedicated **kernel** work queue that registers a dmaengine channel,
   for example with accel-config:
   ```sh
   accel-config config-engine dsa0/engine0.0 --group-id=0     # repeat for each engine
   accel-config config-wq dsa0/wq0.0 --group-id=0 --mode=dedicated --wq-size=128 \
       --priority=10 --type=kernel --name=tide --driver-name=dmaengine \
       --max-batch-size=128 --max-transfer-size=2097152
   accel-config enable-device dsa0
   accel-config enable-wq dsa0/wq0.0
   ```
   Without a DSA channel TIDE copies with the CPU. Boot with `tide_dsa.disable=1` to never use DSA.

## Tunables (`/proc/sys/kernel/`)

Defaults are the configuration used in the evaluation.

| sysctl | default | meaning |
|---|---|---|
| `numa_balancing` | (kernel default) | 2 enables memory tiering |
| `numa_balancing_hot_threshold_ms` | 1000 | initial CIT threshold; DCSC adjusts it at run time |
| `numa_balancing_rate_limit_mbps` | 65536 | upper bound of the promotion rate limit |
| `numa_balancing_statistic_period_ms` | 1000 | DCSC statistic period |
| `numa_balancing_scan_delay_ms` | 1000 | delay before a new task is scanned |
| `numa_balancing_scan_period_min_ms` / `_max_ms` | 1000 / 2000 | scan period range |
| `numa_balancing_scan_size_mb` | 256 | scan volume per period (initial value when adaptive) |
| `numa_balancing_scan_adaptive` | 1 | 1 = adaptive scan volume, 0 = fixed `scan_size_mb` |
| `numa_balancing_scan_adaptive_max_mb` | 4096 | upper bound of the adaptive scan volume |
| `numa_balancing_promote_watermark_mb` | 2048 | free memory kept in the fast tier for promotions |
| `numa_balancing_promote_batch_pages` | 64 | base pages per promotion transaction |
| `numa_balancing_promote_executors` | 8 | maximum promotion executors per node |
| `numa_balancing_copy_mode` | 2 | 0 = CPU copy, 1 = DSA copy, 2 = DSA first, CPU for overflow |
| `numa_balancing_dsa_inflight_limit` | 4 | DSA slots (transactions in flight on DSA) |

## Counters (`/proc/vmstat`)

`pgpromote_candidate`, `pgpromote_success`, `pgdemote_kswapd`, `pgdemote_direct`, `promote_threshold`
(current hot threshold), `pgpromote_demoted` (promoted pages that had been demoted shortly before),
`pgmigrate_dsa_pages` / `pgmigrate_copy_cpu_completed_pages` (pages copied by DSA / by CPU) and
`pgmigrate_dsa_fallback` (DSA batches completed by CPU copy).

## License

GPL-2.0, as the Linux kernel (see `COPYING`).
