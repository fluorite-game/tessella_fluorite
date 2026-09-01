// SPDX-License-Identifier: Apache-2.0
//
// Drives `tsf::Reader` over a ring and slab region the tessella producer wrote, and prints
// what the sink saw.
//
// The counterpart of tessella's own `consumer.c`, and for the same reason: a consumer written in
// the producer's language, sharing its types, cannot fail the way a real one does. This one is
// C++, takes only the flat header, and is the code the Filament mirror will actually run -- so
// what it counts is what the mirror would draw.
//
//   tessella-reader-probe <ring.bin> <slabs.bin>

#include <tsf/drawlist.h>
#include <tsf/reader.h>

#include <cstdio>
#include <vector>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/// Counts everything the reader delivers, and checks what it can along the way.
class CountingSink final : public tsf::FrameSink {
public:
    void beginFrame(std::uint64_t) override { frames_begun++; }
    void endFrame(std::uint64_t frameNo) override {
        frames_ended++;
        lastFrameNo = frameNo;
    }

    void onDrawableAdd(const tsf::DrawableAdd& add) override {
        drawlist.observe(add);
        drawables++;
        vertices += add.vertexCount;
        indices += add.indexCount();
        if (add.indexes.empty()) {
            unresolved_indexes++;
        }
        if (add.tileID) {
            tiled++;
        }
        // The join is the reader's whole reason for existing, so the joined record is checked
        // rather than assumed: a drawable with a shader but no geometry behind it would mean the
        // add and the use were paired wrongly.
        if (add.builtinShader == 0 || add.vertexCount == 0) {
            joined_badly++;
        }
    }

    void onDrawableRemove(const tsf::DrawableRemove& gone) override {
        if (gone.view) {
            releases++;
        } else {
            // A release names a view; a remove retires the geometry itself, and only then is
            // there nothing left to batch.
            drawlist.forget(gone.id);
            removes++;
        }
    }

    void onUboUpdate(const tsf::UboUpdate& update) override {
        ubos++;
        ubo_bytes += update.bytes.size;
        if (!update.layerIndex) {
            frame_wide_ubos++;
        }
    }

    void onTextureUpdate(const tsf::TextureUpdate& update) override {
        textures++;
        texture_bytes += update.pixels.size;
        const std::uint32_t pixel = update.pixelSize();
        if (pixel == 0) {
            texture_bad++;
            return;
        }
        // Empty means the whole texture, which the bytes must account for exactly.
        std::uint64_t want = 0;
        if (update.rects.empty()) {
            whole_texture++;
            want = std::uint64_t{update.width} * update.height * pixel;
        } else {
            for (const auto& rect : update.rects) {
                rects++;
                want += std::uint64_t{rect.w} * rect.h * pixel;
            }
        }
        if (want != update.pixels.size) {
            texture_bad++;
        }
    }

    void onStencilTiles(const tsf::StencilTiles& stencil) override {
        stencils++;
        stencil_tiles += stencil.tiles.size();
    }

    void onFrameOrder(const tsf::FrameOrder& order) override {
        orders++;
        order_entries += order.entries.size();

        // What §11.7 asks the consumer to do with an order, done here so the count is taken on a
        // real frame rather than on a fixture shaped to flatter it.
        const std::vector<tsf::Batch> batches = drawlist.build(order);
        batch_count += batches.size();
        for (const tsf::Batch& batch : batches) {
            batched_geometries += batch.geometries.size();
            if (batch.uboIndexes.size() != batch.geometries.size()) {
                batch_slots_mismatched++;
            }
            if (tsf::DrawList::isSymbol(batch.builtinShader) && batch.merged()) {
                symbols_collapsed++;
            }
        }

        // The picture a batched frame draws must be the one the order described: same geometries,
        // same sequence. Flattening the batches back out has to reproduce the order exactly,
        // minus entries whose geometry this consumer never saw.
        std::vector<std::uint64_t> flattened;
        for (const tsf::Batch& batch : batches) {
            for (std::uint64_t id : batch.geometries) {
                flattened.push_back(id);
            }
        }
        std::vector<std::uint64_t> expected;
        for (const tsl_order_entry& entry : order.entries) {
            if (drawlist.knows(entry.geometry)) {
                expected.push_back(entry.geometry);
            }
        }
        if (flattened != expected) {
            order_diverged++;
        }
        if (order.camera) {
            cameras++;
            // An order delivered without the camera that commits it would mean the epoch pairing
            // failed, which draws this frame's camera over the last frame's painter order.
            if (order.camera->order_epoch != order.orderEpoch) {
                epoch_mismatch++;
            }
        } else {
            orphan_orders++;
        }
    }

    void onViewDeclare(std::uint32_t, std::uint8_t) override { declares++; }
    void onViewUndeclare(std::uint32_t) override { undeclares++; }
    void onMeshAdd(std::uint64_t, std::uint8_t, tsf::Bytes bytes) override {
        meshes++;
        mesh_bytes += bytes.size;
    }

