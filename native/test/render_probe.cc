// SPDX-License-Identifier: Apache-2.0
//
// A style in, a PNG out, through tessella's own pipeline.
//
// The whole chain: a map is created and ticked, its ring drained, its order batched, the batches
// turned into Filament renderables, and the result rendered offscreen and written out. No window
// and no compositor -- a headless swap chain and `readPixels` -- which is what makes this runnable
// where the goldens are produced rather than only on a desktop.
//
//   render_probe <style.json> <materialDir> <out.ppm> [lat lon zoom width height]

#include <tsf/filament_renderer.h>
#include <tsf/host.h>

#include <filament/Camera.h>
#include <math/mat4.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace {

void pause_ms(long ms) {
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&req, nullptr);
}

std::string slurp(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return {};
    }
    std::string out;
    char chunk[4096];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
        out.append(chunk, got);
    }
    std::fclose(file);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <style.json> <materialDir> <out.ppm> [lat lon zoom w h]\n",
                     argv[0]);
        return 2;
    }
    const std::string style = slurp(argv[1]);
    if (style.empty()) {
        std::fprintf(stderr, "probe: cannot read %s\n", argv[1]);
        return 2;
    }
    const std::string materialDir = argv[2];
    const char* out = argv[3];
    const double lat = argc > 4 ? std::atof(argv[4]) : 52.52;
    const double lon = argc > 5 ? std::atof(argv[5]) : 13.405;
    const double zoom = argc > 6 ? std::atof(argv[6]) : 14.0;
    const uint32_t W = argc > 7 ? (uint32_t)std::atoi(argv[7]) : 900;
    const uint32_t H = argc > 8 ? (uint32_t)std::atoi(argv[8]) : 700;

    auto* engine = filament::Engine::Builder()
                       .backend(filament::Engine::Backend::VULKAN)
                       .build();
    if (engine == nullptr) {
        std::fprintf(stderr, "probe: no engine\n");
        return 1;
    }
    auto* swapChain = engine->createSwapChain(W, H, filament::SwapChain::CONFIG_READABLE);
    auto* renderer = engine->createRenderer();
    auto* scene = engine->createScene();
    auto* view = engine->createView();
    auto cameraEntity = utils::EntityManager::get().create();
    auto* camera = engine->createCamera(cameraEntity);
    view->setScene(scene);
    view->setCamera(camera);
    view->setViewport({0, 0, W, H});
    // Identity view *and* identity projection, which is load-bearing rather than tidy.
    //
    // With `vertexDomain : device` Filament reads `getPosition()` as clip space and turns it into
    // world with `worldFromClip` -- and then the pipeline projects world back to clip with
    // viewProj. A material that writes `material.worldPosition` is writing the input to that
    // second projection, so tessella's matrix (which already carries tile-local all the way to
    // clip) was being applied and then transformed again by the camera. Every fill landed
    // somewhere off screen; the background survived only because its own matrix happened to.
    //
    // Making viewProj the identity is what reduces `clip = viewProj * worldPosition` to
    // `clip = matrix * position`, which is the arrangement the capture stream assumes.
    camera->setCustomProjection(filament::math::mat4(), -1.0, 1.0);
    camera->setModelMatrix(filament::math::mat4f());
    // No post-processing. Filament tone maps for photographic rendering by default -- ACES, plus
    // bloom and dithering -- and a map is not a photograph: the style already says exactly what
    // colour each thing is, so anything applied on top of that is a deviation from the oracle by
    // construction. It is what left the first correct frame looking bleached.
    view->setPostProcessingEnabled(false);
    renderer->setClearOptions({.clearColor = {0.0f, 0.0f, 0.0f, 1.0f}, .clear = true});

    // Owned rather than stacked, so it can be released *before* the engine. A stack object here
    // is destroyed at the end of main, which is after `Engine::destroy` has already freed every
    // buffer it created -- and the second destroy is a precondition panic that names a vertex
    // buffer rather than the ordering that caused it.
    auto backendOwned = std::make_unique<tsf::FilamentRenderer>(engine, scene, materialDir, W, H);
    tsf::FilamentRenderer& backend = *backendOwned;
    std::printf("materials %zu\n", backend.materials());

    tessella_config config;
    config.style_json = style.c_str();
    config.width = W;
    config.height = H;
    const int ringMb = std::getenv("TSF_PROBE_RING_MB") ? std::atoi(std::getenv("TSF_PROBE_RING_MB")) : 256;
    config.ring_capacity = (size_t)ringMb << 20;

    std::string error;
    std::unique_ptr<tsf::Host> host = tsf::Host::create(config, lat, lon, zoom, &error);
    if (!host) {
        std::fprintf(stderr, "probe: %s\n", error.c_str());
        return 1;
    }

    const int budgetMs = std::getenv("TSF_PROBE_BUDGET_MS")
                             ? std::atoi(std::getenv("TSF_PROBE_BUDGET_MS"))
                             : 60000;
    int waited = 0;
    while (waited < budgetMs) {
        const std::uint64_t seen = host->tick(backend);
        host->retire(seen);
        if (backend.primitives() > 0 && host->readiness() == TESSELLA_READY) {
            break;
        }
        pause_ms(5);
        waited += 5;
    }
    // Then long enough for the rest to land, since the first frame that draws is not the frame
    // that draws everything.
    for (int settle = 0; settle < 600; settle++) {
        const std::uint64_t seen = host->tick(backend);
        host->retire(seen);
        pause_ms(10);
    }

    std::string reason;
    std::printf("readiness %d\n", (int)host->readiness(&reason));
    std::printf("renderables %llu\n", (unsigned long long)backend.renderables());
    std::printf("primitives %llu\n", (unsigned long long)backend.primitives());
    std::printf("instances_made %llu\n", (unsigned long long)backend.made());
    std::printf("instances_coloured %llu\n", (unsigned long long)backend.coloured());
    for (const auto& [z, n] : backend.zooms()) {
        std::printf("zoom_%u %llu\n", (unsigned)z, (unsigned long long)n);
    }
    for (const auto& [pass, n] : backend.passes()) {
        std::printf("pass_%u %llu\n", (unsigned)pass, (unsigned long long)n);
    }
    std::printf("redrawn %llu\n", (unsigned long long)backend.redrawn());
    for (const auto& [z, n] : backend.overZooms()) {
        std::printf("overzoom_%u %llu\n", (unsigned)z, (unsigned long long)n);
    }
    std::printf("shared_slots %llu\n", (unsigned long long)backend.sharedSlots());
    std::printf("scissored %llu\n", (unsigned long long)backend.scissored());
    std::printf("unplaced %llu\n", (unsigned long long)backend.unplaced());
    std::printf("missing_batches %llu\n", (unsigned long long)backend.missing());
    for (std::int32_t family : backend.missingFamilies()) {
        std::printf("missing_family_%d 1\n", family);
    }
    if (!reason.empty()) {
        std::fprintf(stderr, "probe: %s\n", reason.c_str());
    }

    // Rendered once before the capture. A headless swap chain hands out buffers in rotation, and
    // reading back on the very first frame returns one nothing has drawn into -- which produced a
    // constant image that did not move when the scene, the materials, or even the clear colour
    // changed, and cost a long detour before it was noticed.
    for (int warm = 0; warm < 2; warm++) {
        if (renderer->beginFrame(swapChain)) {
            renderer->render(view);
            renderer->endFrame();
        }
        engine->flushAndWait();
    }

    std::vector<uint8_t> pixels(W * H * 4);
    filament::backend::PixelBufferDescriptor pb(pixels.data(), pixels.size(),
                                                filament::backend::PixelDataFormat::RGBA,
                                                filament::backend::PixelDataType::UBYTE);
    if (renderer->beginFrame(swapChain)) {
        renderer->render(view);
        renderer->readPixels(0, 0, W, H, std::move(pb));
        renderer->endFrame();
    }
    engine->flushAndWait();

    size_t lit = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i] || pixels[i + 1] || pixels[i + 2]) lit++;
    }
    std::printf("lit_pixels %zu of %u\n", lit, W * H);

    // Plain PPM: no encoder to link, and anything can read it.
    if (std::FILE* png = std::fopen(out, "wb")) {
        std::fprintf(png, "P6\n%u %u\n255\n", W, H);
        // Bottom-up out of the GPU, top-down into the file.
        for (uint32_t y = 0; y < H; y++) {
            const uint8_t* row = pixels.data() + (size_t)(H - 1 - y) * W * 4;
            for (uint32_t x = 0; x < W; x++) {
                std::fwrite(row + (size_t)x * 4, 1, 3, png);
            }
        }
        std::fclose(png);
        std::printf("wrote 1\n");
    }

    backendOwned.reset();
    engine->destroy(view);
    engine->destroy(scene);
    engine->destroy(renderer);
    engine->destroyCameraComponent(cameraEntity);
    utils::EntityManager::get().destroy(cameraEntity);
    engine->destroy(swapChain);
    filament::Engine::destroy(&engine);
    return 0;
}
