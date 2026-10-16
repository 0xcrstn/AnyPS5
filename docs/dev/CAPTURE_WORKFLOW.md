# Capture workflow

The capture tools record a run's declared inputs, collect AnyPS5 trace output, assess its limits, and compare repeated runs. They use the Python standard library. Run the commands below from the repository root. The tools do not build AnyPS5, relink a title, prepare its runtime, or reset caches. Detailed capture requires a build containing the opt-in producer described below.

Source delivery and authored tests do not establish that these tools work on a title or that a change improves performance. Compilation, tests, runtime preparation, and game execution are separate validation steps. Obtain any approval required by the workspace before executing them. The commands below describe that later, explicitly authorized use.

## Configuration

Save a JSON configuration outside the source repository. All input paths and the execution working directory must be absolute. Name the executable and each library explicitly; the tools do not discover executables or recursively inventory game assets. For example:

```json
{
  "source_root": "D:/AnyPS5/src",
  "runtime": {
    "executable": "D:/AnyPS5/runtime/title.exe",
    "libraries": ["D:/AnyPS5/runtime/libs/libSceAgcDriver.prx"]
  },
  "build": {
    "compiler": "GCC 15.2.0",
    "build_type": "Release",
    "options": {"ANYPS5_USE_SYSTEM_DEPS": false}
  },
  "machine": {
    "id": "capture-pc",
    "cpu": "Intel Core i9-13900K",
    "gpu": "NVIDIA RTX 5090",
    "driver": "32.0.16.1742",
    "os": "Windows 11 Pro"
  },
  "title": {"id": "local-title", "version": "1.0"},
  "scene": {
    "id": "menu-idle",
    "recipe": {"steps": ["Open the main menu", "Wait without input"]}
  },
  "settings": {
    "resolution": "1920x1080",
    "presentation": "windowed",
    "cache": {
      "shader": "warm",
      "pipeline": "warm",
      "driver": "warm"
    },
    "diagnostics": {"mode": "measurement", "options": {}},
    "relinker": {"flags": ["--windows", "--to-intel", "--registry"]}
  },
  "capture": {"unit": "frames", "start": 600, "length": 300},
  "execution": {
    "command": ["D:/AnyPS5/runtime/title.exe"],
    "cwd": "D:/AnyPS5/runtime"
  },
  "environment": {"overrides": {"APS5_STORE_AT_FLIP": "1"}},
  "experiment": {"id": "menu-comparison", "pair_id": "repeat-01"}
}
```

Use the actual title, content version, scene, machine, build options, libraries, and cache policy. These fields describe conditions; they do not automate scene input, set the resolution, rebuild with the listed flags, or prepare caches. The recipe is hashed from canonical JSON. A cache location or label alone does not prove equivalent cache contents; record stable identities in the cache metadata when the experiment needs them. Diagnostic mode is `measurement`, `attribution`, or `forensic`; record the options there and enable their variables in `environment.overrides`. GPU profiling is not implicitly enabled.

The runner removes inherited `APS5_`, `ANYPS5_`, and `VK_` variables, case insensitively, from the child environment before applying `environment.overrides`. A null override removes a variable. It records relevant child variables and the names of removed inherited variables, without saving unrelated environment values. The parent environment is unchanged. Capture configuration owns `APS5_TRACE_DIR`, `APS5_TRACE_FRAMES`, and `APS5_TRACE_SECONDS`; do not override those names. `frames` derives `APS5_TRACE_FRAMES=start:length`; `seconds` derives `APS5_TRACE_SECONDS=start:length`.

## Capture and evidence

Choose a new run directory outside `source_root` whose parent already exists. Evidence inside the source checkout would change its untracked-file fingerprint. Existing directories are rejected, including an earlier partial run. The manifest-only command records inputs without starting a process:

```powershell
python tools/capture_manifest.py --config D:/captures/config.json --run-dir D:/captures/planned-01
```

Use a different new directory for the opt-in runner:

```powershell
python tools/capture_run.py --config D:/captures/config.json --run-dir D:/captures/baseline-01
```

The command is an argument array, executed without a shell. `manifest.json` is written before spawn. Standard output and standard error go to `run.log`, and the process gets a unique `traces/` directory. The runner finalizes the manifest, inventories selected evidence, verifies declared runtime files again, and writes `health.json` on ordinary exit, launch failure, or handled interruption. An abrupt runner or machine failure can leave a partial manifest and evidence. Preserve that directory for diagnosis and use a new directory for another run.

The capture window controls recording; it does not stop the game. A handled user interrupt and any subsequent termination are recorded separately from normal process completion. A failed or interrupted run cannot pass strict comparison, even when it contains a useful trace.

The source fingerprint describes the inspected checkout, including dirty changes, Git nonignored untracked files, and submodule state. Ignored inputs are excluded, and an uninitialized submodule has no observed checkout identity. The fingerprint does not prove that checkout produced the executable or libraries. Build configuration is declared metadata; an optional `build.receipt` hashes an existing receipt file and records its identity. A receipt is only as reliable as the build process that created it.

