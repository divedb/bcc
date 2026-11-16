# Instruction Scheduling: LLVM vs bcc

Covers LLVM's machine schedulers: dependence-graph construction, the
pre-RA and post-RA pipelines, scheduling models (latency + processor
resources), hazard handling, and the machinery around them (SW
pipelining, execution-domain fixing, false-dependence breaking). Then the
bcc picture (no scheduler — `llc` owns it), what a minimal bcc scheduler
would look like, and an honest cost/benefit verdict: for bcc, scheduling
is the *last* backend phase worth building, and the document says so
explicitly.

---

## 1. LLVM's implementation

### 1.1 Where schedulers run

Scheduling appears at three points in the backend pipeline
(`llvm/lib/CodeGen/TargetPassConfig.cpp`, verified):

1. **SelectionDAG emission scheduling** (`llvm/lib/CodeGen/SelectionDAG/
   ScheduleDAGSDNodes.cpp` + strategy files `ScheduleDAGRRList.cpp`
   (list scheduling), `ScheduleDAGFast.cpp` (linearize, −O0),
   `ScheduleDAGVLIW.cpp`): orders the selected nodes when emitting MIR
   from the DAG.
2. **Pre-RA machine scheduler** — `MachineScheduler` pass
   (`llvm/lib/CodeGen/MachineScheduler.cpp`, added in
   `addOptimizedRegAlloc()`): the main O1+ scheduler, runs on SSA MIR
   *while register pressure is visible* (its defining feature).
3. **Post-RA scheduler** — `PostMachineScheduler` or legacy
   `PostRASchedulerList` (`PostRASchedulerList.cpp`), added in
   `addMachinePasses()` for O1+: schedules with physical registers and
   real hazards only.

For −O0 the whole stack is skipped (linear emission, `ScheduleDAGFast`).

### 1.2 Dependence graph construction — `ScheduleDAGInstrs`

`llvm/lib/CodeGen/ScheduleDAGInstrs.cpp` builds `SUnit`s (scheduling
units ≈ MachineInstrs, minus bundled/pseudo variants) with edges:

- **Data**: real def→use on virtual/physical registers.
- **Anti / output**: use→def / def→def on the same register (hazards
  once physregs are assigned; pre-RA these matter via register masks).
- **Memory dependencies**: ordered pairs (RAW/WAR/WAW) computed with
  alias analysis over loads/stores (`buildSchedGraph` walks *up* the
  block tracking per-register last def and a load/store list; calls and
  barriers are hard ordering edges, via register masks + memory
  barriers).
- **Cluster edges** for loads/stores LLVM wants adjacent (folded
  pairs), and **chain edges** preserving volatile/ordered semantics.

The graph is per basic block (plus some peephole regions); cross-block
scheduling exists only in the software pipeliner.

### 1.3 The MachineScheduler framework

`MachineScheduler.cpp` structure (stable since LLVM 3.5, still accurate):

- `MachineSchedStrategy` interface:
  `initPolicy / initialize(ScheduleDAGMI) / pickNode(bool &IsTopNode)` —
  strategies answer "which ready instruction next?" from two queues
  (Top: available to issue earliest; Bottom: critical-path downward —
  LLVM's **bidirectional list scheduling**; a top-down pass fills
  dependent chains, bottom-up limits register pressure).
- `GenericScheduler` (`llvm/lib/CodeGen/MachineScheduler.cpp`,
  x86-default): the heuristic core — per-SU resource counts
  (`ResourceSegments`), latency priority ("critical path first"),
  **register-pressure heuristic**: pressure ceiling per register class;
  when actual pressure (tracked by `RegisterPressureTracker`) exceeds
  the ceiling's limit, preference flips to the bottom queue (schedule to
  *reduce* live ranges). This is the whole point of pre-RA scheduling:
  interleave for ILP *without* exploding spill counts.
- `PostGenericScheduler`: same skeleton, physical-register and hazard
  aware (`HazardRecognizer` hooks).
- Region-based: functions are scheduled region-by-region (regions
  bounded by scheduling barriers — calls, unanalyzable memory), enabling
  incremental runs from MIR tests (`-run-pass=machine-scheduler`).

