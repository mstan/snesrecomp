# LLE-First Whole-Program Analysis

## Contract

The ROM and live 65816 architectural state are the correctness model.
Interpreter/LLE execution must remain available for every reachable address.
AOT bodies and HLE handlers are optional materializations selected only after
analysis stabilizes; neither is allowed to define game semantics.

## Program Model

The analysis graph is keyed by `(pc24, entry_m, entry_x)`. Each node records a
compact summary rather than generated C:

- decoded control-flow extent and structural validity;
- direct call/tail-call/dispatch demand edges;
- possible exit M/X states;
- stack/return-continuation summary;
- AOT eligibility and the reason for any LLE-only classification;
- optional HLE overlay identity, independent of the function boundary.

Node states are:

1. `LLE_ONLY`: reachable and semantically valid, but not safely or profitably
   materialized as AOT. Runtime dispatch executes the ROM interpreter.
2. `AOT_ELIGIBLE`: analysis has a sound entry state, extent, return contract,
   and all required code-generation facts.
3. `AOT_EMITTED`: an eligible node selected by the materialization policy.
4. `HLE_OVERLAY`: optional replacement for an LLE/AOT node. Disabling the
   overlay must expose the correct underlying implementation.

An unresolved indirect site is not automatically a poisoned function. It
makes that edge/node require LLE handling while earlier direct calls remain
valid demand edges. A wrong-width decode that reaches BRK/COP, invalid ROM,
impossible control flow, or another structural contradiction is poisoned and
must not contribute outgoing demand.

## Fixed Point

1. Seed roots from vectors, explicit host exports, and preserved-bank imports.
2. Decode demanded nodes into compact summaries with a work queue.
3. Propagate entry M/X and exit M/X facts along graph edges.
4. Solve recursive call components as SCCs; requeue only dependents whose
   input facts changed.
5. Classify unsupported or ambiguous nodes as `LLE_ONLY` rather than guessing
   a sibling width or emitting a placeholder.
6. Stop when node facts and demand edges are stable.
7. Select AOT nodes and emit every affected bank once.

Generated C is never an analysis input. Stub-marker scanning remains a final
assertion during migration, not the mechanism that discovers dead variants.

`func ... end:` is a boundary and ownership declaration, not by itself a
reachability root. RESET and native NMI/IRQ vectors are architectural roots;
explicit host exports and preserved-bank imports will be represented as
separate root kinds. Native interrupts preserve live M/X, so analysis admits
all four interrupt-entry states and lets exact unavailable variants use LLE.

## Variant Policy

- Emit only demanded, AOT-eligible M/X variants.
- A runtime M/X combination without an exact AOT body dispatches to LLE at the
  original ROM address. It must not call the "nearest" generated sibling.
- A function may have one, several, or zero AOT variants without affecting its
  semantic reachability.
- Speculative decoding uses bounded analysis. Hitting the budget classifies
  the node `LLE_ONLY`; it does not expand the graph indefinitely.

## Incremental and Atomic Regeneration

- Cache decoded summaries by ROM, cfg, compiler-content, and input-fact hash.
- Store explicit per-bank import/export manifests.
- A partial run treats preserved-bank imports as immutable roots.
- Analyze to convergence before emission.
- Stage generated output beside the live directory and publish a complete
  generation with a recoverable directory swap.
- Interrupted or failed analysis leaves the previous generated program intact.

## Hint Reduction

Hints may state ROM structure that bytes alone cannot prove, such as genuine
data ranges or externally selected entry roots. They must not be required for
facts the decoder/runtime can observe:

- HLE annotations do not replace `func ... end:` boundaries.
- M/X exits, balanced interpreter continuations, stack-return shapes, and
  common indirect-dispatch idioms should be inferred.
- When an indirect target set cannot be proven statically, retain live LLE
  dispatch rather than requiring an exhaustive per-game table.
- Existing hints are migration tests: remove one only after the generic path
  produces identical behavior with the hint absent.

## Measurements From the Initial MMX Probe

The legacy emission-feedback loop reached emission pass 3 at 503.5 seconds,
after pruning 406 wrong-width variants and promoting thousands of additional
entries. A naive attempt to move the same unbounded closure before emission
retained roughly 735 MB of decode graphs and still performed equivalent work.

Disabling memoization for one-shot worklist nodes reduced the same phase to
roughly 94 MB initially. Granular validity gating showed why a binary
clean/dirty function flag is insufficient: unresolved indirect dispatchers
still contained approximately 970 legitimate direct-call demands. The final
model therefore needs per-edge LLE fallback and compact node summaries, not
eager promotion plus whole-function rejection.

The first compact analysis-only implementation (`tools/v2_analyze.py`) then
converged on Mega Man X in 14.9 seconds from 20 configured roots. It produced
4,236 exact variants at 4,115 unique PCs and 31,374 demand edges. Of those
PCs, 4,032 demanded one M/X form, 53 demanded two, 22 demanded three, and only
8 demanded all four. A second run produced a byte-identical 9.3 MB manifest
(SHA-256 `3A967D0A7B3C66DB5B90DA4B8092E894238E8CD2CE53DE8894B649BE169F8EFB`).
This is analysis-only evidence, not yet an emission or gameplay claim.

After separating cfg boundaries from architectural roots, the same bounded
analysis completed in under one second per title: Mega Man X reached 29 exact
variants, SMW 37, Zelda 43, and Super Metroid 85 from nine conservative roots
(RESET M1X1 plus all native NMI/IRQ M/X states). These small sets are the
statically proven AOT frontiers, not claims that the rest of each ROM is dead:
execution beyond an unprovable dynamic edge remains reachable through LLE.
The previous all-cfg-roots counts remain available through
`v2_analyze.py --all-cfg-roots` as a migration diagnostic.

## Acceptance

- Deterministic full and partial generated manifests.
- No generated-C feedback passes to discover reachability or dead M/X nodes.
- No missing combination silently no-ops or runs a nearest-width sibling.
- HLE-disabled execution remains correct.
- SMW, Zelda, Super Metroid, and Mega Man X pass clean attract-demo runs.
- Final acceptance is interactive user playtesting of all four games.
