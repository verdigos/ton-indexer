#pragma once
#include "TraceEmulator.h"
#include "emu/EmuTypes.h"

td::Result<mch::EmuTraceView> build_emu_trace_view(const Trace& trace);
