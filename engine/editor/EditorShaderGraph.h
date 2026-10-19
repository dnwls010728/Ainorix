#pragma once
#include <string>
#include <vector>
#include "core/Json.h"

namespace oe {

// Graph nodes keep stable indices while editing; exporting orders dependencies before consumers.
bool OrderEditorShaderGraph(const Json& document, Json& ordered, std::string* error);
// Connect arbitrary nodes, rejecting cycles even while other pins are disconnected.
bool ConnectEditorShaderGraph(Json& document, int source, int target, int pin, std::string* error);
// Remove a node and remap references; dependent pins are left explicitly disconnected.
void RemoveEditorShaderNode(Json& document, int index);
// Argument counts of the portable shader operations; -1 for unknown operations.
int EditorShaderArity(const std::string& operation);
// All portable operations, in palette order.
const std::vector<std::string>& EditorShaderOperations();
// Whether an operation exists in a surface graph (material) or a post graph (`"stage": "post"`).
bool EditorShaderOperationInStage(const std::string& operation, bool post);
// A post graph document (`"stage": "post"`): color output only.
bool EditorShaderIsPost(const Json& document);
// Most nodes a graph holds (ShaderGraph::kMaxNodes, either stage).
int EditorShaderMaxNodes();

}  // namespace oe