### 1.4 Scheduling models — latencies and resources

`llvm/include/llvm/MC/MCSchedule.h` + generated tables:

- `MCSchedModel` per CPU (from `Target/X86/X86SchedSkylakeClient.td`-era
  files; the set is large — one model per microarchitecture, verified
  present under `lib/Target/X86`).
- Per instruction: `SchedClass → {latency, {ProcResourceKinds × count}}`
  (e.g. Skylake `imul r64,r64` = latency 3, 1× port 1). Legacy
  *itineraries* (`InstrItineraryData`, `MCInstrItineraries`) survive on
  older in-order targets; modern models are resource-based
  (`MCSchedClassDesc::BeginGroup/EndGroup`, `MicroOpBufferSize`).
- `TargetSubtargetInfo::getSchedModel()` hands the model to the
  scheduler; TableGen computes default latencies (1) where the `.td`
  doesn't specify.
- `llvm-mca` (`llvm/tools/llvm-mca`) consumes the same models to
  *simulate* a target — the standalone proof that the models are data,
  not compiler-internal.

### 1.5 Hazard recognition and adjacent passes

- `ScoreboardHazardRecognizer` (stalls from itineraries) and
  `CriticalAntiDepBreaker` (post-RA renaming to break false deps) — the
  legacy pair; modern targets rely on the resource model instead.
- `X86ExecutionDomainFix.cpp` (verified in target dir): pre-RA pass
  making SSE↔AVX domain choices consistent to avoid bypass penalties —
  an example of *target pass collaboration with the scheduler*.
- `BreakFalseDeps` (inside MachineScheduler.cpp): inserts zeroing
  (`xor`) for partial-register definitions at schedule time.
- **MachinePipeliner** (`llvm/lib/CodeGen/MachinePipeliner.cpp`, opt-in,
  PPC/Mips primarily): modulo scheduling for loops — cross-block
  scheduling, II computation, kernel prologue/epilogue generation.
- Macro-op fusion (x86 `cmp+jcc` → fused `ujcc`): `MacroFusion`
  (MachineScheduler.cpp) constrains the scheduler to keep pairs adjacent
  — model-visible, not pattern-visible.

### 1.6 What the scheduler does *not* do

Register allocation is downstream pre-RA (they communicate through
pressure only — see `register_allocator.md` §1); the scheduler never
renames vregs (except anti-dep breaking post-RA); instruction *choice*
is ISel's job. Scheduling = ordering.

---

## 2. bcc's implementation

None. The current pipeline hands `.ll` to `llc`, which schedules;
`bcc-as` re-parses finished text. bcc's `as/` layer even preserves
instruction order strictly (parser → encoder is 1:1 in program order;
`Section` appends bytes linearly).

For the future native backend (`machine_codegen.md`), the default
proposal is **in-order emission** — which is exactly LLVM's `-O0`
behavior (`ScheduleDAGFast.cpp` linearizes, `addMachinePasses` adds no
scheduler at OptLevel 0). This document specifies what a *later* Phase-M
scheduler adds.

---

## 3. Comparison

| Aspect | LLVM | bcc (planned native backend) |
|---|---|---|
| Emission order at −O0 | linear (ScheduleDAGFast) | linear — same class of behavior |
| Pre-RA scheduling | bidirectional list scheduling w/ pressure awareness (GenericScheduler) | deferred; optionally a top-down list scheduler late in the roadmap |
| Dependence model | SUnit DAG with data/anti/output/memory edges | would need at minimum data+memory edges within a block |
| Machine model | MCSchedModel per µarch, resource-based, shared with llvm-mca | one toy model: uniform latency 1 + simple resource counts (ALU/AGU/MUL/DIV) is sufficient to make heuristics meaningful |
| Hazard handling | resource model / scoreboard | x86-64 OoO cores make precise hazards unnecessary; document "no hazard tracking" as a scope decision |
| Pipelining | MachinePipeliner (modulo) | out of scope permanently (OoO targets dominate) |

---

## 4. What bcc should adopt

