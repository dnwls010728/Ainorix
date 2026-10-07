#pragma once
#include <string>
#include <vector>
#include "core/Json.h"

namespace oe {
// A derived view of existing asset references. It never owns a second effect document.
struct EditorVfxNode {
    std::string id, kind, name;
    int block = -1;
    std::vector<std::string> inputs;
};
struct EditorVfxEdge { std::string source, target, field; };
struct EditorVfxGraph { std::vector<EditorVfxNode> nodes; std::vector<EditorVfxEdge> edges; };
// Additional material/anchor references are editor palette entries, not asset data.
EditorVfxGraph BuildEditorVfxGraph(const Json& document, const std::vector<std::string>& materials = {},
    const std::vector<std::string>& anchors = {}, const std::string& field = "rate");
// Returns a free name shared by the field and node add actions.
std::string UniqueEditorVfxName(const Json& document, const std::string& prefix);
// Produces a candidate document only. The caller must submit it to vfx.edit_replace for validation/history.
bool PatchEditorVfxWire(const Json& document, const EditorVfxNode* source, const EditorVfxNode& target,
    const std::string& field, Json& candidate, std::string& hint);
}  // namespace oe
