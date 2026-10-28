# Benchmarks

Machine: MacBook Pro (Mac17,8), Apple M5 Pro (6 super + 12 performance cores), 48 GB, macOS 27.0. Every number on this page is from that machine and comes from the tools in [bench/](bench/), which also explains how to reproduce it.

The primary metric is **retired instructions** (`ri_instructions` from `proc_pid_rusage`). It counts user and kernel work done for the process and is nearly noise-free, unlike %CPU. Energy is the kernel's per-task estimate (`ri_energy_nj`), the same source as Activity Monitor's Energy Impact. A "tick" is one update interval (1 s unless stated).

## Head-to-head

Three interleaved rounds, 60 s windows after 20 s warm-up, medians, idle machine (`bench/suite-final.sh`, `results/suite-final3.log`, build 8c332919). These runs pass `-interval 1`, which puts CPU, GPU, memory and network all at 1 s (GPU now defaults to 2 s, which costs about 19% less). Only one monitor app runs at a time; every round includes a no-app control. (The previous run, `results/suite-final2.log` on build 33b2857f, measured Stats Lite at 34.4 M instructions and 4.2 mJ per minute for the same setup.)

**Your setup: CPU, GPU, memory and disk in the menu bar.**

| App | Interval | CPU | Instructions/min | Energy/min | Wakeups/min | Syscalls/min | Threads | Memory |
|---|---|---|---|---|---|---|---|---|
| Original Stats 3.0.23 | 1 s | 2.81% | 5,723 M | 440 mJ | 303 | 63,535 | 13 | 149 MB |
| AirStats (your config: text, combined item) | 2 s | 0.168% | 143 M | 17.1 mJ | 105 | 9,821 | 5 | 14.3 MB |
| **Stats Lite** | 1 s | **0.037%** | **30.1 M** | **4.5 mJ** | 60 | 2,149 | 4 | 13.8 MB |
| **Stats Lite** | 2 s | **0.019%** | **15.2 M** | **2.3 mJ** | 31 | 1,065 | 3 | 13.8 MB |
| Stats Lite, CPU/GPU/RAM with label-row sparklines | 1 s | 0.037% | 31.3 M | 4.7 mJ | 61 | 2,228 | 3 | 13.9 MB |
| Stats Lite, CPU and GPU as text + history chart | 1 s | 0.041% | 33.5 M | 5.1 mJ | 61 | 2,468 | 3 | 13.9 MB |
| Stats Lite, panel open (charts, cores, top processes) | 1 s | 0.135% | 100.8 M | 18.4 mJ | 61 | 7,491 | 3 | 28.3 MB |

At the same 1 s interval Stats Lite uses **190× fewer instructions, 98× less energy, 76× fewer CPU-seconds, 5× fewer wakeups, 30× fewer system calls and 11× less memory** than the original. At AirStats' own 2 s interval it uses **9.4× fewer instructions and 7.4× less energy** than AirStats. With its panel open (live charts, per-core bars, top processes every 2 s) it uses 18.4 mJ/min, about what AirStats uses with its panel closed (17.1 mJ/min).

**Everything enabled** (CPU, GPU, memory, disk, network, sensors, battery):

| App | Interval | CPU | Instructions/min | Energy/min | Wakeups/min | Syscalls/min | Memory |
|---|---|---|---|---|---|---|---|
| Original Stats | 1 s | 5.11% | 10,368 M | 872 mJ | 333 | 105,657 | 157 MB |
| **Stats Lite** | 1 s | **0.051%** | **41.7 M** | **6.2 mJ** | 61 | 2,568 | 14.2 MB |

That is **249× fewer instructions and 141× less energy**.

**Modules added later** (`results/suite-extra.log`, two runs each):

| Stats Lite | Instructions/min | Energy/min | Wakeups/min |
|---|---|---|---|
| All nine modules (adds Bluetooth accessories and clock), 1 s | 49.2 M | 6.3 mJ | 62 |
| Clock only (wakes on the minute) | 2.3 M | 0.3 mJ | 4 |

WindowServer CPU attributable to each app could not be separated from noise (interleaved control runs varied more than the differences), so it is not reported.

### Build-to-build checks

Every later build was A/B tested against the previous measured one, interleaved:

