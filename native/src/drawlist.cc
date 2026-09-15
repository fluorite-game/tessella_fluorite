// SPDX-License-Identifier: Apache-2.0

#include <tsf/drawlist.h>

#include <tessella_capture_abi.h>

namespace tsf {

bool DrawList::isSymbol(std::int32_t builtinShader) noexcept {
    switch (builtinShader) {
        case TSL_BUILTIN_SYMBOL_ICON_SHADER:
        case TSL_BUILTIN_SYMBOL_SDFSHADER:
        case TSL_BUILTIN_SYMBOL_TEXT_AND_ICON_SHADER:
        case TSL_BUILTIN_CUSTOM_SYMBOL_ICON_SHADER:
            return true;
        default:
            return false;
    }
}

void DrawList::observe(const DrawableAdd& add) {
    Known known;
    known.builtinShader = add.builtinShader;
    known.permutationKey = add.permutationKey;
    known.topology = add.topology;
    known.textureRefs = add.textureRefs;
    known.view = add.view;
    // Assigned rather than inserted: a drawable re-announced with modified attributes keeps its
    // id, and the newer record is the one that describes what will be drawn.
    byId_[add.id] = std::move(known);
}

void DrawList::forget(std::uint64_t id) {
    byId_.erase(id);
}

std::vector<Batch> DrawList::build(const FrameOrder& order) const {
    std::vector<Batch> batches;
    batches.reserve(order.entries.size());

    // The run being extended. `open` is separate from `batches.empty()` because a skipped entry
    // closes the run without starting another.
    bool open = false;

    for (const tsl_order_entry& entry : order.entries) {
        const auto found = byId_.find(entry.geometry);
        if (found == byId_.end()) {
            // Nothing to draw, and nothing to merge across: see the header on why a hole breaks
            // the run rather than being stepped over.
            open = false;
            continue;
        }
        const Known& known = found->second;

        const bool symbol = isSymbol(known.builtinShader);
        const bool extends = open && !symbol && [&] {
            const Batch& run = batches.back();
            // Never across views: two drawables in different views are two passes, and one
            // renderable cannot be in both.
            return run.view == known.view && run.layerIndex == entry.layer_index &&
                   run.pass == entry.pass && run.builtinShader == known.builtinShader &&
                   run.permutationKey == known.permutationKey &&
                   run.topology == known.topology &&
                   run.textureRefs == known.textureRefs;
        }();

        if (extends) {
            Batch& run = batches.back();
            run.geometries.push_back(entry.geometry);
            run.uboIndexes.push_back(entry.ubo_index);
            continue;
        }

        Batch fresh;
        // The drawable's view, not the order's -- see `Known::view`.
        fresh.view = known.view;
        fresh.layerIndex = entry.layer_index;
        fresh.pass = entry.pass;
        fresh.builtinShader = known.builtinShader;
        fresh.permutationKey = known.permutationKey;
        fresh.topology = known.topology;
        fresh.textureRefs = known.textureRefs;
        fresh.geometries.push_back(entry.geometry);
        fresh.uboIndexes.push_back(entry.ubo_index);
        batches.push_back(std::move(fresh));
        // A symbol batch is closed the moment it opens, so the next entry cannot join it.
        open = !symbol;
    }

    return batches;
}

} // namespace tsf
