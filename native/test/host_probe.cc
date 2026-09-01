// SPDX-License-Identifier: Apache-2.0
//
// Drives `tsf::Host` over a real map and counts what a backend would have been asked to draw.
//
// This is the join, exercised end to end: a map is created, ticked, its ring drained, its order
// batched, and the batches delivered -- every link in the chain that stands between a style and
// a picture, minus the GPU. The renderer here counts instead of uploading, which is what makes
// the whole thing testable without a device.
//
// Prints `name value` lines for the Rust side to read.

#include <tsf/host.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_set>

namespace {

/// A backend that draws nothing and remembers everything.
class CountingRenderer final : public tsf::Renderer {
public:
    void beginFrame(std::uint64_t) override { frames++; }
    void endFrame(std::uint64_t) override { framesEnded++; }

    void onGeometry(const tsf::DrawableAdd& add) override {
        geometries++;
        vertices += add.vertexCount;
        if (add.indexes.empty()) {
            unresolvedIndexes++;
        }
        live.insert(add.id);
    }

    void onRetire(std::uint64_t id) override {
        retires++;
        live.erase(id);
    }

    void onBatch(const tsf::Batch& batch) override {
        batches++;
        drawn += batch.geometries.size();
        if (batch.geometries.size() != batch.uboIndexes.size()) {
            malformed++;
        }
        // A batch naming geometry the backend was never given is the one failure that would put
        // a dangling buffer on the GPU. It cannot happen through `DrawList` -- an unknown id is
        // skipped -- and this is what says so rather than assuming it.
        for (std::uint64_t id : batch.geometries) {
            if (live.find(id) == live.end()) {
                unknownInBatch++;
            }
        }
    }

    void onUniforms(const tsf::UboUpdate&) override { uniforms++; }
    void onTexture(const tsf::TextureUpdate&) override { textures++; }

    std::uint64_t frames = 0, framesEnded = 0;
    std::uint64_t geometries = 0, vertices = 0, unresolvedIndexes = 0, retires = 0;
    std::uint64_t batches = 0, drawn = 0, malformed = 0, unknownInBatch = 0;
    std::uint64_t uniforms = 0, textures = 0;
    std::unordered_set<std::uint64_t> live;
};

void pause_ms(long ms) {
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&req, nullptr);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <style.json>\n", argv[0]);
        return 2;
    }

    std::FILE* file = std::fopen(argv[1], "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "probe: cannot open %s\n", argv[1]);
        return 2;
    }
    std::string style;
    char chunk[4096];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
        style.append(chunk, got);
    }
    std::fclose(file);

    tessella_config config;
    config.style_json = style.c_str();
    config.width = 1024;
    config.height = 768;
    config.ring_capacity = 1u << 22;

    std::string error;
    std::unique_ptr<tsf::Host> host = tsf::Host::create(config, 51.505, -0.11, 4.0, &error);
    std::printf("created %d\n", host ? 1 : 0);
    if (!host) {
        std::fprintf(stderr, "probe: %s\n", error.c_str());
        return 1;
    }

    CountingRenderer renderer;
    std::uint64_t seen = 0;

    // The loop a consumer runs: tick, draw what came, release what the driver is done with. This
    // one is done the moment the call returns, because it copied nothing and holds nothing.
    for (int spin = 0; spin < 600; spin++) {
        seen = host->tick(renderer);
        host->retire(seen);
        if (renderer.batches > 0 && host->readiness() == TESSELLA_READY) {
            break;
        }
        pause_ms(5);
    }

    std::string reason;
    const tessella_readiness readiness = host->readiness(&reason);

    std::printf("readiness %d\n", (int)readiness);
    std::printf("last_result %d\n", (int)host->lastResult());
    std::printf("records %llu\n", (unsigned long long)host->records());
    std::printf("frames %llu\n", (unsigned long long)renderer.frames);
    std::printf("frames_ended %llu\n", (unsigned long long)renderer.framesEnded);
    std::printf("geometries %llu\n", (unsigned long long)renderer.geometries);
    std::printf("vertices %llu\n", (unsigned long long)renderer.vertices);
    std::printf("unresolved_indexes %llu\n", (unsigned long long)renderer.unresolvedIndexes);
    std::printf("retires %llu\n", (unsigned long long)renderer.retires);
    std::printf("batches %llu\n", (unsigned long long)renderer.batches);
    std::printf("drawn %llu\n", (unsigned long long)renderer.drawn);
    std::printf("malformed %llu\n", (unsigned long long)renderer.malformed);
    std::printf("unknown_in_batch %llu\n", (unsigned long long)renderer.unknownInBatch);
    std::printf("uniforms %llu\n", (unsigned long long)renderer.uniforms);
    std::printf("textures %llu\n", (unsigned long long)renderer.textures);
    std::printf("cursor_advanced %d\n", seen > 0 ? 1 : 0);
    if (!reason.empty()) {
        std::fprintf(stderr, "probe: readiness reason: %s\n", reason.c_str());
    }
    return 0;
}