Runtime inventory contains hashes, sizes, and paths for the explicitly declared files. It has `inventory_kind=declared_files` and `loaded_modules_verified=false`: it does not prove which modules the process loaded. Pre-run and post-run hashes can detect observed changes, but cannot prove a file was unchanged throughout execution. Later health and comparison use those recorded boundary checks and the preserved capture artifacts; replacing or removing the original runtime does not invalidate historical evidence. Missing boundary checks prevent a strict comparable verdict. No game binaries or assets are copied into evidence. Do not include platform system-library dumps in the inventory. Manifests, logs, traces, and receipts can still expose local paths or identifying information; review them before sharing. The tools do not upload evidence.

## Health assessment

```powershell
python tools/capture_health.py D:/captures/baseline-01
```

Assessment reads the manifest and selected summary/trace artifacts. It rejects malformed or duplicate-key JSON, nonfinite metrics, paths outside the run directory, recorded hash mismatches, ambiguous summary selection, and incompatible capture windows. It does not choose the newest file from mixed sessions.

Frame timing, detailed attribution, and provenance are assessed separately. Missing evidence is unknown or unusable, never a pass. `trace_dropped=false` only reports the producer's drop flag. It does not establish per-thread coverage or complete causal attribution. Legacy traces fold CPU slices shorter than 20 microseconds, omit program ranges, and may omit GPU work collected after the window closes. Their closing present boundary is absent from trace instants, so the trace cannot independently reconstruct every summary frame interval. Missing GPU tracks and unavailable metrics remain visible; unavailable metrics are not zero. Legacy seconds windows retain unknown timing usability because their absolute opening time and overshoot cannot be independently checked; strict comparison remains insufficient. Use fixed-frame windows for strict comparisons with the legacy producer. Health does not render, replay, or import target binaries.

## Repeated comparisons

Collect independent repetitions with the same title, scene recipe, machine, settings, relevant environment, capture window, cache policy, working directory, and command arguments. Use one `experiment.id` and matching `pair_id` values when explicitly pairing baseline and candidate repetitions. Each repetition needs a new run identity and independently captured evidence. Keep source and declared runtime identities consistent within each cohort; the intended code and binary changes may differ between cohorts.

```powershell
python tools/capture_compare.py --baseline D:/captures/baseline-01 --baseline D:/captures/baseline-02 --candidate D:/captures/candidate-01 --candidate D:/captures/candidate-02 --json
```

Comparison recomputes health from evidence instead of trusting a saved `health.json`. It requires recorded child-process identity, successful runtime checks before and after execution, a matching producer revision, known shader/pipeline/driver cache states, and usable frame timing. A matching abbreviated producer revision only correlates a commit prefix; it does not establish source-to-binary binding. Observed GPU, driver, OS, and resolution must be present and agree across captures as well as the declared configuration.

The verdict is `invalid`, `incomparable`, `insufficient`, or `comparable`. Invalid evidence cannot support comparison; incompatible configurations or reused evidence cannot isolate independent repetitions. One pair is insufficient to establish a repeatable gain. Comparable repetitions permit descriptive run-level deltas and within-cohort variation, without an automatic performance-win or statistical-significance verdict. Individual frames are not treated as independent experimental runs, and arbitrary input order does not create pairs. The comparison CLI returns 0 for comparable, 1 for insufficient, and 2 for invalid or incomparable.

Bare legacy `aps5-summary.jsonl` files remain supported by [performance_compare.py](../../tools/performance_compare.py) for numeric comparison. They lack the provenance and experiment metadata needed by the strict workflow. Do not add metadata after the fact to turn a legacy summary into verified capture evidence.

## Detailed draw evidence

Detailed draw capture is opt-in producer instrumentation. Its implementation and authored tests require separate approved execution validation; existing captures do not validate a newly instrumented build. Legacy summary and trace files remain readable and retain their attribution limitations.

The declared scope covers guest graphics operations and their recorded direct, indexed, mesh, indirect, replay, backface, and channel host commands. Indirect commands can remain grouped or expand into individual CPU-read records according to the normal path. Guest compute direct and indirect dispatches, including base-offset expansions, are recorded. Presentation has a shared whole-blit interval. Named host scopes cover the instrumented submission, packet, preparation, pipeline, recording, and wait paths; they do not provide automatic guest-function timing or ownership by guest module.

Earlier detailed sidecars exclude recorder-internal sample-dump/publish, sample-fold/publish, and mesh-argument rewrite kernels from individual host-command and shader attribution. The additional source instrumentation described below supplies declared builtin identities for these helpers; earlier evidence cannot be upgraded retroactively. Presentation subcommands remain grouped inside the shared blit interval.

To request detail, set `settings.diagnostics.mode` to `attribution` and add `"trace_detail": "draw"` to `settings.diagnostics.options`. For example, replace the diagnostics object in the configuration above with:

```json
"diagnostics": {"mode": "attribution", "options": {"trace_detail": "draw"}}
```

The equivalent explicit environment override is `"APS5_TRACE_DETAIL": "draw"`. Conflicting declarations are rejected. The manifest derives the effective opt-in variable and `APS5_CAPTURE_RUN_ID`; the latter is runner-owned and must not be supplied as an override. Capture window variables still come from `capture`. Use the same `capture_run.py` command with a new run directory. Opting in does not enable the separate legacy `APS5_PROFILE_GPU` mode.

