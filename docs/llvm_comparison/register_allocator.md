# Register Allocation: LLVM vs bcc

Covers LLVM's register allocation stack in dependency order: liveness
(LiveVariables, SlotIndexes, LiveIntervals), coalescing, the three
allocators (fast, basic, greedy — the last being the production
algorithm), spilling and splitting, rematerialization, stack-slot
coloring, and the callee-saved-register / PEI interplay. Then the bcc
picture (none — `llc` allocates), and a staged plan: local allocation for
the first native backend milestone, linear-scan global allocation, and a
greedy-lite end state modeled on RAGreedy.

Pipeline positions verified from `TargetPassConfig.cpp::addOptimizedRegAlloc`
(Nov 2026, `main`).

---

## 1. LLVM's implementation

### 1.1 The pipeline around allocation

```
SSA MIR
  → PHIElimination            (llvm/lib/CodeGen/PHIElimination.cpp)
  → TwoAddressInstructionPass (llvm/lib/CodeGen/TwoAddressInstructionPass.cpp)
  → RegisterCoalescer         (llvm/lib/CodeGen/RegisterCoalescer.cpp)
  → [pre-RA scheduler]        (MachineScheduler)
  → RAGreedy / RABasic / RAFast
  → VirtRegRewriter           (llvm/lib/CodeGen/VirtRegRewriter.cpp)
  → StackSlotColoring         (llvm/lib/CodeGen/StackSlotColoring.cpp)
  → MachineCopyPropagation
  → PEI (prologue/epilogue)   (llvm/lib/CodeGen/PrologEpilogInserter.cpp)
```

Key model: **all ISel output uses virtual registers** (`MachineRegisterInfo`
tracks vreg def-use, classes, hints). Allocation maps vregs to physregs
or stack slots. PHI nodes are eliminated *before* RA into parallel
`COPY` pairs, and coalescing removes most of those copies — SSA-ness
ends here, by design.

### 1.2 Liveness

- **SlotIndexes** (`llvm/lib/CodeGen/SlotIndexes.cpp`): each instruction
  gets a slot number (×2, so edits can insert between); the index list
  maps slots ↔ `MachineInstr`s and survives mid-block insertion.
- **LiveIntervals** (`llvm/lib/CodeGen/LiveIntervals.cpp`): per-vreg
  live ranges as sorted, coalesced lists of `[start,end)` segments in
  slot space. Computed by a single backward pass (the classic
  LiveIntervalAnalysis; sub-register liveness via `LaneMasks` when
  enabled). Segments carry VNInfo (value number per definition) — the
  data structure everything below consumes.
- **LiveVariables** (`llvm/lib/CodeGen/LiveVariables.cpp`): legacy
  per-instruction partially-live info, still used by RAFast/early
  passes.
- **LiveStacks / LiveRegMatrix**: intervals for stack slots; the matrix
  (`LiveRegMatrix.cpp`) is the interference *query* structure — an
  interval-units × physreg bitmap answering "can this vreg take this
  physreg?" in O(segments).
- **RegisterPressureTracker** (`RegisterPressure.cpp`): incremental
  pressure per class within a block, used by the scheduler and RAGreedy's
  splitting decisions.

### 1.3 Coalescing (`RegisterCoalescer.cpp`)

Joins live intervals of COPY-related vregs when their register classes
are compatible and the join doesn't create interference (checked against
the matrix). Handles sub-register identities (`AL = EAX = RAX` lanes),
partial copies, and inserts `IMPLICIT_DEF` repair when joining changes
value availability. Runs before allocation so the allocator sees fewer,
larger intervals. This is also where most phi-elimination copies die.

### 1.4 The allocators

**RAFast** (`llvm/lib/CodeGen/RegAllocFast.cpp`, the −O0 allocator):
block-local allocation: walk each block, assign free physregs to live
vregs, spill-on-demand at block boundaries (values live-in reloaded).
No interference graphs, no global intervals. Correctness-first, speed of
compilation second.

**RABasic** (`llvm/lib/CodeGen/RegAllocBasic.cpp`): global LiveIntervals
+ greedy assign-or-spill *without* eviction/splitting (uses
`SpillPlacement` hints only). Largely a stepping stone today.

