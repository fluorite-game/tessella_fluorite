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
#include <map>
#include <set>
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
        shaders[batch.builtinShader].insert(batch.permutationKey);
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

    /// Every distinct (shader, permutation) a frame asked for.
    ///
    /// This is the material inventory, measured rather than guessed: a Filament backend needs one
    /// material per shader family and a parameterisation per permutation, so what a real style
    /// actually emits is the list of things that have to be authored. The ABI declares
    /// thirty-five shader families; a given style uses a handful, and knowing which handful is
    /// the difference between porting everything and porting what is needed.
    std::map<std::int32_t, std::set<std::uint64_t>> shaders;
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
        std::fprintf(stderr, "usage: %s <style.json> [lat lon zoom]\n", argv[0]);
        return 2;
    }
    // The camera decides the inventory as much as the style does: a style with symbol and
    // fill-extrusion layers draws neither when it is pointed at a zoom whose tiles carry no
    // labels and no buildings. Left as an argument rather than fixed, because "which shaders
    // does this style need" is not a question a style answers on its own.
    const double lat = argc > 2 ? std::atof(argv[2]) : 51.505;
    const double lon = argc > 3 ? std::atof(argv[3]) : -0.11;
    const double zoom = argc > 4 ? std::atof(argv[4]) : 4.0;

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

    tessella_config config{};
    config.style_json = reinterpret_cast<const uint8_t*>(style.data());
    config.style_json_len = style.size();
    config.width = 1024;
    config.height = 768;
    // A style's first frame is as big as the style. liberty draws a hundred and eleven layers
    // over two dozen tiles, and four megabytes does not hold it -- see TSF_PROBE_RING_MB.
    const int ringMb = std::getenv("TSF_PROBE_RING_MB") ? std::atoi(std::getenv("TSF_PROBE_RING_MB")) : 4;
    config.ring_capacity = (size_t)ringMb << 20;
    // Zero takes the producer's default.
    config.slab_capacity = 0;

    std::string error;
    std::unique_ptr<tsf::Host> host = tsf::Host::create(config, lat, lon, zoom, &error);
    std::printf("created %d\n", host ? 1 : 0);
    if (!host) {
        std::fprintf(stderr, "probe: %s\n", error.c_str());
        return 1;
    }

    CountingRenderer renderer;
    std::uint64_t seen = 0;

    // The loop a consumer runs: tick, draw what came, release what the driver is done with. This
    // one is done the moment the call returns, because it copied nothing and holds nothing.
    // A deadline rather than a spin count, because how long a style takes to draw its first
    // frame is a property of the style. demotiles is eight layers over small tiles and lands in
    // well under a second; OpenFreeMap's liberty is a hundred and eleven layers over tiles of
    // several hundred kilobytes each, and a spin count tuned for the first reports the second as
    // drawing nothing at all.
    const int budgetMs = std::getenv("TSF_PROBE_BUDGET_MS")
                             ? std::atoi(std::getenv("TSF_PROBE_BUDGET_MS"))
                             : 60000;
    int waited = 0;
    while (waited < budgetMs) {
        seen = host->tick(renderer);
        host->retire(seen);
        if (renderer.batches > 0 && host->readiness() == TESSELLA_READY) {
            break;
        }
        pause_ms(5);
        waited += 5;
    }

    // Then kept going. The first frame that draws anything is not the frame that draws
    // everything -- tiles land one at a time -- and an inventory taken at the first batch would
    // list whichever shaders happened to win the race. These are cheap: a settled map emits
    // nothing and each of these walks no records.
    // Long enough for the *second* wave. Tiles land first and glyphs only after them -- which
    // glyphs a style needs is a property of the data, so nothing can be asked for until the
    // tiles that name them have arrived -- and a symbol bucket is withheld until its glyphs are
    // in hand. A settle that only outlasts the tiles reports a style as needing no symbol
    // shaders when what it needed was another second.
    for (int settle = 0; settle < 600; settle++) {
        seen = host->tick(renderer);
        host->retire(seen);
        pause_ms(10);
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

    // The inventory. Counted first so a caller can assert on the totals, then listed so a reader
    // can see which families they are.
    std::uint64_t permutations = 0;
    for (const auto& entry : renderer.shaders) {
        permutations += entry.second.size();
    }
    std::printf("shader_families %zu\n", renderer.shaders.size());
    std::printf("shader_permutations %llu\n", (unsigned long long)permutations);
    for (const auto& entry : renderer.shaders) {
        std::printf("shader_%d %zu\n", entry.first, entry.second.size());
        // The keys themselves, not just how many. A permutation key is a bitmask over the
        // family's attribute ids saying which paint properties reached the shader as uniforms
        // rather than as vertex attributes -- so its *value* is what says whether two styles need
        // the same material or two different ones.
        for (std::uint64_t key : entry.second) {
            std::printf("permutation_%d_%llu 1\n", entry.first, (unsigned long long)key);
        }
    }
    if (!reason.empty()) {
        std::fprintf(stderr, "probe: readiness reason: %s\n", reason.c_str());
    }
    return 0;
}
