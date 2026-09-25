// Bridge for the emulator's ::Trace type. Its separate target keeps mch-classification-runtime
// linkable without emulator_static.
#pragma once

#include "TieredLookupSource.h"

struct Trace;  // Emulator ::Trace; mch::Trace also exists.

namespace mch {

// Adapts detector results without retaining transaction or block cells.
TieredLookupSource::InterfaceMap make_interface_map(const ::Trace &trace);

}  // namespace mch