**Verdict up front: scheduling is the lowest-priority backend component
for bcc.** x86-64 out-of-order cores hide most ordering damage; the
optimizer and RA deliver the measurable wins; LLVM itself skips the
schedulers at −O0. Adopt these *ideas* when the time comes:

1. **The SUnit/dependence-graph construction as a reusable analysis** —
   even with in-order emission, a block-level dependence graph enables
   *dead-instruction elimination, bundling checks, and future scheduling*
   for free (`ScheduleDAGInstrs::buildSchedGraph` is the reference).
2. **Strategy-pattern separation** (`MachineSchedStrategy::pickNode`):
   the scheduler engine is generic; the policy is one class. If bcc ever
   schedules, this is the shape — one engine, per-target or per-tuning
   policy objects.
3. **Bidirectional queues + pressure ceiling** (GenericScheduler's core
   idea) *if* a pre-RA scheduler is built — but only after RA exists to
   benefit (otherwise "pressure" has no consequence).
4. **A data-driven machine model from day one of MI emission** — not for
   scheduling, but because latency/latency-in-encoder-budget columns in
   the instruction table cost nothing now and enable llvm-mca-style
   analysis later.

Explicitly not adopted: itineraries, VLIW paths, SoftwarePipeliner,
MacroFusion machinery (x86 fused pairs can be handled as a fixed
`cmp+ jcc` adjacency peephole in the peephole pass, good enough).

---

## 5. Concrete design (deferred phase "Sched-M")

Only entered after RA (roadmap M6). Scope: per-block, pre-existing
selected MIR, physical regs known (i.e., a *post-RA* scheduler — simpler
than LLVM's pre-RA flagship).

```
SchedUnit { MachineInstr*; height; ready_cycle }
buildDepGraph(block):
  for each MI bottom-up:
    data edges from physreg def-use (post-RA this is exact)
    memory edges: last store / barrier tracking (conservative:
      unknown-address load/store pairs ordered)
schedule(block):                          // top-down list schedule
  ready = all roots
  while ready:
    pick = argmax(ready, key = (height, fewer regs read, program order))
    emit(pick); update successors' height/cycle
  // deterministic tie-break: program order — reproducibility is a
  // testing requirement, not an afterthought
```

~400 lines total. Expected effect: small (OoO cores); the honest success
metric is *instruction-boundary* behaviors — grouping compare+jump,
spreading long-latency ops — measured on microbenchmarks, not spec
claims.

Machine model table (`machine_codegen.md` §5's instruction table gains
two columns): `latency` (1 for ALU/mov, 3 for imul/mulss..., 20+ for
div), `ports` (bitmask of 4 abstract ports). Data shape mirrors
`MCSchedClassDesc` semantics minus resources precision.

---

## 6. Testing strategy and success criteria

- Determinism first: same input → identical instruction order (golden
  MIR fixtures). LLVM enforces this too (scheduler must be
  reproducible); bcc gets it from program-order tie-breaking.
- Semantics: full differential-execution corpus must stay green with the
  scheduler enabled (`-mbcc-sched=on|off` driver flag; default off until
  proven).
- Metric: cycles on a toy in-order model (llvm-mca-inspired simulator,
  ~200 lines, uses the latency/port columns) on the scalar kernel set.
  Success = ≥5% modeled-cycles improvement on ≥3 kernels, zero
  regressions >1% elsewhere, zero correctness failures. If the modeled
  win can't be demonstrated, the phase ships disabled — documented
  negative results are acceptable output of this phase.

## 7. Challenges and trade-offs

- **Post-RA scheduling constraints**: physical-register anti-dependences
  make aggressive reordering *wrong* without renaming
  (CriticalAntiDepBreaker exists for this in LLVM). bcc's simple
  scheduler should treat all same-register pairs as ordered (no
  renaming) — conservative and correct.
- **Memory ordering**: without alias analysis beyond "alloca vs global
  vs unknown", memory edges are coarse; LLVM pays for AA precision here.
  bcc accepts conservative edges (missed ILP) as the price of scope.
- **Interaction with RA**: LLVM's pre-RA scheduler is famous for
  pressure-driven decisions; skipping it entirely (bcc's plan) trades
  ILP for RA predictability — the correct trade at this scale.