| From → to | Menu bar, instr/min | Panel open, instr/min | Panel memory |
|---|---|---|---|
| 31440e6e → e7f7553b (review fixes) | 36.0 → 34.6 M | 161.5 → 151.1 M | 32 MB |
| e7f7553b → 33b2857f (NEON text, surface choice, process trigger, new modules) | 33.9 → 35.3 M (noise) | 149.0 → 136.8 M | 32.2 → 28.1 MB |
| 33b2857f → lazily loaded UserNotifications | 34.1 → 35.2 M (noise) | | **rejected, reverted** |
| 33b2857f → assembly compositor, ink-limited scrolling, 2 s memory in the panel | 37.5 → 37.0 M (noise) | 130.7 → 123.6 M | 28.1 → 28.3 MB |
| e0b52692 → warm processes every 6th refresh instead of every 3rd (`results/ab-warm.log`, two rounds) | 35.8 → 33.3 M (processes are panel-only, so noise) | 131.2 → 126.7 M, syscalls −7% | 28.2 MB |
| aac06ff0 → copy-back from the shown panel surface (`results/ab-copyback.log`, three rounds) | 37.3 → 36.1 M (noise) | 120.8 → 116.9 M | 28.1 → 28.2 MB |
| c1c9fcc1 → panel position and appearance recomputed only when they change (`results/ab-pos.log`, three rounds) | 46.3 → 46.3 M (both arms with text + chart styles, left saved by a self-test run; the self-test now restores settings and the suites pass the styles) | 123.6 → 118.7 M | 28.3 MB |
| 5512c1e8 → one Core Animation transaction per tick for strip and panel (`results/ab-tx.log`) | 46.9 → 46.2 M (noise; text + chart styles as above) | 119.5 → 118.3 M (pairs +1.1, −7.7, −0.4: noise) | **rejected, reverted** |
| d069c100 → menu bar charts scroll instead of redrawing, text + chart style (`results/ab-chartscroll2.log`, three rounds) | **46.8 → 39.0 M**, energy 5.07 → 4.59 mJ | 130.7 → 124.3 M | 13.8 MB |
| dff13075 → panel clears and copies only the columns an item's text inked (`results/ab-ext.log`, three rounds) | 35.6 → 33.1 M (panel-only change: this is the noise of sequential runs) | 116.4 → 112.5 M, energy 15.4 → 14.2 mJ | 28.1 MB |
| 1b828e30 → spike-attribution baseline only at the first busy tick of a minute (`quick-ab.sh`, three paired 60 s rounds) | **−8.8% instructions, −5.7% energy, −17% syscalls** (rounds 1.02, 0.91, 0.88) | | |
| e802659c → per-module refresh rates; new defaults GPU 2 s (was 1 s), memory 1 s (was 2 s) (`bench/results` not kept: three paired 60 s rounds, both builds at their defaults) | **−19% instructions, −17% energy** (rounds 0.835, 0.799, 0.812) | | |

## The floor

What any 1 Hz menu bar app pays on this machine, measured side by side with Stats Lite over the same minute (`bench/floor.m`; all four processes running at once, so absolute energy is higher than in solo runs):

| Process | Instructions/min | Energy/min | Syscalls/min |
|---|---|---|---|
| Status item + 1 Hz timer that does nothing | 10.0 M | 2.0 mJ | 1,022 |
| + swap the item's IOSurface each tick (one Core Animation commit) | 17.4 M | 4.0 mJ | 2,043 |
| + read GPU PerformanceStatistics each tick (as Stats Lite does) | 30.4 M | 6.5 mJ | 2,102 |
| Stats Lite, CPU/GPU/RAM/disk as text | 39.4 M | 8.4 mJ | 2,826 |

After the later cuts (assembly zero-destination path, value-only repaints, and above all no per-minute process scan on an idle machine), the same side-by-side measurement, three 60 s rounds: floor with GPU read 28.4 / 29.9 / 28.5 M, Stats Lite 29.9 / 31.4 / 28.3 M. Stats Lite's own work (CPU and memory sampling, formatting, drawing, history) is now about 1.5 M instructions per minute, within the run-to-run noise of the floor itself.

Also measured in `floor.m`, none cheaper: rewriting one surface in place and calling `setContentsChanged` (17.6 M, the same as swapping surfaces: the commit is the cost); running the timer on a background thread's plain CFRunLoop (10.3 M vs 10.6 M empty, and committing from it 17.0 M vs 17.6 M, both within noise). Rewriting in place with no commit at all costs 11.6 M, but Core Animation only promises to show new IOSurface contents after a commit, and that could not be checked on screen here, so it is not used. A cheaper GPU busy signal was also checked: `AGCInfo` reads in 43k instead of 179k, but its counters stayed 0 while utilization was up to 11%, so it cannot gate the read.

