# Benchmark harness

Everything in this repository was decided by measurement. This folder holds the tools.

| File | What it does |
|---|---|
| `rumeter.c` | Reads `proc_pid_rusage(RUSAGE_INFO_V6)` and `PROC_PIDTASKINFO` for a pid at the start and end of a window and prints the deltas as JSON: CPU time, retired instructions, cycles, energy (nJ), package-idle and interrupt wakeups, context switches, syscalls, threads, physical footprint. |
| `run.sh` | One measured run. It quits every monitor app (Stats, StatsLite, AirStats) so only one draws in the menu bar, launches the app under test hidden, waits for warm-up, then measures a fixed window. It also records WindowServer CPU time with `ps`, holds a `caffeinate -dimu` assertion so the screen never sleeps or locks, records the session lock state, and appends one JSON line to `results/results.jsonl`. |
| `original-scenario.sh` | Configures the installed original Stats for a scenario (`defaults`, `four`, `all`) and restores the user's backed-up preferences afterwards (`restore`). |
| `lite-scenario.sh` | Configures Stats Lite (modules, interval). |
| `suite-four.sh` | The head-to-head: original vs AirStats (user's config) vs Stats Lite, on the user's four widgets (CPU, GPU, RAM, disk), interleaved with no-app control runs, two rounds. |
| `suite-attrib.sh` | Cost attribution for Stats Lite: the same scenario with parts switched off through hidden defaults (`benchSkip`, `renderMode`, `benchTimerKind`). The differences between variants show where the remaining cost lives. |
| `probe.c`, `probe2.c` | Per-call cost of every candidate kernel/IOKit API (ns and instructions per call). The sampler uses the cheapest. |
| `summarize.py` | Aggregates `results.jsonl` into a markdown table, normalised per minute. |

## Metrics, in order of trust

1. **Retired instructions** (`ri_instructions`). Counts user and kernel instructions executed on behalf of the process. Nearly noise-free, so this is the primary A/B metric.
2. **Energy** (`ri_energy_nj`). The kernel's per-task energy estimate. This is what Activity Monitor's Energy Impact is built from.
3. **Wakeups** (package-idle + interrupt). Each one can pull a CPU cluster out of idle, which matters more for battery than a few microseconds of work.
4. **CPU time.** Resolution is good, but it is affected by frequency and core type (E/P/S).
5. **WindowServer extra CPU**: WindowServer CPU during the run minus the mean of the control runs. Menu bar redraws are composited by WindowServer, so this cost never shows up in the app's own numbers. It is noisy (10 ms resolution, and other apps draw too) and only meaningful as a difference over several runs.

## Reproduce

```sh
make -C .. && clang -O2 -o rumeter rumeter.c && clang -O2 -o session-state session-state.c -framework CoreGraphics -framework CoreFoundation
./suite-four.sh            # ~17 minutes
./summarize.py
```

`original-scenario.sh` backs up nothing itself; `.original-prefs-backup.plist` must exist (it is created once with `defaults export eu.exelban.Stats`). It is git-ignored because it contains a device identifier.

## How to test a change quickly

From fastest to most thorough:

1. **Microbenchmarks** (seconds, deterministic to well under 1%): `panelbench.c` (panel frames, with the compositor holding the shown surface), `chartbench.c` (menu bar strip with charts; `PNG=1` also checks it against a full render), `procbench.c`, `gpuprocbench.c` (top GPU processes per 2 s refresh), `blitbench.c`.
2. **Per-phase profile in the app** (about a minute): `make DEV=1 BUILD=build-dev`, then run the binary with `LITE_PROFILE=1`.
3. **`quick-ab.sh <appA> <appB> [rounds] [window] [four|panel|chart]`** (2 to 3 minutes): runs both builds at the same time and meters them over the same window, so machine activity affects both equally. A build compared with itself comes out within about ±6% per round, so it settles changes larger than about 5%.
4. **`suite-ab.sh`** (about 17 minutes): sequential, interleaved runs. Use it as the final check before quoting a number.

Stats Lite's launch argument `-interval N` (used by the suites) sets CPU, GPU, memory, network and disk activity to N seconds, overriding the per-module defaults, so results stay comparable with earlier runs. Per-module settings are `-every.cpu N`, `-every.gpu N`, `-every.mem N`, `-every.net N`, `-every.sensors N`, `-every.disk N` (free space), `-every.diskio N`.

