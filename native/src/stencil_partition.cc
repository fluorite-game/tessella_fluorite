// SPDX-License-Identifier: Apache-2.0

#include <tsf/stencil_partition.h>

#include <cstddef>
#include <tuple>

namespace tsf {
namespace {

/// The bits needed to count to `n`, since zero means "no mask". Eight at most.
std::uint8_t bitsFor(std::size_t n) {
    std::uint8_t width = 0;
    while (width < 8 && (std::size_t{1} << width) <= n) {
        ++width;
    }
    return width;
}

struct Field {
    std::uint8_t shift = 0;
    std::uint8_t mask = 0;
};

} // namespace

StencilPartition partitionStencil(
        const std::set<TileID>& tiles, const std::map<std::int32_t, std::set<TileID>>& groups) {
    StencilPartition out;

    // One code per place at each zoom. The overscaled zoom is not part of it: two sources can
    // name the same canonical tile at different drawn zooms, and they cover the same ground.
    using Cell = std::tuple<std::int32_t, std::uint32_t, std::uint32_t>;
    std::map<std::uint8_t, std::map<Cell, std::uint8_t>> codes;
    for (const TileID& tile : tiles) {
        codes[tile.z].emplace(Cell{tile.wrap, tile.x, tile.y}, 0);
    }

    // Coarsest zoom in the lowest bits. The order is a choice, not a requirement: the fields
    // are disjoint whichever way round they go.
    std::map<std::uint8_t, Field> fields;
    out.partitioned = true;
    unsigned offset = 0;
    for (auto& [zoom, cells] : codes) {
        const std::uint8_t width = bitsFor(cells.size());
        if (cells.size() > 255 || offset + width > 8) {
            out.partitioned = false;
            break;
        }
        std::uint8_t code = 0;
        for (auto& [cell, value] : cells) {
            value = ++code;
        }
        fields[zoom] = Field{static_cast<std::uint8_t>(offset),
                             static_cast<std::uint8_t>(((1u << width) - 1u) << offset)};
        offset += width;
    }

    if (!out.partitioned) {
        // A value per tile in the whole byte, stopping short of 255 as the renderer always has.
        std::uint8_t next = 1;
        for (const TileID& tile : tiles) {
            if (next == 255) {
                break;
            }
            out.tiles[tile] = StencilAssignment{next++, 0xFF, 0xFF};
        }
        return out;
    }

    for (const TileID& tile : tiles) {
        const Field& field = fields.at(tile.z);

        // The fields of the coarser tiles this one lies inside, within any one group holding
        // both. Clearing them is what makes a child replace the parent standing in for it.
        std::uint8_t cleared = 0;
        for (const auto& [group, members] : groups) {
            if (members.count(tile) == 0) {
                continue;
            }
            for (const TileID& other : members) {
                if (other.z >= tile.z || other.wrap != tile.wrap) {
                    continue;
                }
                const int shift = tile.z - other.z;
                if ((tile.x >> shift) == other.x && (tile.y >> shift) == other.y) {
                    cleared |= fields.at(other.z).mask;
                }
            }
        }

        const std::uint8_t code = codes.at(tile.z).at(Cell{tile.wrap, tile.x, tile.y});
        out.tiles[tile] = StencilAssignment{static_cast<std::uint8_t>(code << field.shift),
                                            field.mask,
                                            static_cast<std::uint8_t>(field.mask | cleared)};
    }
    return out;
}

} // namespace tsf