    std::uint64_t frames_begun = 0, frames_ended = 0, lastFrameNo = 0;
    std::uint64_t drawables = 0, vertices = 0, indices = 0, tiled = 0;
    std::uint64_t unresolved_indexes = 0, joined_badly = 0;
    std::uint64_t removes = 0, releases = 0;
    std::uint64_t ubos = 0, ubo_bytes = 0, frame_wide_ubos = 0;
    std::uint64_t textures = 0, texture_bytes = 0, texture_bad = 0, rects = 0, whole_texture = 0;
    std::uint64_t stencils = 0, stencil_tiles = 0;
    std::uint64_t orders = 0, order_entries = 0, cameras = 0, epoch_mismatch = 0, orphan_orders = 0;

    /// What §11.7's batching produced, and the three ways it could be wrong.
    tsf::DrawList drawlist;
    std::uint64_t batch_count = 0, batched_geometries = 0;
    /// A batch whose slot list does not match its geometry list -- the two are parallel by
    /// construction, so a mismatch means a run was extended without its UBO index.
    std::uint64_t batch_slots_mismatched = 0;
    /// A symbol that got collapsed, which R-9 forbids until the collapse is measured.
    std::uint64_t symbols_collapsed = 0;
    /// A frame whose batches, flattened, are not the order that was delivered. The property the
    /// whole design exists to keep.
    std::uint64_t order_diverged = 0;
    std::uint64_t declares = 0, undeclares = 0, meshes = 0, mesh_bytes = 0;
};

tsf::Region mapShared(const char* path) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        std::fprintf(stderr, "probe: cannot open %s\n", path);
        std::exit(2);
    }
    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        std::fprintf(stderr, "probe: cannot size %s\n", path);
        std::exit(2);
    }
    void* at = mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (at == MAP_FAILED) {
        std::fprintf(stderr, "probe: cannot map %s\n", path);
        std::exit(2);
    }
    return {static_cast<const std::uint8_t*>(at), static_cast<std::size_t>(info.st_size)};
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <ring.bin> <slabs.bin>\n", argv[0]);
        return 2;
    }
    const tsf::Region ring = mapShared(argv[1]);
    const tsf::Region slabs = mapShared(argv[2]);

    CountingSink sink;
    tsf::Reader reader(ring, slabs);
    const std::size_t records = reader.drain(sink);

    std::printf("records %zu\n", records);
    std::printf("unknown_records %llu\n", (unsigned long long)reader.unknownRecords());
    std::printf("frames_begun %llu\n", (unsigned long long)sink.frames_begun);
    std::printf("frames_ended %llu\n", (unsigned long long)sink.frames_ended);
    std::printf("drawables %llu\n", (unsigned long long)sink.drawables);
    std::printf("vertices %llu\n", (unsigned long long)sink.vertices);
    std::printf("indices %llu\n", (unsigned long long)sink.indices);
    std::printf("tiled %llu\n", (unsigned long long)sink.tiled);
    std::printf("unresolved_indexes %llu\n", (unsigned long long)sink.unresolved_indexes);
    std::printf("joined_badly %llu\n", (unsigned long long)sink.joined_badly);
    std::printf("removes %llu\n", (unsigned long long)sink.removes);
    std::printf("batches %llu\n", (unsigned long long)sink.batch_count);
    std::printf("batched_geometries %llu\n", (unsigned long long)sink.batched_geometries);
    std::printf("batch_slots_mismatched %llu\n", (unsigned long long)sink.batch_slots_mismatched);
    std::printf("symbols_collapsed %llu\n", (unsigned long long)sink.symbols_collapsed);
    std::printf("order_diverged %llu\n", (unsigned long long)sink.order_diverged);
    std::printf("releases %llu\n", (unsigned long long)sink.releases);
    std::printf("ubos %llu\n", (unsigned long long)sink.ubos);
    std::printf("ubo_bytes %llu\n", (unsigned long long)sink.ubo_bytes);
    std::printf("frame_wide_ubos %llu\n", (unsigned long long)sink.frame_wide_ubos);
    std::printf("textures %llu\n", (unsigned long long)sink.textures);
    std::printf("texture_bytes %llu\n", (unsigned long long)sink.texture_bytes);
    std::printf("texture_bad %llu\n", (unsigned long long)sink.texture_bad);
    std::printf("rects %llu\n", (unsigned long long)sink.rects);
    std::printf("whole_texture %llu\n", (unsigned long long)sink.whole_texture);
    std::printf("stencils %llu\n", (unsigned long long)sink.stencils);
    std::printf("stencil_tiles %llu\n", (unsigned long long)sink.stencil_tiles);
    std::printf("orders %llu\n", (unsigned long long)sink.orders);
    std::printf("order_entries %llu\n", (unsigned long long)sink.order_entries);
    std::printf("cameras %llu\n", (unsigned long long)sink.cameras);
    std::printf("epoch_mismatch %llu\n", (unsigned long long)sink.epoch_mismatch);
    std::printf("orphan_orders %llu\n", (unsigned long long)sink.orphan_orders);
    std::printf("declares %llu\n", (unsigned long long)sink.declares);
    std::printf("undeclares %llu\n", (unsigned long long)sink.undeclares);
    std::printf("meshes %llu\n", (unsigned long long)sink.meshes);
    std::printf("mesh_bytes %llu\n", (unsigned long long)sink.mesh_bytes);
    return 0;
}
