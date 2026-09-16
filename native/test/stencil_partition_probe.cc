// SPDX-License-Identifier: Apache-2.0
//
// `partitionStencil`, checked against a stencil buffer painted the way the GPU paints it.
//
// Each mask writes `(old & ~writeMask) | (value & writeMask)` over its tile, coarsest first, and
// a drawable passes where `(stencil & readMask) == (value & readMask)` -- Vulkan's REPLACE under a
// write mask and EQUAL under a compare mask. So the questions below are asked of pixels, not of
// the numbers: does geometry survive where it should, and only there.
//
//   stencil_partition_probe   (no arguments; exits non-zero on the first failure)

#include <tsf/stencil_partition.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <utility>

namespace {

using tsf::TileID;

/// Every tile is painted at this zoom's resolution, one cell a pixel.
constexpr std::uint8_t kFine = 6;

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

TileID tile(std::uint8_t z, std::uint32_t x, std::uint32_t y, std::uint8_t overscaled = 0) {
    return TileID{z, x, y, 0, overscaled ? overscaled : z};
}

using Buffer = std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint8_t>;

template <typename F>
void forCells(const TileID& t, F&& f) {
    const int shift = kFine - t.z;
    for (std::uint32_t x = t.x << shift; x < (t.x + 1) << shift; ++x) {
        for (std::uint32_t y = t.y << shift; y < (t.y + 1) << shift; ++y) {
            f(x, y);
        }
    }
}

/// Paints every mask, in the order the renderer does.
Buffer paint(const tsf::StencilPartition& partition) {
    Buffer buffer;
    for (const auto& [t, a] : partition.tiles) {
        forCells(t, [&](std::uint32_t x, std::uint32_t y) {
            std::uint8_t& cell = buffer[{x, y}];
            cell = static_cast<std::uint8_t>((cell & ~a.writeMask) | (a.value & a.writeMask));
        });
    }
    return buffer;
}

/// Whether `t`'s geometry passes the stencil at every cell of `where`.
bool passesOver(const tsf::StencilPartition& partition, const Buffer& buffer, const TileID& t,
                const TileID& where) {
    const auto found = partition.tiles.find(t);
    if (found == partition.tiles.end()) {
        return false;
    }
    const auto& a = found->second;
    bool all = true;
    forCells(where, [&](std::uint32_t x, std::uint32_t y) {
        const auto cell = buffer.find({x, y});
        const std::uint8_t value = cell == buffer.end() ? 0 : cell->second;
        all = all && (value & a.readMask) == (a.value & a.readMask);
    });
    return all;
}

/// Whether `t`'s geometry fails the stencil at every cell of `where`.
bool failsOver(const tsf::StencilPartition& partition, const Buffer& buffer, const TileID& t,
               const TileID& where) {
    const auto& a = partition.tiles.at(t);
    bool none = true;
    forCells(where, [&](std::uint32_t x, std::uint32_t y) {
        const auto cell = buffer.find({x, y});
        const std::uint8_t value = cell == buffer.end() ? 0 : cell->second;
        none = none && (value & a.readMask) != (a.value & a.readMask);
    });
    return none;
}

/// Two groups at two zooms, the finer tiles inside the coarser one: both draw everywhere.
///
/// style-lines-with-a-data-driven-property, in miniature. Bright clips against one tile two
/// levels coarser than it is drawn; the example's GeoJSON against tiles at the drawn zoom.
void two_sources_at_two_zooms_both_draw() {
    const TileID basemap = tile(2, 1, 1, 4);
    std::set<TileID> geojson;
    for (std::uint32_t x = 4; x < 7; ++x) {
        for (std::uint32_t y = 4; y < 6; ++y) {
            geojson.insert(tile(4, x, y));
        }
    }
    std::set<TileID> all = geojson;
    all.insert(basemap);
    const auto partition = tsf::partitionStencil(all, {{10, {basemap}}, {119, geojson}});
    const Buffer buffer = paint(partition);

    check(partition.partitioned, "two zooms fit in the byte");
    check(passesOver(partition, buffer, basemap, basemap),
          "the basemap draws over its whole tile, under the GeoJSON's masks too");
    for (const TileID& t : geojson) {
        check(passesOver(partition, buffer, t, t), "each GeoJSON tile draws over itself");
    }
    check(failsOver(partition, buffer, tile(4, 4, 4), tile(4, 5, 4)),
          "a GeoJSON tile is still clipped against its neighbor");
}

/// A parent standing in for a child, in one group: the child's ground is the child's.
void a_child_replaces_its_parent() {
    const TileID parent = tile(2, 1, 1);
    const TileID child = tile(3, 2, 2);
    const auto partition = tsf::partitionStencil({parent, child}, {{5, {parent, child}}});
    const Buffer buffer = paint(partition);

    check(partition.partitioned, "a parent and child fit in the byte");
    check(passesOver(partition, buffer, child, child), "the child draws over itself");
    check(failsOver(partition, buffer, parent, child),
          "the parent does not paint over the child that replaced it");
    check(passesOver(partition, buffer, parent, tile(3, 3, 3)),
          "the parent still draws where no child has arrived");
}

/// The same pair in different groups is two sources, and neither clears the other.
void a_finer_tile_of_another_group_does_not_replace() {
    const TileID coarse = tile(2, 1, 1);
    const TileID fine = tile(3, 2, 2);
    const auto partition = tsf::partitionStencil({coarse, fine}, {{5, {coarse}}, {6, {fine}}});
    const Buffer buffer = paint(partition);

    check(passesOver(partition, buffer, coarse, fine),
          "a coarse source still draws under a finer source's mask");
    check(passesOver(partition, buffer, fine, fine), "the finer source draws over itself");
}

/// One zoom: neighbors clip each other, which is what the mask is for.
void neighbors_at_one_zoom_clip_each_other() {
    const TileID a = tile(3, 0, 0);
    const TileID b = tile(3, 1, 0);
    const auto partition = tsf::partitionStencil({a, b}, {{1, {a, b}}});
    const Buffer buffer = paint(partition);

    check(passesOver(partition, buffer, a, a) && passesOver(partition, buffer, b, b),
          "each tile draws over itself");
    check(failsOver(partition, buffer, a, b) && failsOver(partition, buffer, b, a),
          "a tile's overhang does not paint into its neighbor");
}

/// More than the byte can share: the whole byte per tile, as before, and nothing past 254.
void too_many_tiles_fall_back() {
    std::set<TileID> many;
    for (std::uint32_t x = 0; x < 20; ++x) {
        for (std::uint32_t y = 0; y < 15; ++y) {
            many.insert(tile(8, x, y));
        }
    }
    const auto partition = tsf::partitionStencil(many, {{1, many}});
    check(!partition.partitioned, "three hundred tiles do not partition");
    check(partition.tiles.size() == 254, "the fallback stops at 254 tiles");
    std::set<std::uint8_t> values;
    for (const auto& [t, a] : partition.tiles) {
        values.insert(a.value);
        check(a.readMask == 0xFF && a.writeMask == 0xFF, "the fallback compares the whole byte");
    }
    check(values.size() == 254, "every fallback value is distinct");
}

/// Two zooms whose fields together overflow the byte also fall back.
void fields_that_overflow_fall_back() {
    std::set<TileID> tiles;
    for (std::uint32_t x = 0; x < 16; ++x) {
        for (std::uint32_t y = 0; y < 8; ++y) {
            tiles.insert(tile(8, x, y)); // 128 tiles: eight bits
        }
    }
    tiles.insert(tile(2, 0, 0)); // one more zoom: one more bit
    const auto partition = tsf::partitionStencil(tiles, {{1, tiles}});
    check(!partition.partitioned, "nine bits of fields do not partition");
}

/// The same canonical tile at two drawn zooms is one place, and gets one code.
void one_place_at_two_drawn_zooms_shares_a_code() {
    const TileID stood = tile(3, 1, 1, 3);
    const TileID over = tile(3, 1, 1, 4);
    const auto partition = tsf::partitionStencil({stood, over}, {{1, {stood}}, {2, {over}}});
    check(partition.tiles.at(stood).value == partition.tiles.at(over).value,
          "one canonical tile, one value");
}

} // namespace

int main() {
    two_sources_at_two_zooms_both_draw();
    a_child_replaces_its_parent();
    a_finer_tile_of_another_group_does_not_replace();
    neighbors_at_one_zoom_clip_each_other();
    too_many_tiles_fall_back();
    fields_that_overflow_fall_back();
    one_place_at_two_drawn_zooms_shares_a_code();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("stencil partition: all checks passed\n");
    return EXIT_SUCCESS;
}
