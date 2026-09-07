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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
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

/// Degrees of pitch, matching the quad app.
constexpr double kPitch = 15.0;

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
    // TSF_EXT_ZOOM overrides every pane's home zoom, so a camera the sweep passes through can be
    // rendered settled. A sweep frame cannot be compared against the oracle on its own: which
    // tiles have landed at frame N is a function of how fast the build ran, so two builds put
    // different zooms of the same ground on the screen. Settled, the cover is the cover.
    const char* zoomOverride = std::getenv("TSF_EXT_ZOOM");
    for (std::uint32_t slot = 0; slot < kCities.size(); slot++) {
        const double home = zoomOverride ? std::atof(zoomOverride) : kCities[slot].zoom;
        // Pitched, because the app is: the map-aligned label arrangement is only reached with a
        // pitch, and it is where the last defects were.
        tessella_fluorite_set_camera(slot, kCities[slot].latitude, kCities[slot].longitude, home,
                                     0.0, kPitch);
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

    // The sweep the app runs, through the path the app runs it on. Settled frames are clean on
    // every probe here and on mbgl; what the quad shows while its camera moves is not, and the
    // rung missing between the two was a moving camera on *this* side of the extension.
    //
    // TSF_EXT_SWEEP is how many frames to sweep, TSF_EXT_DUMP_AT which of them to write.
    if (const char* sweeping = std::getenv("TSF_EXT_SWEEP")) {
        const int frames = std::max(1, std::atoi(sweeping));
        // A comma-separated list, because the defects worth looking at are motion-only: one
        // frame cannot show a label that is not anchored, and a second run of the same sweep is
        // a second trajectory's worth of tile arrivals rather than the next frame of this one.
        std::set<int> dumpAt;
        if (const char* at = std::getenv("TSF_EXT_DUMP_AT")) {
            for (const char* p = at; *p != '\0';) {
                dumpAt.insert(std::atoi(p));
                while (*p != '\0' && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
        const auto ease = [](const double t) {
            const double clamped = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
            return 0.5 - 0.5 * std::cos(clamped * M_PI);
        };
        std::vector<std::uint8_t> shot(static_cast<std::size_t>(W) * H * 4);
        const bool traceScissor = std::getenv("TSF_SCISSOR_TRACE") != nullptr;
        // TSF_EXT_HOLD freezes the camera for the last N frames of the sweep. A settled probe
        // starts cold and waits for the picture to stop changing, which cannot see instability
        // that motion *leaves behind*: the fades mid-run, the cross-tile index mid-churn, a cache
        // holding what the last camera wanted. Holding after a sweep can, because the camera is
        // identical across the held frames and anything that still differs is ours.
        const int hold = std::getenv("TSF_EXT_HOLD") ? std::atoi(std::getenv("TSF_EXT_HOLD")) : 0;
        const int moving = std::max(1, frames - std::max(0, hold));
        for (int frame = 0; frame < frames; frame++) {
            const double t = static_cast<double>(std::min(frame, moving)) / frames;
            if (traceScissor) {
                // The renderer's own trace is per drawable and says nothing about when; this is
                // what separates one frame's boxes from the next's.
                std::fprintf(stderr, "=== frame %d\n", frame);
            }
            // The zoom the first pane reached, for the dump line: every pane sweeps the same
            // curve from its own home, so one of them names where in the sweep this frame is.
            double leading = 0.0;
            for (std::uint32_t slot = 0; slot < kCities.size(); slot++) {
                const double home = kCities[slot].zoom;
                double zoom = home;
                if (t < 0.33) {
                    zoom = home + (0.0 - home) * ease(t / 0.33);
                } else if (t < 0.80) {
                    zoom = 18.0 * ease((t - 0.33) / 0.47);
                } else {
                    zoom = 18.0 + (home - 18.0) * ease((t - 0.80) / 0.20);
                }
                tessella_fluorite_set_camera(slot, kCities[slot].latitude, kCities[slot].longitude,
                                             zoom, 0.0, kPitch);
                if (slot == 0) {
                    leading = zoom;
                }
            }
            if (dumpAt.count(frame) != 0) {
                filament::backend::PixelBufferDescriptor pb(
                    shot.data(), shot.size(), filament::backend::PixelDataFormat::RGBA,
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
                char named[4096];
                if (dumpAt.size() > 1) {
                    std::snprintf(named, sizeof named, "%s.%03d", out, frame);
                } else {
                    std::snprintf(named, sizeof named, "%s", out);
                }
                if (std::FILE* ppm = std::fopen(named, "wb")) {
                    std::fprintf(ppm, "P6\n%u %u\n255\n", W, H);
                    for (std::uint32_t y = 0; y < H; y++) {
                        const std::uint8_t* row = shot.data() + (std::size_t)(H - 1 - y) * W * 4;
                        for (std::uint32_t x = 0; x < W; x++) {
                            std::fwrite(row + (std::size_t)x * 4, 1, 3, ppm);
                        }
                    }
                    std::fclose(ppm);
                    std::printf("swept_dump frame %d %s zoom %.2f -> %s\n", frame,
                                kCities[0].name, leading, named);
                }
            } else {
                renderFrame();
            }
            pause_ms(5);
        }
        for (std::size_t i = 0; i < panes.size(); i++) {
            g_extension.detach(g_extension.user, static_cast<std::uint32_t>(i));
        }
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
        if (std::getenv("TESSELLA_WATCH") != nullptr) {
            std::printf("capture\n");
            std::fflush(stdout);
        }
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

    // And a few more after it, because "two captures matched" is a weaker stopping rule than it
    // looks. A label fading in moves by one increment per emitted frame, and the map emits only
    // while a fade is in flight -- so a round that happens to fall between emits sees two equal
    // pictures and stops with the fade half done. Cheap insurance: keep capturing until the
    // picture has held still for several rounds rather than one.
    for (int settled = 0, extra = 0; settled < 4 && extra < 40; extra++) {
        previous.swap(pixels);
        for (int t = 0; t < 20; t++) {
            renderFrame();
            pause_ms(5);
        }
        capture(pixels);
        settled = (pixels == previous) ? settled + 1 : 0;
    }

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
