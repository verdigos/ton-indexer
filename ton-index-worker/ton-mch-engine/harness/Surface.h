// `--surface`: dump the C++ host/parse registries as stable-sorted JSON.
// Matcher runnability is gated on these live registry names; this mode
// emits them so consumers cannot silently drift from the linked surface.
#pragma once

namespace mch {

int run_surface();

}  // namespace mch
