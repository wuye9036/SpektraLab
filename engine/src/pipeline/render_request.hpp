#pragma once
#include "spektrafilm/spk_engine.h"
#include <functional>

namespace spk {
// Drop recomputable tier inputs and negatives while the published frame and source remain usable.
void discard_tier_cache(spk_session*);

// A worker-owned render request can retire between pipeline nodes. Existing
// C callers render without a predicate.
spk_status render_interruptible(spk_session*, const char* tier, bool reprint, spk_result*,
                                std::function<bool()> interrupted);
}
