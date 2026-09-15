// SPDX-License-Identifier: Apache-2.0
//
// Groups a frame's draw order into the fewest renderables that still draw the same picture.

#ifndef TSF_DRAWLIST_H
#define TSF_DRAWLIST_H

#include <tsf/frame.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace tsf {

/// One run of drawables issued as a single renderable.
///
/// The geometries are in draw order and stay that way: a batch is always a *contiguous* run of
/// the frame's order, never a gather of like things from across it. See `DrawList` for why that
/// distinction is the whole design.
struct Batch {
    std::uint32_t view = 0;
    std::uint32_t layerIndex = 0;
    std::uint8_t pass = 0;

    std::int32_t builtinShader = 0;
    std::uint64_t permutationKey = 0;
    /// `tsl_topology`. Part of the merge key: the location indicator's circle is a fan and a
    /// strip of one family over one vertex buffer, adjacent in the order and alike in every
    /// other respect, and one renderable draws one primitive.
    std::uint8_t topology = 0;
    std::vector<TextureBinding> textureRefs;

    /// Geometry ids, in draw order.
    std::vector<std::uint64_t> geometries;
    /// The consolidated-buffer slot for each, parallel to `geometries`.
    std::vector<std::uint32_t> uboIndexes;

    [[nodiscard]] bool merged() const noexcept { return geometries.size() > 1; }
};

/// Turns a frame's order into batches.
///
/// # What it merges, and what it refuses to
///
/// §11.7 asks for geometry batching by (layer, shader permutation, texture set). R-9 is the
/// reason it is not simply a group-by: merging drawables into multi-primitive renderables assumes
/// layer-contiguous draw order, and translucent layers with cross-tile sort keys -- symbol fade,
/// line sort-key -- can violate that assumption. A group-by would happily merge the first and
/// third entries of a run and silently move the second one behind them.
///
/// So this merges only *adjacent* entries. A run is extended while the merge key holds and broken
/// the moment it does not, which makes the painter order it produces identical to the one it was
/// given -- not approximately, but by construction, because nothing is ever reordered. That is
/// R-9's "collapse only within (layer, pass) groups the order proves contiguous", and the proof is
/// that adjacency is the only thing it ever collapses.
///
/// Symbols are excluded entirely, as R-9 requires until the collapse is measured: every symbol
/// drawable is its own batch.
///
/// # What it is not responsible for
///
/// It does not upload, own, or outlive anything. A `Batch` names geometry ids the caller has
/// already seen through `onDrawableAdd`, and holding the bytes behind them is the caller's, on
/// the terms §11.7 states.
class DrawList {
public:
    /// Records a drawable, so a later order can find what it needs to batch by.
    void observe(const DrawableAdd& add);

    /// Forgets one, when its geometry is removed.
    void forget(std::uint64_t id);

    /// Groups `order` into batches, in draw order.
    ///
    /// An entry whose geometry has not been seen is skipped *and* breaks the run: a consumer that
    /// merged across a hole would draw the two sides of a missing tile as though they were
    /// adjacent, which is exactly the reordering this refuses to do elsewhere.
    [[nodiscard]] std::vector<Batch> build(const FrameOrder& order) const;

    /// How many drawables are known.
    [[nodiscard]] std::size_t known() const noexcept { return byId_.size(); }

    /// Whether one is, which is what decides if an order entry can be drawn at all.
    [[nodiscard]] bool knows(std::uint64_t id) const noexcept { return byId_.count(id) != 0; }

    /// Whether a builtin shader draws symbols, and so is held out of collapse.
    [[nodiscard]] static bool isSymbol(std::int32_t builtinShader) noexcept;

private:
    /// What batching needs from a drawable, kept rather than the whole record.
    struct Known {
        std::int32_t builtinShader = 0;
        std::uint64_t permutationKey = 0;
        std::uint8_t topology = 0;
        std::vector<TextureBinding> textureRefs;
        /// The view whose use joined this geometry.
        ///
        /// Not the order's. An order is per view and its entries carry none, which was right
        /// while every drawable in a view's order belonged to that view -- and a heatmap's
        /// kernels do not: they are bound into an offscreen view of their own (DR-25) and
        /// ordered in the map's, because that is where their place in the painter's sequence
        /// is. Taking the view from the order stamped them as the map's and they drew onto it.
        std::uint32_t view = 0;
    };

    std::unordered_map<std::uint64_t, Known> byId_;
};

} // namespace tsf

#endif // TSF_DRAWLIST_H
