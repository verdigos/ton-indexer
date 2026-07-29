// S7 — the tier-2 SUSPENSION-LOOP gate (em2-tier2-design §10 W4).
// HARNESS-ONLY and hermetic: no store, no redis, no artifact, no fixture
// corpus, no network. It drives the real EmuClassifierActor on a real
// td::actor::Scheduler with a FAKE tier-2 fetch that can answer, error, delay
// or never answer at all.
//
// Why the fixpoint is scripted rather than real: the actor's own converter
// needs transaction CELLS, and the fixture corpus carries parsed transactions
// without them — so a corpus-driven version of this gate could not be a ctest
// (the fixture gates are deliberately outside ctest). Scripting the fixpoint
// through the driver's already-public builder seams isolates what this gate is
// actually about: the actor's suspend/resume/commit machinery, not the engine.
#pragma once

namespace mch {

int run_tier2_suspend_test();

}  // namespace mch
