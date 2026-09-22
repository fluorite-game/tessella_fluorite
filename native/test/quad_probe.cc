// SPDX-License-Identifier: Apache-2.0
//
// Four maps, one engine, one scene, four Filament views -- the quad, headless.
//
//   quad_probe <style.json> <materialDir> <out.ppm> [paneW paneH]
//
// What this is for: Fluorite hands every platform view the same scene and
// differs them by camera, so four maps in one app share a scene and would each
// draw in all four panes without layer masks. That is the part of the quad worth
// proving before any Flutter is involved, and it needs no embedder to prove --
// four `tsf::MapView`s on one engine, one per layer, four viewports over one
// swapchain.

#include <tsf/filament_renderer.h>
#include <tsf/map_view.h>

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace {

struct City {
    const char* name;
    double latitude;
    double longitude;
    double zoom;
};

// Reading order, matching the Flutter app's `kQuad`.
constexpr std::array<City, 4> kCities{{
    {"Seattle", 47.6062, -122.3321, 13.0},
    {"Tokyo", 35.6812, 139.7671, 15.0},
    {"Liestal", 47.4839, 7.7345, 12.0},
    {"Shanghai", 31.2304, 121.4737, 14.0},
}};

std::string slurp(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return {};
    std::string out;
    char buffer[8192];
    while (const std::size_t read = std::fread(buffer, 1, sizeof buffer, file)) {
        out.append(buffer, read);
    }
    std::fclose(file);
    return out;
}

void pause_ms(const long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&ts, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <style.json> <materialDir> <out.ppm> [paneW paneH]\n",
                     argv[0]);
        return 2;
    }
    const std::string style = slurp(argv[1]);
    if (style.empty()) {
        std::fprintf(stderr, "quad: cannot read %s\n", argv[1]);
        return 2;
    }
    const std::string materialDir = argv[2];
    const char* out = argv[3];
    const long paneWArg = argc > 4 ? std::strtol(argv[4], nullptr, 10) : 640;
    const long paneHArg = argc > 5 ? std::strtol(argv[5], nullptr, 10) : 480;
    if (paneWArg < 1 || paneWArg > 8192 || paneHArg < 1 || paneHArg > 8192) {
        std::fprintf(stderr, "quad: pane size out of range (1..8192)\n");
        return 2;
    }
    const std::uint32_t paneW = static_cast<std::uint32_t>(paneWArg);
    const std::uint32_t paneH = static_cast<std::uint32_t>(paneHArg);
    const std::uint32_t W = paneW * 2;
    const std::uint32_t H = paneH * 2;

    auto* engine =
        filament::Engine::Builder().backend(filament::Engine::Backend::VULKAN).build();
    if (engine == nullptr) {
        std::fprintf(stderr, "quad: no engine\n");
        return 1;
    }
    auto* swapChain = engine->createSwapChain(
        W, H, filament::SwapChain::CONFIG_READABLE | filament::SwapChain::CONFIG_HAS_STENCIL_BUFFER);
    auto* renderer = engine->createRenderer();
    // One scene for all four, which is what Fluorite does. The layers are what
    // keep them apart.
    auto* scene = engine->createScene();
    renderer->setClearOptions({.clearColor = {0.0f, 0.0f, 0.0f, 1.0f}, .clear = true});

    struct Pane {
        filament::View* view = nullptr;
        filament::Camera* camera = nullptr;
        utils::Entity cameraEntity;
        std::unique_ptr<tsf::MapView> map;
    };
    std::array<Pane, 4> panes;

    for (std::size_t i = 0; i < panes.size(); i++) {
        Pane& pane = panes[i];
        const std::uint8_t layer = static_cast<std::uint8_t>(1u << i);

        pane.cameraEntity = utils::EntityManager::get().create();
        pane.camera = engine->createCamera(pane.cameraEntity);
        tsf::MapView::configureCamera(*pane.camera);

        pane.view = engine->createView();
        pane.view->setScene(scene);
        pane.view->setCamera(pane.camera);
        // The map is drawn y-down, so the writer flips the framebuffer to get it
        // upright -- and that flip swaps the pane rows too. Lay the rows out
        // pre-flip: the first two cities go low, and read back on top.
        const std::uint32_t left = (i % 2) * paneW;
        const std::uint32_t bottom = (i < 2) ? 0 : paneH;
        pane.view->setViewport({(std::int32_t)left, (std::int32_t)bottom, paneW, paneH});
        // Only this map's renderables. Without it every pane draws all four.
        pane.view->setVisibleLayers(0xFF, layer);
        pane.view->setPostProcessingEnabled(false);
        pane.view->setStencilBufferEnabled(true);

        std::string error;
        pane.map = tsf::MapView::create(engine, scene, materialDir, style, paneW, paneH,
                                        kCities[i].latitude, kCities[i].longitude, kCities[i].zoom,
                                        &error, layer);
        if (!pane.map) {
            std::fprintf(stderr, "quad: %s: %s\n", kCities[i].name, error.c_str());
            return 1;
        }
    }

    // Every map to quiescence. They share one worker pool, so ticking them in
    // turn is what lets four covers load at once rather than one after another.
    const int budgetMs = std::getenv("TSF_QUAD_BUDGET_MS")
                             ? std::atoi(std::getenv("TSF_QUAD_BUDGET_MS"))
                             : 120000;
    const int quietTicks =
        std::getenv("TSF_QUAD_QUIET") ? std::atoi(std::getenv("TSF_QUAD_QUIET")) : 200;
    int quiet = 0;
    std::array<std::uint64_t, 4> held{};
    for (int waited = 0; waited < budgetMs && quiet < quietTicks; waited += 5) {
        bool still = false;
        for (std::size_t i = 0; i < panes.size(); i++) {
            panes[i].map->tick();
            if (panes[i].map->records() != held[i] || panes[i].map->pending() != 0) {
                held[i] = panes[i].map->records();
                still = true;
            }
        }
        quiet = still ? 0 : quiet + 1;
        pause_ms(5);
    }
    std::printf("quiescent %d\n", quiet >= quietTicks ? 1 : 0);
    for (std::size_t i = 0; i < panes.size(); i++) {
        std::printf("%s primitives %llu\n", kCities[i].name,
                    (unsigned long long)panes[i].map->renderer().primitives());
        // And why, when a pane drew almost nothing. A source whose origin has gone -- a dated
        // pmtiles build is kept about a week -- leaves every pane at two primitives and says
        // nothing, which reads as a regression in whatever was being tested and cost a bisect to
        // tell apart from one. The readiness carries the count and the first reason.
        std::string reason;
        const auto ready = panes[i].map->readiness(&reason);
        if (!reason.empty()) {
            std::printf("%s readiness %d: %s\n", kCities[i].name, (int)ready, reason.c_str());
        }
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(W) * H * 4);
    const auto capture = [&](std::vector<std::uint8_t>& into) {
        filament::backend::PixelBufferDescriptor pb(into.data(), into.size(),
                                                    filament::backend::PixelDataFormat::RGBA,
                                                    filament::backend::PixelDataType::UBYTE);
        if (renderer->beginFrame(swapChain)) {
            for (Pane& pane : panes) renderer->render(pane.view);
            renderer->readPixels(0, 0, W, H, std::move(pb));
            renderer->endFrame();
        }
        engine->flushAndWait();
    };

    // Settle on the image, as render_probe does and for the same reason.
    std::vector<std::uint8_t> previous(pixels.size(), 0);
    capture(pixels);
    int rounds = 0;
    for (; rounds < 60; rounds++) {
        previous.swap(pixels);
        for (int t = 0; t < 20; t++) {
            for (Pane& pane : panes) pane.map->tick();
            pause_ms(5);
        }
        capture(pixels);
        if (pixels == previous) break;
    }
    std::printf("image_stable %d\n", rounds < 60 ? 1 : 0);

    if (std::FILE* ppm = std::fopen(out, "wb")) {
        std::fprintf(ppm, "P6\n%u %u\n255\n", W, H);
        for (std::uint32_t y = 0; y < H; y++) {
            const std::uint8_t* row = pixels.data() + (std::size_t)(H - 1 - y) * W * 4;
            for (std::uint32_t x = 0; x < W; x++) std::fwrite(row + (std::size_t)x * 4, 1, 3, ppm);
        }
        std::fclose(ppm);
        std::printf("wrote 1\n");
    }

    for (Pane& pane : panes) {
        pane.map.reset();
        engine->destroy(pane.view);
        engine->destroyCameraComponent(pane.cameraEntity);
        utils::EntityManager::get().destroy(pane.cameraEntity);
    }
    engine->destroy(scene);
    engine->destroy(renderer);
    engine->destroy(swapChain);
    filament::Engine::destroy(&engine);
    return 0;
}