**Drawing in place without a commit: measured, then rejected on screen.** Drawing into the shown surface and never handing Core Animation new contents measured **−18.5% instructions, −19.5% energy, −35% syscalls** in the app (three paired 60 s rounds). But it is only correct if the window server shows the changes promptly. `bench/nocommit-check.m` puts two squares in the menu bar, both changing color every second: one committed (reference), one rewritten in place. Watched on screen with nothing else updating the menu bar, the in-place square changed late and unevenly while the committed one changed every second. The window server does not reliably pick up in-place IOSurface changes without a commit, so a menu bar monitor would show stale values. The code path was removed; the commit per changed frame stays.

Waking once a second and putting a new picture in the menu bar costs 17.4 M on its own. The GPU read adds 13 M, almost all kernel work: the dictionary has only 11 entries, and the request is already a hand-written Mach message parsed in place. Everything else Stats Lite does (CPU and memory sampling, formatting, drawing the strip, history) is the remaining 9 M.

## How the steady state got here

Per-tick cost of the menu bar with CPU, GPU, RAM and disk shown at 1 s, measured in the running app (attribution suite, `bench/results/attrib-v2.log`, then later builds):

| Step | Instructions/tick | Energy mJ/min | Kept? |
|---|---|---|---|
| v1: cropped `CGImage` set on the view's layer | 1.70 M | 12.1 | baseline |
| Exact-size `CGImage` (no crop copy) | 1.58 M | 11.6 | superseded |
| IOSurface as layer contents (no image copy in the CA commit) | 0.72 M | 7.2 | yes |
| + glyph-atlas blitter (no CoreGraphics text per frame) | 0.61 M | 5.2 | yes |
| Dedicated worker thread for sample+render+commit | 0.62 M | 6.1 | **no**: no gain, +17% energy |
| Dispatch timer instead of aligned CFRunLoopTimer | 1.76 M (v1 render) | 12.8 | **no** |
| + repaint only changed cells per pooled surface | strip render 94k → 27k/frame | | yes |

### Menu bar charts

With a module's style set to chart (or text and chart), its chart moves one column left every sample. Redrawing a chart blends every filled pixel of its 54 × 32 pixel box. Now the renderer compares the chart's columns with what the target surface showed: when they are the old columns moved left by k (oldest dropped, or none dropped while the chart is still filling), it moves the pixels with `memmove` and draws only the newest k columns, plus the first column when the old first one was dropped (a first column has no connector line). Every column's pixels depend only on its own height and the previous column's, so the result is exact; a unit test compares stacked, plain and mirrored network charts with full renders over 120 frames, including scale changes, filling charts and a held shown surface.

| Strip frame, CPU/GPU/RAM text + chart, shown surface held | Instructions/frame |
|---|---|
| Charts redrawn | 168k |
| Charts scrolled | 71k |

The first in-app A/B (`results/ab-chartscroll.log`) gained only 1.2 M/min: with 20 s of warm-up, charts were still filling for most of the measured minute, and the first version only scrolled full charts. Handling filling charts gave the 17% above.

