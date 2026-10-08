#pragma once
#include "core/Json.h"

namespace oe {
// Transient authoring graphs only. Both conversions use the runtime expression compiler.
// Graph output is recompiled and bounded; no graph is serialized into a VFX asset.
Json VfxExpressionGraphCall(const Json& args, bool toSource);
// Operation palette and graph format/limits for headless clients and the editor.
Json VfxExpressionGraphSchema();
}  // namespace oe