The producer writes `traces/aps5-draw-<run_uuid>-<pid>.json` alongside the legacy trace and summary. This bounded sidecar records a distinct capture UUID, the selected legacy trace basename, every opening/intermediate/closing present boundary, thread accounting, canonical entity and event IDs, metadata dictionaries, asynchronous links, GPU query outcomes, loss counters, and finalization state. Repeated records can refer to one operation or query; each event record has its own ID. Explicit references to work outside the window leave origin coverage unknown without erasing independently recorded shader identities or intervals. A missing sidecar after requesting detail, mismatched linkage, conflicting dictionaries, or malformed evidence cannot supply valid detailed attribution.

Shader and pipeline dictionaries record identities, hashes, stage labels, compiler/cache context, and links. They do not embed guest shader instructions, executable contents, or game assets. Process-local handles and object IDs identify relationships within one capture; they are not stable identities across runs. Content hashes support matching recorded inputs without proving semantic equivalence or which modules were loaded. Review the sidecar and its metadata before sharing it.

Operation lifecycles include skipped, rejected, cancelled, and failed attempts. Their presence is not an executed-draw count. One guest operation can expand into several host commands, while a GPU indirect command can represent a group with a dynamic count. Read the command grouping, requested counts, outcome, and query association before attributing an interval to an individual draw.

Inspect the independently recomputed index with:

```powershell
python tools/capture_draws.py D:/captures/baseline-01
python tools/capture_draws.py D:/captures/baseline-01 --json
```

The runner saves compact `draws.json` and `draws.txt` reports by default, recording their hashes as derived artifacts. Compact assessment keeps function totals, bounded present-interval summaries, counts, and coverage without constructing a full row for every host span. Set `settings.diagnostics.options.report_detail` to `full` to request the full derived index. The full index links operations, shader/pipeline metadata, host spans, and GPU queries to canonical `/events/<index>` source records. `health.json` remains compact. The canonical source events are preserved in either mode; compact reporting is not event sampling.

Saved reports and `health.json` are not trusted as input. Shader/pipeline identity, origin-graph coverage, CPU scope completeness, and GPU query completeness have separate `valid`, `unknown`, or `invalid` states. An unknown pre-window origin does not erase independently valid host intervals, GPU measurements, or frame timing. Comparisons report each dimension's availability and include only its validated metrics. A valid detail verdict means complete evidence within the declared instrumented scope; it does not verify rendering correctness, gameplay, loaded modules, or a performance gain. The report CLI returns 0 for valid detail, 1 for unknown, and 2 for invalid evidence.

Declared record loss retains a diagnostic index with attempted, recorded, and lost counts while making complete attribution ineligible. An intact observed span or terminal query result can remain visible, but missing lifecycle endpoints have unknown completeness and unavailable elapsed values. Event loss also makes host self time unavailable because missing children cannot be subtracted safely. Observed command counts are separate from complete-window workload metrics. Loss does not excuse contradictory identities, duplicate lifecycle records, or malformed JSON; these remain structural errors. A valid legacy frame window can coexist with invalid detailed evidence.

Host scope durations are elapsed wall time between recorded endpoints. They include scheduling delays and waits; they are not CPU utilization or processor time. GPU draw ranges measure the recorded whole draw, which may execute several shader stages. Associating a range with shader or pipeline identities does not isolate a shader stage or a function inside a GPU program. Compilation and shader preparation spans describe host work, separate from GPU execution.

Host self time subtracts the union of recorded nested intervals on the same thread. Asynchronous work remains linked to its scheduling operation without being subtracted as same-thread self time or assigned an exclusive frame. Boundary-crossing scopes remain indexed and are excluded from complete-window function totals. GPU intervals can overlap each other and CPU work; their sum is not a wall-time utilization percentage. Unsupported, failed, pending, or uncollected queries have unavailable durations. A shared range does not establish an exclusive duration for each draw.

Function totals carry individual availability states. A clipped or boundary-crossing scope does not supply a complete total for that function, while other fully recorded functions can remain available. Missing coverage or lost records still prevents claiming complete function totals.

A query's measured interval and its association with a particular operation are separate evidence. An available interval can remain unassociated; it must not become a per-operation duration until the recorded links support that association. Host present-interval membership describes timestamp overlap and does not establish ownership of a displayed frame. GPU intervals are not assigned to host present intervals without a verified clock mapping.

New available GPU query records include decimal-string `begin_gpu_ticks` and `end_gpu_ticks`, `period_ns`, and a `duration_ns` computed from the integer tick difference before conversion. `duration_origin` identifies this as `integer_tick_delta`. With `timestamp_valid_bits` equal to 64, the delta is computed modulo 2^64 and `timestamp_wraps` records whether the raw end precedes the raw beginning. A small interval crossing that wrap is supported; a delta of at least 2^63 ticks is ambiguous and rejected. The reader recomputes the duration and rejects contradictory precision fields. Absolute nanosecond coordinates remain explicitly `approximate_binary64`; subtracting them can lose a small interval when the underlying timestamps are large. Legacy endpoint-only intervals remain approximate and rounded-equal endpoints do not prove a hardware zero. Equal raw ticks establish a measured zero at the reported timestamp-period granularity. These duration fields do not establish GPU-to-CPU clock alignment.

Clock calibration records include Vulkan's reported uncertainty plus the measured bracket used to convert the native host clock to the trace clock, with a rounding allowance. This describes the sampled conversion uncertainty; it is not a complete proof of synchronization accuracy or causality.