**RAGreedy** (`llvm/lib/CodeGen/RegAllocGreedy.cpp`, ~3k lines — the
production allocator, verified file). One thread per function:

1. **Priority order**: vregs sorted by **spill weight**
   (`CalcSpillWeights.cpp`: use/def frequency × block frequency from
   `MachineBlockFrequencyInfo`, normalized per register class; huge
   weights pin rematerializable/always-live values).
2. **`RAGreedy::selectOrSplit(LiveInterval&, SmallVirtRegSet&)** — the
   heart:
   - try physreg assignment: for each allowed reg (order shaped by
     allocation hints — copy-related regs, `llvm.lifetime`, ABI regs),
     check the LiveRegMatrix for interference;
   - if a candidate is blocked, consider **eviction**: compute
     `EvictionCost` (spills+weights) of all intervals blocking it; evict
     the cheaper set *if* their total weight is below the new interval's
     (plus loop-depth protection). Evicted intervals return to the
     queue. (The ML advisor `MLRegAllocEvictAdvisor.cpp` — verified in
     tree — is an ML replacement for this policy.)
   - if no assignment, **split** (`SplitKit.cpp`, `SplitEditor`): cut
     the live range into pieces at block boundaries chosen by
     `SpillPlacement.cpp` (an min-cut/flow problem over the CFG to place
     splits where reload traffic is cheapest), each piece re-queued;
   - else **spill** (`InlineSpiller.cpp` via `LiveRangeEdit.cpp`):
     choose spill slots, insert stores after defs and loads before uses;
     prefer **rematerialization** when the def is a cheap constant
     (recompute at use instead of reload).

3. **Rewrite** (`VirtRegRewriter.cpp`): rewrite vreg operands to the
   assigned physregs; delete now-dead COPYs; convert to a
   `MachineFunctionPass`-visible form.

**PBQP** (`RegAllocPBQP.cpp`): alternative exact-ish solver (retargetable
bipartite quadratic assignment), used on a few targets — evidence the
framework pluggably supports different core algorithms.

### 1.5 Callee-saved registers, PEI, and stack coloring

- Callee-saved policy: which CSRs to save is decided by PEI
  (`PrologEpilogInserter.cpp`, class `PEI`) from
  `TargetRegisterInfo::getCalleeSavedRegs` minus those already used
  (computed after RA — "shrink wrapping" `ShrinkWrap.cpp` moves
  save/restore regions to the smallest CFG regions that contain all
  uses, when profitable).
- **StackSlotColoring** (`StackSlotColoring.cpp`): non-overlapping spill
  slots share stack bytes (graph coloring over slot live ranges).
- PEI also assigns frame offsets, replaces `FRAME_IDX` operands
  (`replaceFrameIndices`), computes the stack size, and inserts
  prologue/epilogue code (`emitPrologue/emitEpilogue` — target's
  `TargetFrameLowering`, e.g. `X86FrameLowering.cpp` with its
  push-sequence and frame-pointer logic).
- After RA: `MachineCopyPropagation`, `MachineLICM`-lite cleanup pass
  (verified order in `addOptimizedRegAlloc`).

---

## 2. bcc's implementation

None — `llc` performs allocation; bcc never sees registers except as
text in `.s`. Two relevant facts from bcc's tree:

- The IR/AST layers are aggressively SSA — all values are virtual; the
  same "everything is a vreg" discipline LLVM assumes pre-RA is already
  bcc's frontend discipline.
- `as/register.hh` knows the x86-64 register file structurally
  (`RegClass`, REX rules, 0..15 numbering) but has no allocator — it
  parses whatever registers the text names.

For the native backend (`machine_codegen.md`), RA is the highest-value
*new algorithm* bcc must own: without it, a naive backend is stuck with
everything-in-memory or single-register strategies (10–50× slowdown on
register-starved kernels).

---

## 3. Comparison

| Aspect | LLVM | bcc plan |
|---|---|---|
| Pre-RA form | SSA MIR, unlimited vregs | same (bcc IR is already SSA; MI layer keeps vregs) |
| PHI handling | PHI-elim → COPY → coalescer | same shape, simplified (no subreg lanes) |
| Liveness repr | SlotIndexes + LiveIntervals + VNInfo + LiveRegMatrix | slot-numbered intervals (map, not bitmap matrix); interference via segment-overlap scan |
| −O0 allocator | RAFast (local) | stage RA-1: block-local, load/store at boundaries — same design |
| −O2 allocator | RAGreedy: weight-ordered assign / evict / split / spill / remat | stage RA-2: linear scan (Poletto-style) → stage RA-3: greedy-lite (assign/evict/spill, no splitting) |
| Spill placement | SpillPlacement min-cut | fixed: store-after-def, reload-before-use (InlineSpiller's simple mode) |
| Remat | yes (constants, cheap defs) | stage RA-3: constants only |
| CSR handling | PEI computes used CSRs, shrink wrapping | PEI-lite: save all CSRs used per function, no shrink wrap |
| Slot coloring | StackSlotColoring | out of scope (debuggable trade) |

---

## 4. What bcc should adopt

1. **The vreg-first MI discipline** (LLVM's central decision): ISel emits
   only virtual registers; *nothing* knows physregs until RA. This keeps
   ISel simple, makes RA the single owner of a hard problem, and matches
   what bcc's IR already does. (`machine_codegen.md` formalizes the MI
   form.)
2. **PHI-elimination before RA**: phis become parallel copies; a later
   copy-coalescing-lite removes most. This is far simpler than phi-aware
   allocation and is battle-proven by LLVM.
3. **LiveIntervals as the liveness currency**: slot-numbered instruction
   indices + per-vreg segment lists. RA-2 and RA-3 consume the same
   structure — exactly LLVM's layering (LiveIntervals serves RABasic and
   RAGreedy identically; RAFast deliberately skips it).
4. **RAFast's design as bcc's RA-1**: the −O0 allocator is the correct
   first implementation — small, obviously correct, and the reference
   oracle for later stages.
5. **RAGreedy's assign/evict/spill loop as the end-state shape**, minus
   splitting and SpillPlacement: eviction alone captures most of greedy's
   win on scalar code, at a fraction of the complexity.
6. **CalcSpillWeights' metric**: weight = Σ uses×block-frequency, loop
   depth exponentiated — cheap to compute, correct priorities.

Not adopted initially: sub-register/lane liveness (x86-64 GPR8/16/32/64
subreg tracking is where LLVM's RA complexity multiplies — bcc can keep
8/16-bit values in 32/64-bit registers throughout, truncating at
boundaries, and skip lane masks entirely); PBQP; ML advisors; splitting
(Linear-scan + eviction covers the gap until measured otherwise).

---

## 5. Concrete design for bcc

### 5.1 Prerequisites in the MI layer (with `machine_codegen.md`)

```cpp
// A vreg is a dense unsigned id; MachineRegisterInfo tracks:
struct MachineRegisterInfo {
  std::vector<RegClass> class_of_;          // kGPR64 | kGPR32 | kXMM...
  std::vector<std::vector<MachineInstr*>> defs_, uses_;   // per vreg
  // hints: preferred physreg for copy-related vregs (coalescer feed)
};
// Instructions carry FrameIndex operands (spill slots) and
// physical-register operands only after RA.
```

### 5.2 PHI elimination + copy coalescing-lite

Parallel-copy lowering (the classic trick: cycle-breaking with a temp),
then a union-find coalescer over `COPY` pairs whose classes match and
whose merged interval stays non-cyclic (overlap check on segments).
LLVM reference: `PHIElimination.cpp` + `RegisterCoalescer.cpp`'s
`tryCoalesce` simplified to whole-vreg joins (no lanes).

### 5.3 LiveIntervals-lite

```cpp
struct Segment { unsigned start, end; };   // slot indexes; 2 per instr
struct LiveInterval {
  unsigned vreg; std::vector<Segment> segs;  // sorted, coalesced
};
// one backward walk per function (LiveIntervals.cpp's computeVirtRegs):
//   def closes segment at def slot; uses extend; block live-out
//   propagates via CFG union of successor live-ins.
```

Interference query for stage RA-3: sort all segments once; per-physreg
"occupied" interval lists (a simplified LiveRegMatrix).

### 5.4 RA-1: local allocation (fast path, roadmap M4)

Per block, per class: keep a free list of allocatable GPR64s
(`rax,rcx,rdx,rsi,rdi,r8..r11` caller-saved set first); on first def of a
vreg assign a free reg; on exhaustion spill the least-recently-used live
vreg to its frame slot (store after its def, reload before later uses
within the block); at block end, spill live-outs. ABI-clobbered regs
(args) are pre-colored (see calling convention in `machine_codegen.md`).
~300 lines; correctness gate: full differential corpus green.

### 5.5 RA-2: linear scan (global)

Poletto & Sarkar's algorithm over LiveIntervals sorted by start:
active-list of intervals; assign any free reg; on exhaustion spill the
interval with the farthest end (or the incoming one, by weight — take
weights from §5.6). Well-understood, allocation-time linear, the
standard "second" allocator for exactly this reason (used by many JITs;
LLVM skips it because RAGreedy exists, but it is the right bcc size).

### 5.6 RA-3: greedy-lite (assign/evict/spill)

```
order vregs by spill weight descending (loop-depth-weighted use counts)
for each interval I:
  for each physreg r allowed (hints first, then caller-saved, then
                            callee-saved):
    if no interference with r's assigned intervals: assign(I, r)
    else record evictable set S(r) = assigned intervals blocking r
  if some r exists where weight(S(r)) < weight(I): evict S(r) (unassign,
    requeue), assign(I, r)                       // eviction
  else if I.def is a materializable constant: rematerialize at uses
  else spill(I): stores after defs, loads before uses, split not done
