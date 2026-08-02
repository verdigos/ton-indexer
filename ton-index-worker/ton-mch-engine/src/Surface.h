// `--surface`: dump the C++ host/parse registries as stable-sorted JSON.
//
// The Python A/B twins gate matcher runnability against the SAME sets the C++
// engine uses (message parsers, host predicates/fns/shapers, lookup kinds, BOC
// field names). Those sets were hand-mirrored in each twin (CPP_PARSERS etc.);
// this mode emits them from the live registries so the twins load one source of
// truth and can never silently drift. Any diff between this output and a twin's
// expected literals is registry drift, not a formatting change.
#pragma once

namespace mch {

int run_surface();

}  // namespace mch