Completion applies only to the document's declared scope. A complete set of instrumented draw and host-scope records does not cover every guest function, uninstrumented thread, hardware operation, or causal dependency. Missing query results, buffer overflow, unclosed spans, dictionary loss, and incomplete asynchronous collection must remain visible. Idle observations may be clipped at window close without holding the drain open; their recorded elapsed interval does not establish a completed wait. Unavailable durations are not zero.

The default event ceiling is 65,536 records per registered thread, with a shared 512 MiB event-payload budget and a global ceiling of 8,388,608 retained events. `APS5_DRAW_TRACE_MEMORY_MB` accepts 64 through 8192 MiB and defaults to 512. Storage grows in lazy 1024-event chunks, with shorter tails at a per-thread ceiling; thread registration no longer reserves the full per-thread ceiling. Active threads compete for the shared payload budget, and any exhausted ceiling or budget still causes explicit loss. Coverage records the effective `maximum_events`, `chunk_records`, `allocated_record_bytes`, and `allocated_index_bytes`; `budget_scope` is `stored_event_chunk_payloads`. Index, metadata, and query storage are outside this payload budget, so it does not bound every process allocation or guarantee complete capture. `APS5_DRAW_TRACE_EVENTS` can request 1 through 8,388,608 records per thread within that budget. `APS5_DRAW_TRACE_METADATA` accepts 1 through 262,144 dictionary entries, and `APS5_DRAW_TRACE_QUERIES` accepts 1 through 1,048,576 queries for the session. The producer streams final JSON instead of building the full document in memory.

The duplicate Perfetto `traceEvents` view is omitted by default. Set `APS5_DRAW_TRACE_PERFETTO=1` to include it; `coverage.perfetto_view_included` records the decision. Canonical `events` remain authoritative. The reader accepts older sidecars containing the duplicate view and validates then discards that view. It accepts at most 8,388,608 canonical events and 262,144 dictionary entries, with a 16 GiB file ceiling and a 16,777,216-character individual-item ceiling. Larger documents are rejected explicitly, not silently truncated. Compact assessment reduces derived storage but still retains canonical event objects in memory; it is not a fully streaming analysis or a promise that every allowed capture fits available memory. GPU recording admits up to 512 command query pairs per batch. Present boundary storage is bounded at 65,536 entries. Timestamp queues with fewer than 64 valid bits are currently unsupported for detailed GPU intervals. Capacity refusal, allocation failure, missing availability, invalid timestamps, and uncollected results remain explicit evidence limitations. New collection and reporting paths need measured overhead validation.

The producer must preserve the configured normal execution path. Profiling must not silently replace parallel emit or chunked recording with an inline path. Unsupported instrumentation is reported as unavailable instead of being presented as timing of the intended path. Capture-window closure and collection finalization are separate boundaries: GPU results can arrive after recording closes. Instrumentation overhead and any change to scheduling still need measured validation before using detailed captures to make a performance claim.

## Additional evidence and execution gate

Validation proceeds through source review, focused builds and tests, then separately approved runtime preparation, collector execution, and captures. Synthetic tests check behavior and producer/consumer contracts; they do not establish game rendering correctness or instrumentation overhead. Build and test approval does not authorize preparing or launching the game. A plan, configured flag, successful source check, or saved report does not establish that a collector ran. Existing captures do not validate newly added paths.

Detailed records now include native operating-system thread IDs and a bracketed native-clock anchor. `APS5_DRAW_TRACE_THREAD_CPU=1` requests inclusive user-plus-kernel processor time for selected host scopes; it defaults off. On Windows its 100 ns tick unit does not imply 100 ns measurement resolution; short scopes may return zero deltas. `APS5_DRAW_TRACE_THREAD_CYCLES=1` independently requests raw Windows `QueryThreadCycleTime` begin/end/delta readings for the selected scopes. Cycle readings are inclusive of recorded nested work and instrumentation; they are not elapsed nanoseconds, exclusive function cost, or an estimate for uninstrumented functions. The reader never converts them to time. Both options default off, and unavailable platforms, failed readings, changed thread identity, nonmonotonic readings, and clipped observations remain explicit. Elapsed scope time, inclusive thread processor time, thread cycles, scheduler-running time, and sampling estimates remain distinct measurements. Subtracting processor time from elapsed time does not prove a blocking/preemption split.

`APS5_DRAW_TRACE_JOURNAL=1` requests an incremental JSONL journal; it defaults off. The background writer uses bounded 4096-event batches, a 100 ms idle poll, and a 16 GiB output ceiling. Only newline-committed records form a recoverable prefix. Failure and budget exhaustion remain visible. There is no checksum or power-loss durability guarantee, and a recovered prefix never becomes a complete capture. Normal finalized sidecars retain atomic publication and separate recording-close and drain boundaries.

Recover a preserved journal into a separate diagnostic report with `python tools/capture_recover.py D:/captures/interrupted-run --json-output D:/reviews/recovered.json`. For a crash before artifact inventory publication, `--journal` accepts an explicitly selected contained journal path. Recovery retains committed evidence and its incompleteness; it does not repair or replace the original sidecar or turn a failed run into a successful comparison sample.