```

That loop, minus splitting/SpillPlacement/hint machinery, is ~600 lines
and is structurally RAGreedy's `selectOrSplit` with the split arm
removed. The three stages share LiveIntervals and rewrite code, so RA-3
subsumes RA-2 the way RABasic→RAGreedy share machinery.

### 5.7 Rewriting and PEI-lite

After assignment: rewrite operands (vreg→physreg), delete dead COPYs
(LLVM `VirtRegRewriter.cpp`), then PEI-lite: compute used callee-saved
regs, emit push/pop sequences + `sub rsp, N` frames via
`X86FrameLowering`-analogous code, replace FrameIndex operands with
`[rbp−off]`/`[rsp+off]` addressing (`replaceFrameIndices` analog). Frame
pointer: keep `rbp` always (frame pointers on; simpler debug + stack
walks; `-fomit-frame-pointer` equivalent later).

---

## 6. Testing strategy and success criteria

- **Unit**: per stage, synthetic MI fixtures → expected assignments/
  spill code; interval computation property tests (every def covered,
  every use inside a segment).
- **Differential execution** (the gate): whole corpus, RA-1 vs RA-2 vs
  RA-3 vs `llc -O0`, identical program outputs.
- **Metrics** (per function, driver-flagged): #spills, #spill-reloads,
  #copies remaining after coalesce, #vregs assigned vs spilled. Success:
  RA-2 spills < 5% of vregs on the scalar corpus; RA-3 strictly ≤ RA-2
  spills; dynamic instruction count on kernels within 1.5× of `llc -O0`
  (RA-3).
- **Stress**: functions with 100+ live scalar variables (generated
  combinatorially) to exercise eviction/spill paths deterministically
  (fixed seeds; reproducibility required).

## 7. Challenges and trade-offs

- **No subreg lanes** means 8/16/32-bit values occupy whole 64-bit regs
  (zero-extended); some waste, zero lane complexity. Correct choice until
  measured otherwise; revisit if/when porting tricks like
  `X86FixupBWInsts.cpp`-style narrowing.
- **Eviction without splitting** can ping-pong (A evicts B, B requeues
  and evicts A). Guard: monotonically increasing "eviction depth"
  counter per interval; at depth ≥2 force spill. (LLVM's analog guard:
  `MaxEvictionChain` in RegAllocGreedy.)
- **Two-address x86 instructions** (`add r1, r2` destroys r1) — LLVM
  inserts the constraint via `TwoAddressInstructionPass` + tied operands;
  bcc's MI table marks tied operands and a pre-RA pass inserts `mov`
  repairs. Must not be left to the encoder to discover (silent
  miscompile source).
- **Calls clobber caller-saved regs**: modeled as register-mask operands
  on call instructions (LLVM's `RegMask` operand) — intervals crossing a
  call must not live in caller-saved regs unless reloaded; or prefer
  callee-saved for cross-call liveness (LLVM does both; bcc: prefer
  callee-saved, weight cross-call intervals ×4).
