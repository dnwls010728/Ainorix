#pragma once
#include "core/Json.h"

namespace oe {

class Engine;

// Pictures a 2D skeleton is missing, asked of an image generator and brought back safely (docs/SKELETON2D.md,
// "Repainting parts"). The engine never calls a generator itself: it writes requests an agent hands to one (with
// the user's consent, since the pictures leave the machine) and checks what comes back.

// kind "fill" (default): for each part (or `slots`), the area of its picture that lay under a front part in the setup
// pose - where it shows cut edges or a copy of the front part when the skeleton moves - within `reach` pixels of the
// part, optionally on a canvas grown by `grow` pixels. kind "attachment": a new picture for `slot` named `name`
// (`description` says what changes: a fist, closed eyes). Each request is a folder with source.png, mask.png
// (transparent = may change), edit.png (the source with that area magenta), context.png (the whole figure),
// prompt.txt and request.json.
Json Skeleton2DVariantRequests(Engine& engine, const Json& args);

// Takes a generated picture for a request: matches it to the source (size and aspect, a shift of up to `maxShift`
// pixels, a tint), refuses it when the area that had to stay changed more than `tolerance`, keeps only the masked area
// (keying out an opaque background), writes it as a new image beside the part's picture and points the skeleton at it
// - a fill replaces the attachment's picture, an attachment variant is added to the slot. Meshes are regridded over
// the new picture with weights carried over from the nearest old vertices.
Json Skeleton2DVariantImport(Engine& engine, const Json& args);

}  // namespace oe