`APS5_TRACE_GPU_LABELS=1` requests Vulkan debug labels with run, capture, operation, batch, pipeline, and command identifiers. The bounded label buffer reports refusal rather than truncating an identity silently. `APS5_TRACE_GPU_COUNTERS=1` requests supported standard pipeline-statistics counters, with at most 512 commands per batch and 64 cached counter pools. Both require detailed capture and default off. Collection uses ordinary fence retirement and availability without a forced query wait. Unsupported mesh statistics and occlusion where guest queries conflict remain unavailable. Internal sample-dump, fold/publish, and mesh-argument helper dispatches have separately declared builtin SPIR-V identities; these do not invent guest shader or compiler ownership. Presentation remains a shared whole-path range.

Native lineage links and wait outcomes describe observed relationships between submission, packet, job, batch, completion, flip, and present records. Preparation, cache outcomes, already-read indirect arguments, copies, and compilation counters are available only where the producer records them. A cache hit does not prove global cache warmth. The evidence report separates dependency coverage, clock mapping, selected host elapsed/processor time, sampling, scheduler-running time, waits, workload, copies, cache activity, compilation, GPU intervals/counters, and external presentation evidence. Each metric carries availability and units. Unknown remains a valid result; missing work is never a zero.

## Process observations and external measurements

An optional `observation` object in the capture configuration enables bounded process sampling. For example:

```json
"observation": {
  "enabled": true,
  "interval_ms": 1000,
  "max_samples": 4096,
  "max_output_bytes": 67108864,
  "hash_roots": ["D:/private-runtime"],
  "effective_settings_files": [],
  "cache_locations": [],
  "build_receipts": [],
  "symbol_receipts": [],
  "profiling_evidence": []
}
```

This records sampled process creation/executable identity, mapped modules, native threads, and resource observations. Explicit hash roots and byte budgets limit which mapped files can be hashed. Sampling does not establish continuous module lifetime or every transient mapping. Declared runtime files, observed mapped images, and verified source-to-binary provenance remain separate facts. Cache snapshots cover only explicitly selected application files and do not delete or reset caches. Build/symbol receipts hash explicit files and settings; a declared pairing alone does not verify a debug identifier, resolve functions, or prove the compiler invocation.

Process sample output uses unbuffered UTF-8 bytes and LF delimiters. The byte budget applies to those actual bytes, including on Windows. A short write or output error permanently ends further sample writes, preserves any partial tail, and reports `sampling_status: "output_failed"`; `sample_bytes` records the actual file size and `complete_sample_bytes` counts only fully written records. Child exit and observer failure remain separate facts. This is not an `fsync` or power-loss durability guarantee. Earlier observer reports remain readable and are not retrospectively certified by the corrected byte accounting.

An optional `collectors` configuration runs a bounded Windows WPR CPU collector around the retained child process. Omit it or use `{"enabled":false}` to keep external collection disabled. Enabling it is an explicit collector execution request and must follow the workspace execution gate:

```json
"collectors": {
  "enabled": true,
  "kind": "wpr_cpu",
  "wpr": "C:/Windows/System32/wpr.exe",
  "profile": "CPU",
  "max_duration_seconds": 120,
  "max_trace_bytes": 536870912,
  "command_timeout_seconds": 30
}
```

The runner uses a unique instance name, records tool identity and command output, checks visible session identities, and stops only sessions it can identify as its own. It does not elevate, install tools, change symbol servers, or cancel an existing recording. Start/stop failures and unresolved cleanup are retained in the collector receipt and `execution.collector_errors`. A requested collector failure cannot silently become a successful profiling run, although independently valid native frame timing can remain useful. File and duration limits are polled bounds, not kernel-enforced hard limits. Collection begins after spawn and therefore cannot certify the process's complete startup lifetime.

Enable `observation.enabled` alongside collection to preserve independent retained-child process and sampled module evidence. A collector receipt alone cannot satisfy those identity checks. Collector polling uses a separate short cadence from the configured process sampling interval; synchronous sampling and collector commands can still delay a polled limit.

To export CPU rows automatically, add `collectors.export` with `tool` naming an existing `wpaexporter.exe`, `profile` naming an explicitly saved `.wpaProfile`, and `table_file` naming the actual exported CSV basename. Optional `mapping` names an existing `anyps5.table-mapping` JSON file. The runner preserves ETL, CSV, mapping, command logs, and normalized records where available. It does not invent WPA table headers, process-relative clock anchors, loss accounting, or complete coverage from a successful command. Imported rows must still pass process, module, clock, and coverage checks before per-frame attribution.

Historical collector assessment reads the preserved receipt and artifact hashes instead of requiring the original tools to remain installed. The collection policy and recorded tool identities are comparison conditions: the same path with different tool bytes is not equivalent. Missing requested collector evidence prevents strict experiment eligibility. An explicitly declared enabled-versus-disabled collector overhead experiment remains possible; successful receipt verification establishes recorded identity and lifecycle facts, not tool authenticity or exhaustive profiling coverage.

For future optimized profiling builds, `-DANYPS5_CAPTURE_SYMBOLS=ON` adds debug information to AnyPS5 core C/C++ targets on supported GCC/Clang toolchains. It does not change the chosen optimization level or create names for stripped guest code. Preserve the exact executable/library and matching debug files locally; no game binaries are attached to reports. The symbol resolver verifies image identity and either embedded DWARF, a matching GNU debug link, or a matching PE/PDB debug identifier. A matching PDB identity alone does not extract function records. Unsupported debug formats, absent names, dynamic code, and unverified addresses retain module-plus-offset or unresolved results.