**Spike attribution on an idle machine.** A per-phase profile showed about 4.3 M instructions once a minute in the history phase: the spike-attribution baseline (a scan of every process) started once a minute had accumulated "1 s of whole-machine CPU", which an 18-core Mac idling at 7% reaches after about 14 s, so every idle minute paid for a scan whose result was then discarded (the minute's average stayed under 15%). The baseline now starts at the first tick at or above the spike threshold; a minute cannot average 15% without one. Checked live: six `yes` processes for 75 s, the loaded minute is attributed to `yes`.

**Value-only repaints.** When a percentage cell's label is unchanged, only the rows its value's glyph masks cover (old or new string) are cleared and redrawn; the label-row sparkline, if any, scrolls separately. Falls back to the whole cell if the value's and label's ink could overlap. Realistic text strip frame (shown surface held): 44.6k → 35.8k instructions (−20%). A mutation that clears two rows too few fails three tests.

**Sparkline style.** The label row of a percentage cell has about 15 pt free right of "CPU"; the sparkline uses it (30 × 12 px at 2x), two samples per column with the higher kept, pairs anchored to the absolute sample count so a new sample either fills the newest column in place or shifts the line by one. Both cases go through the chart scroll path (the shift check allows the last kept column to change and redraws it). Measured side by side, three copies of the same build over five 30 s rounds: sparklines cost 2% more instructions than text (median ratio 1.02), the text + chart style 8% more (1.08). A unit test covers in-place fills, shifts and steady text with a held surface; a mutation that skips redrawing the filled column fails it.

Copying unchanged-since-last-frame cells from the shown strip surface (the panel's copy-back) was also tried for the strip: no gain (52.9k vs 53.5k per frame), because locking the shown surface (8k) costs as much as redrawing a text cell. Not kept.

Where the remaining steady-state cost sits (attribution variants): run-loop wakeup floor about 0.20 M, sampling about 0.20 M (of which the GPU read is about 0.18 M of kernel work), render plus commit about 0.1 M.

## Sampling: cheapest API per metric

Per call on this machine (`bench/probe.c`, `bench/probe2.c`, `bench/probe3.c`; `results/probe*.txt`):

| Metric | Chosen | Instructions | Rejected alternatives |
|---|---|---|---|
| CPU total | `host_statistics(HOST_CPU_LOAD_INFO)` | 8.3k | `host_processor_info` 45k (used only for per-core in the panel) |
| Memory | `host_statistics64` + `sysctl` on a cached MIB | 8.5k + 8.1k | `sysctlbyname` 17.9k |
| GPU | hand-rolled MIG 2889 + in-place parse | 184k | `IORegistryEntryCreateCFProperty` 216k; `CreateCFProperties` (what the original does) **37M**; IOReport GPU residency **2M** |
| Disk free | `fstatfs` on a cached fd | 6.2k | `statfs` 8.6k; purgeable estimate 5.8 ms (refreshed every 5 min, off the main thread) |
| Disk I/O | MIG 2889 on the cached driver | 46k | CF property 81k |
| Network | `sysctl(IFMIB_IFDATA)` for the primary interface | 7.8k | `getifaddrs` 548k; `NET_RT_IFLIST2` 175k |
| SMC sensor | one key | ~190 µs blocked each | read 4 temperature keys per tick, round-robin |

## Top processes (panel only)

All libproc per-process calls cost about the same (5.9k instructions each, `bench/results`), and no bulk API carries CPU time (`KERN_PROC_ALL` has none). So the gains come from reading fewer processes (`bench/procbench.c`):

| Strategy | Reads/refresh | Instructions/refresh |
|---|---|---|
| Read every own process (what a naive libproc scan does) | 644 | 4.1 M |
| Tiered: hot (≥0.5% of a core) every refresh, warm every 3rd, idle every 8th | 136 | 1.05 M |
| Same, idle every 4th (faster discovery of a process that wakes up) | 196 | 1.41 M |
| Idle every 12th + sweep when unexplained machine CPU rises by more than 0.5 core | 113 | 0.91 M |
| Same, warm every 6th | 89 | 0.76 M |
| Same, warm every 6th, hot threshold 1% or 2% | 86–89 | 0.73–0.76 M |

The warm-every-6th row is the shipped one (`results/procbench4.txt`). Raising the hot threshold saved little more and would demote processes the list should keep fresh. With idle every 12th and the sweep trigger: a unit test starts an idle child process that begins spinning, and it is listed 1.1 to 1.6 s later (0.5 s refreshes), where the 12-refresh period alone would allow up to 6 s (`results/procbench3.txt`).

The original spawns `/bin/ps` (CPU), `/usr/bin/top` (memory) and `ps` again (disk) every second while its popup is open.

## Panel rendering

Same panel content (37 rows, 688 pt) rendered by `src/core/panel.c` (`bench/panelbench.c`, `bench/sparkbench.c`):

| Version | Full repaint | Incremental frame |
|---|---|---|
| First custom renderer | 12.0 M | 2.8 M |
| Plain stores for chart fills and rectangle interiors | 5.6 M | 1.39 M |
| Two layers (static labels/backgrounds, dynamic values) | 5.7 M | 1.00 M |
| Sparklines scroll by whole pixels instead of redrawing | 5.7 M | 0.46 M |
| Corner coverage math only for corner pixels | 5.3 M | 0.42 M |
| Assembly text compositor | 5.2 M | 0.39 M |

The table above renders into whichever surface is free, and every row changes every frame. In the app the compositor holds the surface on screen, so each frame paints a surface that is a frame or more behind; rows also change at different rates (processes and memory every 2 s). The benchmark now models both (`bench/panelbench.c`; `NOHOLD=1` gives the old behavior):

| Version | Incremental frame | Rows painted per frame |
|---|---|---|
| Nothing held (lower bound) | 0.34 M | 7.0 |
| Shown surface held, rows behind are redrawn | 0.47 M | 10.2 |
| Shown surface held, rows behind are copied from it ("copy-back") | 0.43 M | 7.3 |

Rows are also cleared and copied only across the columns the item's text may have inked (its advance plus 3 pt, recorded per surface), instead of the full 600-pixel width: 433k → 416k instructions and about 10% fewer cycles per incremental frame (less memory traffic). A test row alternates between long and short values; narrowing the recorded extent by 1 pt makes the held-surface test fail.

Copy-back copies rows that changed after the stale surface was painted but not in the current frame, about 16k instructions per row against 28k to redraw one. Sparklines are the exception: scrolling one by two samples from the stale surface's own state is cheaper than copying it and then scrolling by one. A unit test holds the shown surface like the compositor, changes rows at three rates, and compares 26 frames with full renders (at most 1/255 off at one sparkline edge pixel, which scrolling by two samples already produced before).

Scrolling only moves rows that can hold ink (the higher of the old and new tallest sample). A low chart (CPU at 5%) moves a handful of rows instead of 68. A unit test scrolls a tall spike in and out of a low chart and compares every checkpoint byte-for-byte with a full render.

One changing sparkline went from 648k to 114k instructions per frame. Unit tests check that the scrolled result is identical to a full render and that partial strip repaints are identical to full repaints.

In the running app with the panel open (CPU, GPU, memory, disk): the AppKit-drawn panel measured 15.6–16.9 M instructions/tick, 7 threads and 127 MB; the custom renderer measured 3.6 M/tick, 3 threads and 32 MB before the scrolling and 2 s process cadence changes.

## Text compositing: scalar, NEON intrinsics, hand-written assembly

`bench/blitbench.c`: five strings plus a full clear of a 600×48 target, 20,000 frames. All three are pixel-identical (maximum channel difference 0). Results are in `results/blitbench*.txt`.

| Compositor | Instructions/frame | Time/frame |
|---|---|---|
| Scalar (256-entry premultiplied color table per pixel) | 60.5k | 3.0–3.6 µs |
| NEON intrinsics, 16 pixels per iteration, scalar tail per row | 50.1k | 2.7–2.9 µs |
| Hand-written AArch64, one call per row | 51.0k | 2.8 µs (call overhead: **lost to the intrinsics**) |
| NEON intrinsics over zero-padded masks (no tails) | 48.6k | 2.8 µs |
| Hand-written AArch64 over zero-padded masks, whole glyph per call (`src/core/blit_arm64.S`) | 39.9k | 2.7 µs |
| **Same, plus: a 16-pixel block whose destination is all zero stores the source directly** | **29.2k** | **2.0 µs** |

The shipped compositor is the last one: 52% fewer instructions than scalar and 40% fewer than the best intrinsics version. Text is almost always drawn onto an area that was just cleared, and "source over zero" is the source itself, so those blocks skip the four destination multiplies; the check is three `orr`s and a `umaxv`. Still pixel-identical (`results/blitbench-zerodst.txt`). It took the realistic strip frame (shown surface held) from 53.0k to 45.1k instructions and the incremental panel frame from 414k to 389k; in the app, panel open measured 3.5% fewer instructions and 5.7% less energy (median of five paired rounds, `quick-ab.sh`). Padding each glyph mask to a multiple of 16 pixels with zero coverage removes the scalar tail from every row; zero coverage blends to exactly the destination, so the output is unchanged. The menu bar strip frame went from 27.3k (scalar) to 24.9k (NEON) to 22.5k instructions (assembly); the incremental panel frame from 420k to 394k.

## IOSurface calls

`IOSurfaceLock` + `IOSurfaceUnlock` cost about 8.2k instructions per frame (`kIOSurfaceLockAvoidSync` makes no difference); `IOSurfaceIsInUse` is about 20 instructions. Unlocking after a write is what bumps the surface seed the compositor watches, so the lock stays.

## Tried and rejected

| Idea | Result |
|---|---|
| Dedicated worker thread (sample, render and commit off the main thread) | Same instructions (0.62 M vs 0.61 M per tick), 17% more energy. Removed. |
| Dispatch timer instead of a CFRunLoopTimer | Slightly more instructions per tick. |
| GPU utilization from IOReport residency instead of PerformanceStatistics | About 2 M instructions per sample, roughly 10× more. Re-checked with a subscription to the single "GPU Performance States" channel: 1.7 M per `IOReportCreateSamples` plus 0.25 M to diff and parse, nearly all of it inside the kernel (a profile shows only `mach_msg2_trap`), so a hand-rolled request could not make it cheaper. |
| Other GPU sources (searched for a cheaper exact utilization) | `PerformanceStatistics` is the only utilization in the I/O registry. Reading it costs 179k instructions: about 43k is the request itself (any small property costs 36–47k) and about 136k is the driver building the statistics. Per-process `accumulatedGPUTime` lives on 65 `AGXDeviceUserClient` objects, one read each (about 2.8 M). `AGCInfo` (43k) stays 0 while the GPU is busy. GPUInfo's `gpustatistics_get_dynamic_statistics` costs 90k and returns an empty dictionary on Apple Silicon. Metal and IOGPU export no utilization; GPURawCounter samples hardware counters through a GPU user client (Instruments' path), far heavier. |
| Second GPU survey | The 187k read splits into a 24k bare Mach round trip (a property that does not exist), about 30k to serialize an 11–12 entry dictionary, and about 130k of AGX driver work. A full-registry diff 10 s apart shows only three GPU-related values that change: `PerformanceStatistics`, `AGCInfo` and per-client `AppUsage`. **IOReport:** every provider and channel tried (GPU performance states, active/idle-time histograms, Fender state, GPU-off abort counter) costs about 1.7 M per `IOReportCreateSamples`, a fixed overhead of the IOReport path; the "Energy Model" group costs 27 M. **Resource coalitions:** `coalition_info_resource_usage` exposes `gpu_time` unprivileged, but there are 759 resource coalitions (9.4 M instructions per sweep) and the sum is overlapping engine time (133–161% of wall time under load). **SMC power keys** (PZC0, PHPC, PPSC, … rise under GPU load): 61k per key read, and power is not utilization. **IOKit interest notifications** on the accelerator: busy-state messages arrive only when a GPU client process exits, never per burst, so they cannot drive event-based sampling. **kdebug tracing:** EPERM without root. **AGX user-client selectors or mapped counters:** no documented ABI; probing private selectors on a production machine was not attempted. Conclusion: the hand-rolled `PerformanceStatistics` read stays the cheapest exact source. |
| Sampling GPU every 2–3 s when idle | The driver's "Device Utilization %" is a short moving average (it drops from 88% to 5% within 1 s of a load stopping; `bench/gpuavg.m`, `results/gpuavg.txt`), not a counter since the last read, so bursts between reads would be missed. |
| Loading UserNotifications with `dlopen` on first use instead of linking it | No measurable change (34.1 vs 35.2 M instr/min, same footprint). Reverted. |
| `malloc_zone_pressure_relief` after startup | No footprint change (14 MB either way); the small-malloc pages are live AppKit/CoreText state. |
| Full process scan every refresh | 4.1 M instructions; replaced by the tiered scan. |
| Repainting per-core bars individually (only bars whose height changed) | The per-core row is about 20% of an incremental panel frame, but per-bar clears cost more than one band clear: +20k instructions per frame when every bar changes, break-even when half do (`bench/panelbench.c` variants). Reverted. |

## Things that turned out to matter for measuring

- Rebuilding and re-signing an app bundle with the same bundle ID while an instance runs makes LaunchServices notify it, and it does a lot of extra work for a while. No bundle builds during runs.
- `defaults write` immediately before launch can be read stale; knobs are passed as launch arguments.
- WindowServer CPU time (`ps`, 10 ms resolution) moves with whatever else is on screen; it is reported against interleaved no-app control runs and treated as secondary.
- A stray load generator (`yes`, started for a chart render test) kept one core busy during the v3 suite and the v3/v4 A/B. All apps in those runs saw the same load (runs were interleaved), but the headline table uses a clean re-run of the final build.
