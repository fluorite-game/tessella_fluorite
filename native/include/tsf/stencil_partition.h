// SPDX-License-Identifier: Apache-2.0
//
// How the clip masks share one stencil byte.
//
// mbgl repaints the stencil before each layer group, so at any moment the buffer holds one
// group's tiles and nothing else. A scene drawn in one pass cannot: every group's masks are in
// the buffer together. That is harmless while the groups agree on a grid, and it is not when
// they do not -- style-lines-with-a-data-driven-property clips bright's layers against one z14
// tile drawn at 16 and the example's own GeoJSON against six z16 tiles inside it, and a single
// value per pixel handed the whole z14 tile to the GeoJSON. The basemap there vanished.
//
// So the byte is shared out by canonical zoom: a field of bits for each zoom, a mask writing
// only its own field, and geometry comparing only its own. Two zooms then coexist, and a finer
// tile still replaces its own ancestor, by clearing that ancestor's field -- but only where one
// layer group asked for both. Across groups a finer tile is a different source, not a
// replacement.
//
// Pure, so it can be tested without a GPU; the renderer paints what this returns.

#pragma once

#include <tsf/frame.h>

#include <cstdint>
#include <map>
#include <set>

namespace tsf {

/// How one tile's mask is painted and how its geometry tests against it.
struct StencilAssignment {
    /// Written by the mask, compared by the geometry.
    std::uint8_t value = 0;
    /// The bits the geometry compares: its own zoom's field.
    std::uint8_t readMask = 0xFF;
    /// The bits the mask writes: its own field, and those of any ancestor it replaces.
    std::uint8_t writeMask = 0xFF;
};

/// Every tile's assignment for one frame.
struct StencilPartition {
    /// Whether the fields fit in the byte. When they do not, each tile has a value of its own in
    /// the whole byte, which is the scheme this replaces and is right for a single zoom.
    bool partitioned = false;
    /// A tile absent here has no mask; its geometry is left unclipped.
    std::map<TileID, StencilAssignment> tiles;
};

/// Shares the stencil byte among `tiles`.
///
/// `groups` is each layer group's own tile set, as the producer named it; a finer tile clears an
/// ancestor's field only when some group holds both. Every tile in a group must be in `tiles`.
[[nodiscard]] StencilPartition partitionStencil(
        const std::set<TileID>& tiles, const std::map<std::int32_t, std::set<TileID>>& groups);

} // namespace tsf