`capture_symbols.py` reads symbol identity metadata and can request bounded exact-address lookups from an explicitly selected installed `addr2line`. Use RVAs from the sampled module-offset report, not guessed function ranges. For example, replace `0x1234` below with an observed RVA and retain command output inside the capture directory:

```powershell
python tools/capture_symbols.py --image D:/private-runtime/libs/libSceAgcDriver.prx --rva 0x1234 --addr2line D:/toolchain/bin/addr2line.exe --command-output-dir D:/captures/run-01/external/symbol-command-01 --output D:/reviews/symbols-01.json
python tools/capture_symbols.py attach --run-dir D:/captures/run-01 --receipt D:/reviews/symbols-01.json
```

The first command may execute the selected lookup tool and requires the corresponding execution approval. Add `--symbols` only for an explicit matching separate debug file. Lookup outputs, tool identity, and exact argv are preserved; the resolver never downloads symbols or estimates a function's end from the next symbol. Attachment requires a finalized run and an independently observed matching module, preserves the previous manifest, and inventories the receipt and lookup outputs. Fresh assessment rechecks file/debug identity and recorded output before using names in sampled-function reports. Sample hits remain sampling observations rather than function invocation durations. Missing verification inputs leave names unknown, while contradictory hashes or ranges invalidate the symbol evidence.

External profiles are attached as normalized `anyps5.external-evidence` version 1 documents plus their hashed raw source. The importer requires process identity and source references, plus timestamp-domain evidence for timed records. Aggregate `shader_metrics` rows have no timestamps and cannot supply per-frame timing. The importer does not trust a label containing a PID or a declared flag as proof. The `capture_process.py attach-profile` command preserves the previous manifest and uses the shared exclusive attachment lock. External records may support CPU samples/context switches, GPU counters, or presentation measurements when their bindings and units are valid. Addresses outside independently observed module ranges remain unresolved; no guest function is invented.

Normalized ETW/WPR and Nsight documents are interchange records, not native ETL or proprietary report parsers. Preserve the original collector output, extractor identity/version, exported fields, process binding, and timestamp units. A source hash verifies preserved bytes, not an unverified extractor's semantic accuracy. `capture_external.py normalize-presentmon` supports the documented CSV path; unsupported formats remain unavailable rather than being guessed. Generate fresh local JSON/HTML evidence reports with `python tools/capture_evidence.py D:/captures/run-01 --json-output D:/reviews/evidence.json --html-output D:/reviews/evidence.html`. The HTML keeps selected source-event references and explicit unavailable dimensions.

`capture_tabular.py` provides an explicit adapter for exported CPU-sample, context-switch, and GPU-counter CSV tables. Its `anyps5.table-mapping` version 1 records actual tool/version, table name/record kind, CSV dialect, clock domain/input units/frequency, and a mapping from normalized fields to real column headers. It preserves row/line references and the mapping digest, filters the selected PID, and refuses unsupported or fractional timestamp conversions rather than inventing anchors. Table size is bounded at 64 MiB and 262,144 rows. Aggregated tables without required timestamps cannot become per-frame measurements; absent coverage/loss/symbolization facts stay unknown. A declared instruction address and symbol remain exported claims until independent image/debug-identity evidence validates them.

