// SPDX-License-Identifier: Apache-2.0
//
// The quad through the extension, which is the code Fluorite calls.
//
//   extension_probe <style.json> <materialDir> <out.ppm> [paneW paneH]
//
// quad_probe drives `tsf::MapView` directly. This drives `tessella_fluorite_*`
// and the `FluoriteViewExtension` it installs, so what is tested is the shipped
// path: configure, install, four cameras set before any view exists, attach per
// slot with that slot's engine, scene and size, frame on the render loop,
// detach when the views go.
//
// It stands in for fluorite rather than linking it: `fluorite_set_view_extension`
// is defined here, so the probe receives the struct the extension registers and
// calls it the way `FilamentProducer` does. That is the whole of fluorite's side
// of the seam, which is what makes standing in for it honest.

#include <tsf/extension.h>

#include <fluorite/view_extension.h>

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
#include <string>
#include <vector>

namespace {

/// What the extension registered. Fluorite copies it under its own lock; one
/// thread here, so a plain global is the same thing.
FluoriteViewExtension g_extension{};

struct City {
    const char* name;
    double latitude;
    double longitude;
    double zoom;
};

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

/// Fluorite's half of the seam, which this probe is standing in for.
extern "C" void fluorite_set_view_extension(const FluoriteViewExtension* extension) {
    g_extension = extension != nullptr ? *extension : FluoriteViewExtension{};
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <style.json> <materialDir> <out.ppm> [paneW paneH]\n",
                     argv[0]);
        return 2;
    }
    const std::string style = slurp(argv[1]);
    if (style.empty()) {
        std::fprintf(stderr, "extension: cannot read %s\n", argv[1]);
        return 2;
    }
    const char* out = argv[3];
    const long paneWArg = argc > 4 ? std::strtol(argv[4], nullptr, 10) : 640;
    const long paneHArg = argc > 5 ? std::strtol(argv[5], nullptr, 10) : 480;
    if (paneWArg < 1 || paneWArg > 8192 || paneHArg < 1 || paneHArg > 8192) {
        std::fprintf(stderr, "extension: pane size out of range (1..8192)\n");
        return 2;
    }
    const auto paneW = static_cast<std::uint32_t>(paneWArg);
    const auto paneH = static_cast<std::uint32_t>(paneHArg);
    const std::uint32_t W = paneW * 2;
    const std::uint32_t H = paneH * 2;

    // What the Dart half does, in order and before any view exists.
    if (tessella_fluorite_configure(style.c_str(), argv[2]) != 0) {
        std::fprintf(stderr, "extension: configure refused\n");
        return 1;
    }
    if (tessella_fluorite_install() != 0) {
        std::fprintf(stderr, "extension: install refused\n");
        return 1;
    }
    if (g_extension.attach == nullptr || g_extension.frame == nullptr ||
        g_extension.detach == nullptr) {
        std::fprintf(stderr, "extension: install registered nothing\n");
        return 1;
    }
    std::printf("installed 1\n");

    // Cameras before views, which is the point of holding them: a slot with no
    // camera gets no map, and a slot with one comes up already pointed.
    for (std::uint32_t slot = 0; slot < kCities.size(); slot++) {
        tessella_fluorite_set_camera(slot, kCities[slot].latitude, kCities[slot].longitude,
                                     kCities[slot].zoom, 0.0, 0.0);
        if (tessella_fluorite_attached(slot) != 0) {
            std::fprintf(stderr, "extension: slot %u attached before its view\n", slot);
            return 1;
        }
    }

    auto* engine = filament::Engine::Builder().backend(filament::Engine::Backend::VULKAN).build();
    if (engine == nullptr) {
        std::fprintf(stderr, "extension: no engine\n");
        return 1;
    }
    auto* swapChain = engine->createSwapChain(
        W, H, filament::SwapChain::CONFIG_READABLE | filament::SwapChain::CONFIG_HAS_STENCIL_BUFFER);
    auto* renderer = engine->createRenderer();
    auto* scene = engine->createScene();
    renderer->setClearOptions({.clearColor = {0.0f, 0.0f, 0.0f, 1.0f}, .clear = true});

    struct Pane {
        filament::View* view = nullptr;
        filament::Camera* camera = nullptr;
        utils::Entity cameraEntity;
    };
    std::array<Pane, 4> panes;

    for (std::size_t i = 0; i < panes.size(); i++) {
        Pane& pane = panes[i];
        pane.cameraEntity = utils::EntityManager::get().create();
        pane.camera = engine->createCamera(pane.cameraEntity);
        tsf::MapView::configureCamera(*pane.camera);

        pane.view = engine->createView();
        pane.view->setScene(scene);
        pane.view->setCamera(pane.camera);
        pane.view->setViewport({static_cast<std::int32_t>((i % 2) * paneW),
                                static_cast<std::int32_t>(i < 2 ? 0 : paneH), paneW, paneH});
        pane.view->setPostProcessingEnabled(false);
        pane.view->setStencilBufferEnabled(true);
        // Fluorite's `CreateView` does exactly this before calling attach: layer 0
        // stays the ECS content, and slot i gets bit i + 1.
        const auto layer = static_cast<std::uint8_t>(1u << (i + 1));
        pane.view->setVisibleLayers(0xFF, FLUORITE_VIEW_EXTENSION_ECS_LAYER | layer);
        g_extension.attach(g_extension.user, static_cast<std::uint32_t>(i), engine, scene,
                           pane.view, paneW, paneH);
        if (tessella_fluorite_attached(static_cast<std::uint32_t>(i)) != 1) {
            std::fprintf(stderr, "extension: slot %zu did not attach\n", i);
            return 1;
        }
    }
    std::printf("attached 4\n");

    const int budgetMs = std::getenv("TSF_EXT_BUDGET_MS")
                             ? std::atoi(std::getenv("TSF_EXT_BUDGET_MS"))
                             : 120000;
    const auto renderFrame = [&]() {
        // The order fluorite uses: the extension's per-frame work, then the draw.
        for (std::size_t i = 0; i < panes.size(); i++) {
            g_extension.frame(g_extension.user, static_cast<std::uint32_t>(i), 1.0 / 60.0);
        }
        if (renderer->beginFrame(swapChain)) {
            for (Pane& pane : panes) renderer->render(pane.view);
            renderer->endFrame();
        }
        engine->flushAndWait();
    };

    int quiet = 0;
    std::array<std::uint64_t, 4> held{};
    for (int waited = 0; waited < budgetMs && quiet < 200; waited += 5) {
        bool moving = false;
        for (std::uint32_t slot = 0; slot < panes.size(); slot++) {
            const std::uint64_t pending = tessella_fluorite_pending(slot);
            if (pending != held[slot] || pending != 0) {
                held[slot] = pending;
                moving = true;
            }
        }
        renderFrame();
        quiet = moving ? 0 : quiet + 1;
        pause_ms(5);
    }
    std::printf("quiescent %d\n", quiet >= 200 ? 1 : 0);
    for (std::uint32_t slot = 0; slot < panes.size(); slot++) {
        char reason[256] = {0};
        const int readiness = tessella_fluorite_readiness(slot, reason, sizeof reason);
        std::printf("%s readiness %d %s\n", kCities[slot].name, readiness, reason);
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(W) * H * 4);
    std::vector<std::uint8_t> previous(pixels.size(), 0);
    const auto capture = [&](std::vector<std::uint8_t>& into) {
        filament::backend::PixelBufferDescriptor pb(into.data(), into.size(),
                                                    filament::backend::PixelDataFormat::RGBA,
                                                    filament::backend::PixelDataType::UBYTE);
        for (std::size_t i = 0; i < panes.size(); i++) {
            g_extension.frame(g_extension.user, static_cast<std::uint32_t>(i), 1.0 / 60.0);
        }
        if (renderer->beginFrame(swapChain)) {
            for (Pane& pane : panes) renderer->render(pane.view);
            renderer->readPixels(0, 0, W, H, std::move(pb));
            renderer->endFrame();
        }
        engine->flushAndWait();
    };
    capture(pixels);
    int rounds = 0;
    for (; rounds < 60; rounds++) {
        previous.swap(pixels);
        for (int t = 0; t < 20; t++) {
            renderFrame();
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

    // And the teardown, which fluorite drives from DestroyView.
    for (std::size_t i = 0; i < panes.size(); i++) {
        g_extension.detach(g_extension.user, static_cast<std::uint32_t>(i));
        if (tessella_fluorite_attached(static_cast<std::uint32_t>(i)) != 0) {
            std::fprintf(stderr, "extension: slot %zu still attached after detach\n", i);
            return 1;
        }
    }
    std::printf("detached 4\n");
    tessella_fluorite_uninstall();

    for (Pane& pane : panes) {
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