Use official collectors only after execution approval. [WPR](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options) supports CPU recording; an experiment packet must specify a bounded stop, especially for file mode. [PresentMon's documented CSV](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md) can supply presentation observations with explicit process/time joins. [Nsight shader profiling](https://docs.nvidia.com/nsight-graphics/UserGuide/shader-profiler.html) supplies samples and stall/callsite information, not exact timing of every invocation. [LiveReplay measurements](https://docs.nvidia.com/nsight-graphics/UserGuide/graphics-capture-ui.html) describe replay work and must remain labeled replay. These measurements cannot silently replace original live timing or certify every shader stage and guest function.

`capture_gpu_profile.py` prepares a documented Nsight Graphics GPU Trace command packet and imports preserved Shader Profiler CSV. It does not execute NVIDIA tools. A request uses `document_type: "anyps5.gpu-profile-request"`, `schema_version: 1`, exact run/capture/process identities, a hashed native draw artifact, and explicit native command/pipeline/shader targets. Each target supplies `operation`, `batch`, `command`, `pipeline`, `shader_variant`, `spirv`, `stage`, and `spirv_sha256`. The request also declares a tool version, `scope` (`live`, `replay`, or `unknown`), and bounded `collection` settings; recorded executable/version evidence is assessed separately. `limit_to_frames` is 1 through 15 tool-defined presents; it is not assumed to equal the native 100-frame window. IDs from a completed capture cannot predict command IDs in another launch.

```powershell
python tools/capture_gpu_profile.py --run-dir D:/captures/run-01 plan --request external/gpu-request.json --ngfx D:/tools/Nsight/ngfx.exe --target-exe D:/private-runtime/title.exe --working-dir D:/private-runtime --output-dir D:/profiles/session-01 --output external/gpu-packet.json
python tools/capture_gpu_profile.py --run-dir D:/captures/run-01 normalize --request external/gpu-request.json --source external/shaders.csv --mapping external/shader-mapping.json --output external/shader-profile.json
```

Before a later authorized execution, preserve the installed tool's version and `--help-all` output and verify support for the packet's options and architecture-specific metric set. The packet requests unchanged GPU clocks. Shader CSV export uses the tool's documented **Save as CSV** action; the adapter does not assume that CLI auto-export emits this table. An `anyps5.nsight-shader-mapping` version 1 document maps the real CSV headers, row types, shader stages, and metric columns with explicit units and unavailable-value tokens. Hierarchical aggregate and pipeline rows are excluded from shader-row totals. Warp/stall samples and hardware counters retain their original meaning; a vendor shader hash is never treated as a SPIR-V SHA-256 hash.

The normalized `shader_metrics` rows intentionally have no fabricated timestamp. Raw CSV, request, mapping, and optional collector receipt must be preserved and attached with the profile; assessment rechecks their bytes and recomputes the transformation. Only actually exported labels/digests can join an observed row to a native target. The native final-SPIR-V identity uses the `anyps5.final-spirv.v1` domain, word count, and little-endian words; it is not the SHA-256 of an arbitrary exported file. Missing that exact identity remains an unresolved shader join. Declared IDs in a request, a matching file hash, or a successful transformation do not prove the selected range, original live process, sampler loss, or exact stage duration. GPU stage overlap and compiler transformations prevent a whole-draw timer from supplying exact time for every shader function. Replay and aggregate results remain separate evidence.

## Lightweight workload observations

Set `APS5_TRACE_WORKLOAD=1` in `environment.overrides` to record workload counters alongside a fixed-frame trace without enabling detailed draw recording. The runner assigns the run UUID in measurement, attribution, and forensic modes. The producer writes `traces/aps5-workload-<run_uuid>-<pid>.json.gz`, linked to the legacy trace basename, producer revision, PID, and exact opening/intermediate/closing present boundaries. The option defaults off and supports at most 65,536 intervals.

The ledger counts requested draw and dispatch operations, indirect records, emitted host draw/dispatch commands, terminal outcomes, and asynchronous job lifetimes. Counts are serialized into host-present buckets; they do not identify ownership of a displayed GPU frame. Pending operations, commands, and jobs at window edges, unmatched lifetimes, counter overflow, or lost intervals prevent complete-window workload eligibility. Their observed counts remain visible. No extra GPU wait or different recording path is introduced to make a window appear complete.

Health verifies compressed-file bounds, exact frame boundaries, per-frame/totals consistency, lifecycle balance, identity, and linkage independently of shader attribution. When both lightweight and detailed counts exist, contradictory comparable counts block workload equivalence. Strict experiments can use complete lightweight counts for measurement runs. Equal counts still do not prove identical scene content, rendering, indirect GPU execution counts, or cache state; those retain separate evidence requirements. The new counter path's runtime cost remains to be measured.

## Reviewed scene, readiness, cache, and correctness

Post-run condition evidence uses one receipt format: `anyps5.capture-conditions`, `schema_version: 1`, the exact `run_id`, `capture` object, optional native `capture_id`, and `claims`. Supported claim names are `scene`, `readiness`, `cache.shader`, `cache.pipeline`, `cache.driver`, and `correctness`. Missing claims and `{"state":"unknown"}` remain unknown. A declared native capture UUID must match a freshly decoded sidecar before its claims become eligible.

Known claims require `method: "artifact_review"`, a local `reviewer`, `reviewed_at` in UTC after the run ends, explicit `observations`, `scope: "whole_window"`, and `criteria` entries with `id`, `result` (`passed`, `failed`, or `unavailable`), and `observation`. They also require contained manifest artifact references with `path`, `sha256`, `size`, `role`, `media`, and `coverage: {unit,start,length}`. Receipt assessment checks bytes, bindings, stated coverage, and explicit review results. Accepted claims have `basis: "reviewed_claim"`; they are not automatically observed truth. Merely hashing a file never makes a criterion pass.

Scene claims use `state: "matched"`, `scene_id`, and the canonical `recipe_sha256`; readiness uses `state: "ready"` and a named `checkpoint`. Correctness uses `state: "preserved"`, `reference_identity`, and both `actual` and `reference` evidence covering the window. The same artifact path cannot serve both roles; distinct artifacts may have equal hashes when their captured pixels match. A single image covers one frame; a sampled image cannot certify a whole 100-frame window. A reviewer must explicitly inspect the actual evidence and criteria across the stated coverage. Cache claims use `cold`, `warm`, or `disabled` plus `policy_id`. Application snapshots prove only declared application-file content. A driver-cache claim remains unknown unless explicit vendor/measured evidence, `observation_scope: "vendor_measured"`, and `measurement_method` support the reviewed state; application file hashes cannot verify global driver state.

Concrete driver-cache evidence must include an `actual` artifact with `media: "measurement"`, `evidence_type: "vendor_cache_state"`, and `tool: {name,version}`. Relabeling an application snapshot or supplying a tool flag does not satisfy this gate. The measurement and its interpretation still remain a reviewed claim; missing vendor evidence legitimately leaves the state unknown.

Attach a local reviewed receipt without hand-editing its manifest:

```powershell
python tools/capture_experiment.py attach-conditions --run-dir D:/captures/run-01 --receipt D:/reviews/run-01.json --evidence D:/reviews/copies.json
```

The optional copies file is a JSON list such as `[{"source":"D:/reviews/sequence.bin","path":"external/conditions/sequence.bin"}]`. Receipt references must use the destination paths and exact source hashes/sizes. Existing selected evidence may be referenced without copying. Copies are ordinary exclusive files; dump/fakelib paths and reparse points are refused. Correctness rejects hardlink aliases and copying one original file into both actual/reference roles. Attachment shares `.capture-attach.lock`, requires a finalized run, preserves the exact previous manifest as a revision artifact, and refuses to replace an existing selected receipt silently. An interrupted attachment may leave unselected files; it must not be represented as a completed attachment.

## Experiment schedules and hypotheses

`capture_experiment.py` plans schedules and freshly assesses existing runs. It never starts the game or an external collector, changes caches, or treats a schedule as execution approval. `aa` requires identical configurations for same-mode variation. `overhead` requires the same build/runtime and only explicitly declared diagnostic differences. `ab` requires explicit treatment JSON pointers; machine, title, scene, and capture window cannot be treatment variables. Default schedules have five independent pairs; fewer pairs remain diagnostic and insufficient. Alternating order uses AB, BA, AB, BA; randomized order uses a recorded seed and deterministic SHA-256 block ordering, with one A and one B in every pair.

For a same-mode A/A plan, save this specification and use the same existing capture configuration twice:

```json
{"id":"aa-control","mode":"aa","pairs":5,"ordering":"randomized","seed":"local-01","metric":{"name":"mean_frame_ms","unit":"ms","direction":"lower"}}
```

```powershell
python tools/capture_experiment.py plan --spec D:/plans/aa-spec.json --config-a D:/plans/measurement.json --config-b D:/plans/measurement.json --output-dir D:/plans/aa-control
```

The output directory must be new, its parent must exist, and it must be outside the source checkout. It contains a sealed `plan.json` and per-slot configuration snapshots with experiment/pair IDs. For overhead, set `mode` to `overhead` and declare, for example, `"treatment_paths":["/settings/diagnostics"]` while supplying measurement and attribution configurations. Explicit supported diagnostic environment differences must also be declared. For A/B, use `mode: "ab"` with the intended treatment pointers, for example `/build/options/feature`. External tool packets can record explicit start/stop/export argv, live/replay kind, and a bounded `maximum_seconds`; their lifecycle remains planned until separately observed evidence exists.

Observer overhead can be an explicit treatment through `/observation/enabled`, `/observation/interval_ms`, and other bounded collection-policy fields. Both configurations should include the observation object so the difference is specific. Hash roots, cache snapshot locations, and effective-settings file selection affect collection cost; post-run build/symbol/profile attachment paths are not an observer-overhead treatment. Assessment binds the recorded collection policy, so a different sampling interval cannot pass under the planned label.

After separately approved execution, preserve every attempt in a JSON list. A selected entry has `slot_id`, `selection: "selected"`, and absolute `run_dir`. A rejected entry has the same slot ID, `selection: "rejected"`, a reason, and `evidence: {path,sha256,size}` pointing to the local failure/preflight artifact. Replacements do not erase failed attempts; rejection history keeps selection-bias limitations visible.

```powershell
python tools/capture_experiment.py assess --plan D:/plans/aa-control/plan.json --attempts D:/plans/aa-attempts.json --output D:/plans/aa-assessment.json
```

Assessment recomputes health from raw run evidence, verifies plan/configuration and artifact bindings, rejects reused identities/traces and overlapping process intervals, and checks stable source/runtime identity within each variant. AA/overhead also require the same source/runtime identity across A and B. Five complete pairs, known reviewed conditions, matching cache policy/state, verified provenance gates, and equal independently available workload counts are required for `eligible`. Measurement runs can obtain those counts from the lightweight ledger; absent or incomplete ledger/detail evidence still leaves workload equivalence unproven. Forensic recording is not forced onto every measurement. Units must be verified for non-frame metrics. The report separates same-mode variation, instrumentation overhead, and treatment observations. It reports paired descriptive deltas, median/range, and dispersion; `eligible` is not statistical significance or a performance-win verdict. CLI exits are 0 for eligible, 1 for insufficient, and 2 for malformed/contradictory evidence.

The workload gate requires complete requested-draw, dispatch, emitted host draw/dispatch-command, and indirect-record counts, with available operation outcomes. Diagnostic scope and query totals are excluded from equivalence. Matching these selected counts does not prove every uninstrumented operation is identical; correctness and observed scene reviews remain separate gates. Log issue candidates retain normalized signatures and exact artifact/line references, suppress encoded payloads, and report truncation or capacity limits. An empty candidate list does not verify correctness or mean the game was error-free.

The Python hypothesis-ledger API stores hash-linked, append-only observation, hypothesis, prediction, experiment, decision, and rejection events. Observations reference exact artifact hashes and source events or lines. Predictions name a metric, unit, direction, and threshold before the linked experiment. Experiment events bind the sealed plan hash. Directly supplied supported/contradicted decisions are refused; public `conclude_hypothesis(..., report_path=...)` always freshly assesses the linked plan/attempts before deriving that descriptive outcome, otherwise it records inconclusive. The report path must be new; the saved report hash and exact artifact reference are added to the decision. Decisions explicitly retain `evidence_scope: "descriptive_matched_observations"`, `causal_status: "unknown"`, and `statistical_significance: "not_established"`. A matched median threshold is not a proven fix/root cause or a performance gain, especially without independent A/A noise and collector-overhead controls. The chain detects accidental history edits; it is not a cryptographic signature proving who reviewed evidence. None of these workflows removes the need for actual correctness review, bounded external measurements, or later approved validation.
