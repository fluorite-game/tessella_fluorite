// SPDX-License-Identifier: Apache-2.0
//
// What the quad costs: four maps, one engine, one scene, four views.
//
//   quad_bench <style.json> <materialDir> [paneW paneH]
//
// Three phases, because they measure different things and only one of them is
// the number people mean by "frame time":
//
//   cold    create to all four quiescent. Tile fetch, parse, layout, upload.
//           Dominated by the network when the source is remote, so it is
//           reported but not compared.
//   still   frames with nothing moving. A settled map should send nothing, so
//           this is the floor -- the cost of having four maps rather than
//           drawing them. If it is not near zero, something re-emits per frame.
//   solo    one camera moving, three still. The cost of one map in motion.
//   motion  every camera moving every frame. Against solo this says whether
//           four maps cost four times one, or more -- they share a worker pool
//           and a tile cache, and contention would show here and nowhere else.
//   sweep   the whole zoom range and back: out to 0, in to 18, home. The
//           hardest thing the pipeline is asked to do -- a pan moves the camera,
//           this replaces everything it is looking at, twenty times over.
//
// Motion is a realistic rate, not a stress test: the pan covers a screen width
// in about a second at 60fps, the zoom moves 0.3 levels a second and the bearing
// 8 degrees a second. A benchmark that teleports the camera every frame measures
// cold loading over and over.
//
// Timing is split into tick (drain the producer's records into the scene) and
// frame (submit and wait). The wait is deliberate: without it the loop measures
// how fast frames can be *recorded*, and a GPU-bound quad would look free.
//
// TSF_BENCH_ONLY=<phase> runs the cold load and then just that phase, for
// attaching a profiler to one of them without the others in the sample.
//
// Env: TSF_BENCH_FRAMES (default 600), TSF_BENCH_WARMUP (60),
//      TSF_BENCH_COLD_MS (default 120000), TSF_BENCH_COLD_QUIET (default 200),
//      TSF_BENCH_CSV (per-frame samples).

#include <tsf/filament_renderer.h>
#include <tsf/map_view.h>

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>

#include <ctime>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(const Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void pause_ms(const long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&ts, nullptr);
}

struct City {
    const char* name;
    double latitude;
    double longitude;
    double zoom;
};

// The same four the quad draws, so the benchmark and the picture are the same
// scene.
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

int env_int(const char* name, const int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) return fallback;
    const long parsed = std::strtol(value, nullptr, 10);
    return parsed > 0 ? static_cast<int>(parsed) : fallback;
}

/// Peak resident set, in mebibytes. VmHWM rather than VmRSS: the question is
/// what the process needed, not what it happens to hold at the end.
double peak_rss_mib() {
    std::FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) return 0.0;
    char line[256];
    double kib = 0.0;
    while (std::fgets(line, sizeof line, status) != nullptr) {
        if (std::strncmp(line, "VmHWM:", 6) == 0) {
            kib = std::strtod(line + 6, nullptr);
            break;
        }
    }
    std::fclose(status);
    return kib / 1024.0;
}

/// One phase's samples, summarised. Percentiles rather than a mean alone: a
/// quad that is fast on average and stalls every thirtieth frame is a quad that
/// drops frames, and the mean hides exactly that.
struct Summary {
    double mean = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

Summary summarise(std::vector<double> samples) {
    Summary out;
    if (samples.empty()) return out;
    std::sort(samples.begin(), samples.end());
    double total = 0.0;
    for (const double value : samples) total += value;
    const auto at = [&](const double quantile) {
        const std::size_t index = static_cast<std::size_t>(quantile * (samples.size() - 1) + 0.5);
        return samples[index];
    };
    out.mean = total / static_cast<double>(samples.size());
    out.p50 = at(0.50);
    out.p95 = at(0.95);
    out.p99 = at(0.99);
    out.max = samples.back();
    return out;
}

void report(const char* phase, const char* what, const Summary& summary) {
    // A phase TSF_BENCH_ONLY skipped has no samples, and printing zeros for it
    // reads as a phase that cost nothing rather than one that did not run.
    if (summary.max == 0.0 && summary.mean == 0.0) {
        return;
    }
    std::printf("  %-6s %-6s mean %7.3f  p50 %7.3f  p95 %7.3f  p99 %7.3f  max %7.3f\n", phase, what,
                summary.mean, summary.p50, summary.p95, summary.p99, summary.max);
    std::printf("bench %s.%s.mean=%.4f %s.%s.p50=%.4f %s.%s.p95=%.4f %s.%s.p99=%.4f %s.%s.max=%.4f\n",
                phase, what, summary.mean, phase, what, summary.p50, phase, what, summary.p95,
                phase, what, summary.p99, phase, what, summary.max);
}

struct Pane {
    filament::View* view = nullptr;
    filament::Camera* camera = nullptr;
    utils::Entity cameraEntity;
    std::unique_ptr<tsf::MapView> map;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <style.json> <materialDir> [paneW paneH]\n", argv[0]);
        return 2;
    }
    const std::string style = slurp(argv[1]);
    if (style.empty()) {
        std::fprintf(stderr, "bench: cannot read %s\n", argv[1]);
        return 2;
    }
    const std::string materialDir = argv[2];
    const long paneWArg = argc > 3 ? std::strtol(argv[3], nullptr, 10) : 640;
    const long paneHArg = argc > 4 ? std::strtol(argv[4], nullptr, 10) : 480;
    if (paneWArg < 1 || paneWArg > 8192 || paneHArg < 1 || paneHArg > 8192) {
        std::fprintf(stderr, "bench: pane size out of range (1..8192)\n");
        return 2;
    }
    const auto paneW = static_cast<std::uint32_t>(paneWArg);
    const auto paneH = static_cast<std::uint32_t>(paneHArg);
    const std::uint32_t W = paneW * 2;
    const std::uint32_t H = paneH * 2;

    const int frames = env_int("TSF_BENCH_FRAMES", 600);
    const int warmup = env_int("TSF_BENCH_WARMUP", 60);
    const char* onlyPhase = std::getenv("TSF_BENCH_ONLY");
    const auto wanted = [&](const char* phase) {
        return onlyPhase == nullptr || std::strcmp(onlyPhase, phase) == 0;
    };
    const int coldBudgetMs = env_int("TSF_BENCH_COLD_MS", 120000);
    // Quiet iterations before the cold phase is called settled, each with a 5ms
    // pause. Two hundred is a second, which has to outlast a tile round trip:
    // pending() reaching zero only means nothing is outstanding *right now*,
    // and a map that has asked for nothing yet looks exactly like one that has
    // everything. Sixty un-paused iterations called an empty scene settled.
    const int coldQuiet = env_int("TSF_BENCH_COLD_QUIET", 200);

    auto* engine = filament::Engine::Builder().backend(filament::Engine::Backend::VULKAN).build();
    if (engine == nullptr) {
        std::fprintf(stderr, "bench: no engine\n");
        return 1;
    }
    auto* swapChain = engine->createSwapChain(
        W, H, filament::SwapChain::CONFIG_READABLE | filament::SwapChain::CONFIG_HAS_STENCIL_BUFFER);
    auto* renderer = engine->createRenderer();
    auto* scene = engine->createScene();
    renderer->setClearOptions({.clearColor = {0.0f, 0.0f, 0.0f, 1.0f}, .clear = true});

    std::array<Pane, 4> panes;
    for (std::size_t i = 0; i < panes.size(); i++) {
        Pane& pane = panes[i];
        const auto layer = static_cast<std::uint8_t>(1u << i);

        pane.cameraEntity = utils::EntityManager::get().create();
        pane.camera = engine->createCamera(pane.cameraEntity);
        tsf::MapView::configureCamera(*pane.camera);

        pane.view = engine->createView();
        pane.view->setScene(scene);
        pane.view->setCamera(pane.camera);
        pane.view->setViewport({static_cast<std::int32_t>((i % 2) * paneW),
                                static_cast<std::int32_t>(i < 2 ? 0 : paneH), paneW, paneH});
        pane.view->setVisibleLayers(0xFF, layer);
        pane.view->setPostProcessingEnabled(false);
        pane.view->setStencilBufferEnabled(true);

        std::string error;
        pane.map = tsf::MapView::create(engine, scene, materialDir, style, paneW, paneH,
                                        kCities[i].latitude, kCities[i].longitude, kCities[i].zoom,
                                        &error, layer);
        if (!pane.map) {
            std::fprintf(stderr, "bench: %s: %s\n", kCities[i].name, error.c_str());
            return 1;
        }
    }

    std::printf("quad_bench %ux%u per pane, %u x %u total, %d frames (%d warmup)\n", paneW, paneH,
                W, H, frames, warmup);

    // One frame, timed the way every frame below is timed. The wait is what
    // makes the number a frame time rather than a submission time.
    const auto renderFrame = [&]() -> double {
        const auto start = Clock::now();
        if (renderer->beginFrame(swapChain)) {
            for (Pane& pane : panes) renderer->render(pane.view);
            renderer->endFrame();
        }
        engine->flushAndWait();
        return ms_since(start);
    };
    std::array<std::vector<double>, 4> perPane;
    std::vector<double> produce;
    std::vector<double> drain;
    bool recordPerPane = false;
    const auto tickAll = [&]() -> double {
        const auto start = Clock::now();
        double producedNs = 0.0;
        double drainedNs = 0.0;
        for (std::size_t i = 0; i < panes.size(); i++) {
            const auto one = Clock::now();
            panes[i].map->tick();
            producedNs += static_cast<double>(panes[i].map->produceNs());
            drainedNs += static_cast<double>(panes[i].map->drainNs());
            if (recordPerPane) perPane[i].push_back(ms_since(one));
        }
        if (recordPerPane) {
            produce.push_back(producedNs / 1e6);
            drain.push_back(drainedNs / 1e6);
        }
        return ms_since(start);
    };

    // Where each pane's camera is at a point in the loop. Out of phase by a
    // quarter turn so the four are not asking for the same tiles at the same
    // moment, which would measure a burst rather than a load.
    const auto moveTo = [&](const std::size_t i, const double turn) {
        const double phase = turn + (M_PI / 2.0) * static_cast<double>(i);
        panes[i].map->setCamera(kCities[i].latitude + 0.02 * std::sin(phase),
                                kCities[i].longitude + 0.02 * std::cos(phase),
                                kCities[i].zoom + 0.5 * std::sin(phase),
                                30.0 * std::sin(phase * 0.5), 0.0);
    };

    // --- cold: create to quiescent -------------------------------------------
    const auto coldStart = Clock::now();
    std::array<std::uint64_t, 4> held{};
    int quiet = 0;
    while (ms_since(coldStart) < coldBudgetMs && quiet < coldQuiet) {
        bool moving = false;
        for (std::size_t i = 0; i < panes.size(); i++) {
            panes[i].map->tick();
            if (panes[i].map->records() != held[i] || panes[i].map->pending() != 0) {
                held[i] = panes[i].map->records();
                moving = true;
            }
        }
        renderFrame();
        quiet = moving ? 0 : quiet + 1;
        pause_ms(5);
    }
    const bool settled = quiet >= coldQuiet;
    // The quiet run is not load time, it is how long the benchmark waited to be
    // sure. Subtract it so the number is what a user would see.
    const double coldMs = ms_since(coldStart) - (settled ? coldQuiet * 5.0 : 0.0);
    std::printf("cold   %.0f ms to quiescent (%s)\n", coldMs, settled ? "settled" : "gave up");
    std::printf("bench cold.ms=%.0f cold.settled=%d\n", coldMs, settled ? 1 : 0);
    for (std::size_t i = 0; i < panes.size(); i++) {
        std::printf("  %-10s records %6llu  primitives %5llu\n", kCities[i].name,
                    (unsigned long long)panes[i].map->records(),
                    (unsigned long long)panes[i].map->renderer().primitives());
        std::printf("bench cold.%s.records=%llu cold.%s.primitives=%llu\n", kCities[i].name,
                    (unsigned long long)panes[i].map->records(), kCities[i].name,
                    (unsigned long long)panes[i].map->renderer().primitives());
    }

    // --- still: nothing moves ------------------------------------------------
    std::vector<double> stillTick;
    std::vector<double> stillFrame;
    stillTick.reserve(frames);
    stillFrame.reserve(frames);
    const std::uint64_t recordsBeforeStill = panes[0].map->records();
    for (int frame = 0; wanted("still") && frame < frames + warmup; frame++) {
        const double tick = tickAll();
        const double render = renderFrame();
        if (frame >= warmup) {
            stillTick.push_back(tick);
            stillFrame.push_back(render);
        }
    }
    if (wanted("still")) std::printf("still  (nothing moving)\n");
    report("still", "tick", summarise(stillTick));
    report("still", "frame", summarise(stillFrame));
    std::printf("bench still.records=%llu\n",
                (unsigned long long)(panes[0].map->records() - recordsBeforeStill));

    // --- solo: one camera moving ----------------------------------------------
    std::vector<double> soloTick;
    std::vector<double> soloFrame;
    soloTick.reserve(frames);
    soloFrame.reserve(frames);
    for (int frame = 0; wanted("solo") && frame < frames + warmup; frame++) {
        moveTo(0, 2.0 * M_PI * frame / (frames + warmup));
        const double tick = tickAll();
        const double render = renderFrame();
        if (frame >= warmup) {
            soloTick.push_back(tick);
            soloFrame.push_back(render);
        }
    }
    if (wanted("solo")) std::printf("solo   (one camera moving, three still)\n");
    report("solo", "tick", summarise(soloTick));
    report("solo", "frame", summarise(soloFrame));

    // --- motion: every camera moving every frame ------------------------------
    //
    // A slow circle and a zoom breathe, per pane and out of phase, so the four
    // are not asking for tiles in lockstep -- which would measure a burst rather
    // than a load. One full circle over the run.
    std::vector<double> motionTick;
    std::vector<double> motionFrame;
    motionTick.reserve(frames);
    motionFrame.reserve(frames);
    const std::uint64_t recordsBeforeMotion = panes[0].map->records();
    recordPerPane = true;
    for (int frame = 0; wanted("motion") && frame < frames + warmup; frame++) {
        const double turn = 2.0 * M_PI * frame / (frames + warmup);
        for (std::size_t i = 0; i < panes.size(); i++) moveTo(i, turn);
        const double tick = tickAll();
        const double render = renderFrame();
        if (frame >= warmup) {
            motionTick.push_back(tick);
            motionFrame.push_back(render);
        }
    }
    if (wanted("motion")) std::printf("motion (all four cameras moving)\n");
    report("motion", "tick", summarise(motionTick));
    report("motion", "frame", summarise(motionFrame));
    const std::uint64_t motionRecords = panes[0].map->records() - recordsBeforeMotion;
    std::printf("  Seattle    %llu records over %d frames (%.1f per frame)\n",
                (unsigned long long)motionRecords, frames + warmup,
                static_cast<double>(motionRecords) / (frames + warmup));
    recordPerPane = false;
    for (std::size_t i = 0; i < panes.size(); i++) {
        const auto [live, slabs] = panes[i].map->slabOccupancy();
        std::printf("  %-10s last result %d  region %.1f MiB, live %.1f MiB in %llu slabs\n",
                    kCities[i].name, static_cast<int>(panes[i].map->lastResult()),
                    static_cast<double>(panes[i].map->slabUsed()) / (1024.0 * 1024.0),
                    static_cast<double>(live) / (1024.0 * 1024.0), (unsigned long long)slabs);
        const auto [ringHeld, ringCapacity] = panes[i].map->ringPeak();
        std::printf("  %-10s ring peak %.2f MiB of %.0f MiB\n", kCities[i].name,
                    static_cast<double>(ringHeld) / (1024.0 * 1024.0),
                    static_cast<double>(ringCapacity) / (1024.0 * 1024.0));
        std::printf("bench motion.%s.ring_peak_mib=%.3f\n", kCities[i].name,
                    static_cast<double>(ringHeld) / (1024.0 * 1024.0));
        std::printf("bench motion.%s.slab_mib=%.2f motion.%s.result=%d\n", kCities[i].name,
                    static_cast<double>(panes[i].map->slabUsed()) / (1024.0 * 1024.0),
                    kCities[i].name, static_cast<int>(panes[i].map->lastResult()));
    }
    report("motion", "prod", summarise(produce));
    report("motion", "drain", summarise(drain));
    for (std::size_t i = 0; i < panes.size(); i++) {
        const Summary pane = summarise(perPane[i]);
        std::printf("  %-10s tick   mean %7.3f  p50 %7.3f  p95 %7.3f  max %7.3f\n",
                    kCities[i].name, pane.mean, pane.p50, pane.p95, pane.max);
        std::printf("bench motion.%s.tick.mean=%.4f motion.%s.tick.p50=%.4f\n", kCities[i].name,
                    pane.mean, kCities[i].name, pane.p50);
    }
    std::printf("bench motion.records=%llu motion.records_per_frame=%.3f\n",
                (unsigned long long)motionRecords,
                static_cast<double>(motionRecords) / (frames + warmup));

    // --- sweep: the whole zoom range and back --------------------------------
    //
    // The same three legs the app runs, eased the same way, so what is measured
    // here is what a user sees when the quad starts. Zoom is interpolated
    // rather than scale, because a level is a doubling and linear-in-zoom is
    // what makes the rate constant.
    std::vector<double> sweepTick;
    std::vector<double> sweepFrame;
    sweepTick.reserve(frames);
    sweepFrame.reserve(frames);
    std::uint64_t sweepFull = 0;
    // A frame the producer emitted with nothing in it. `beginFrame` clears the
    // scene and rebuilds it from the frame's order, so an empty order is a pane
    // that goes black -- which is what a zoom sweep looked like on screen.
    std::array<std::uint64_t, 4> sweepBlank{};
    std::array<std::uint64_t, 4> sweepEmitted{};
    std::array<std::uint64_t, 4> sweepLowest{};
    sweepLowest.fill(~0ull);
    const auto ease = [](const double t) {
        const double clamped = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
        return 0.5 - 0.5 * std::cos(clamped * M_PI);
    };
    for (int frame = 0; wanted("sweep") && frame < frames + warmup; frame++) {
        // Three legs of the run: out for a third, in for a half, home for the
        // rest.
        const double t = static_cast<double>(frame) / (frames + warmup);
        double sweepZoom = 0.0;
        for (std::size_t i = 0; i < panes.size(); i++) {
            const double home = kCities[i].zoom;
            double zoom = home;
            if (t < 0.33) {
                zoom = home + (0.0 - home) * ease(t / 0.33);
            } else if (t < 0.80) {
                zoom = 0.0 + (18.0 - 0.0) * ease((t - 0.33) / 0.47);
            } else {
                zoom = 18.0 + (home - 18.0) * ease((t - 0.80) / 0.20);
            }
            panes[i].map->setCamera(kCities[i].latitude, kCities[i].longitude, zoom, 0.0, 0.0);
            if (i == 0) sweepZoom = zoom;
        }
        const double tick = tickAll();
        const double render = renderFrame();
        // One frame of the sweep, written out. A count of blank frames says how
        // often the screen goes empty; this says what it goes to.
        if (const char* at = std::getenv("TSF_BENCH_SWEEP_DUMP");
            at != nullptr && frame == std::atoi(at)) {
            std::vector<std::uint8_t> shot(static_cast<std::size_t>(W) * H * 4);
            filament::backend::PixelBufferDescriptor pb(
                shot.data(), shot.size(), filament::backend::PixelDataFormat::RGBA,
                filament::backend::PixelDataType::UBYTE);
            if (renderer->beginFrame(swapChain)) {
                for (Pane& pane : panes) renderer->render(pane.view);
                renderer->readPixels(0, 0, W, H, std::move(pb));
                renderer->endFrame();
            }
            engine->flushAndWait();
            if (std::FILE* ppm = std::fopen("sweep_frame.ppm", "wb")) {
                std::fprintf(ppm, "P6\n%u %u\n255\n", W, H);
                for (std::uint32_t y = 0; y < H; y++) {
                    const std::uint8_t* row = shot.data() + (std::size_t)(H - 1 - y) * W * 4;
                    for (std::uint32_t x = 0; x < W; x++) {
                        std::fwrite(row + (std::size_t)x * 4, 1, 3, ppm);
                    }
                }
                std::fclose(ppm);
                std::printf("sweep_dump frame %d\n", frame);
            }
        }
        for (std::size_t i = 0; i < panes.size(); i++) {
            if (panes[i].map->lastResult() == TESSELLA_REGION_FULL) sweepFull++;
            // Only frames the producer actually emitted: one that published
            // nothing leaves the scene alone, and the pane keeps what it had.
            if (panes[i].map->drainNs() == 0) continue;
            const std::uint64_t prims = panes[i].map->renderer().primitives();
            sweepEmitted[i]++;
            if (prims == 0) sweepBlank[i]++;
            if (std::getenv("TSF_BENCH_SWEEP_TRACE") != nullptr && i == 0) {
                std::printf(
                    "trace frame=%d zoom=%.2f prims=%llu entries=%lld batches=%lld drain=%.3f "
                    "prod=%.3f\n",
                    frame, sweepZoom, (unsigned long long)prims,
                    (long long)(panes[i].map->lastOrderEntries() == ~0ull
                                    ? -1
                                    : (long long)panes[i].map->lastOrderEntries()),
                    (long long)(panes[i].map->lastBatches() == ~0ull
                                    ? -1
                                    : (long long)panes[i].map->lastBatches()),
                    static_cast<double>(panes[i].map->drainNs()) / 1.0e6,
                    static_cast<double>(panes[i].map->produceNs()) / 1.0e6);
            }
            if (prims < sweepLowest[i]) sweepLowest[i] = prims;
        }
        if (frame >= warmup) {
            sweepTick.push_back(tick);
            sweepFrame.push_back(render);
        }
    }
    if (wanted("sweep")) std::printf("sweep  (zoom 0 to 18 and home, all four)\n");
    report("sweep", "tick", summarise(sweepTick));
    report("sweep", "frame", summarise(sweepFrame));
    std::printf("  region-full ticks %llu\n", (unsigned long long)sweepFull);
    std::printf("bench sweep.region_full=%llu\n", (unsigned long long)sweepFull);
    for (std::size_t i = 0; i < panes.size(); i++) {
        std::printf(
            "  %-10s emitted %llu, blank %llu, fewest primitives %llu, orders without a camera "
            "%llu\n",
            kCities[i].name, (unsigned long long)sweepEmitted[i],
            (unsigned long long)sweepBlank[i],
            (unsigned long long)(sweepLowest[i] == ~0ull ? 0 : sweepLowest[i]),
            (unsigned long long)panes[i].map->orphanedOrders());
        std::printf("bench sweep.%s.blank=%llu sweep.%s.emitted=%llu\n", kCities[i].name,
                    (unsigned long long)sweepBlank[i], kCities[i].name,
                    (unsigned long long)sweepEmitted[i]);
    }
    for (std::size_t i = 0; i < panes.size(); i++) {
        const auto [live, slabs] = panes[i].map->slabOccupancy();
        std::printf("  %-10s region %.1f MiB, live %.1f MiB in %llu slabs\n", kCities[i].name,
                    static_cast<double>(panes[i].map->slabUsed()) / (1024.0 * 1024.0),
                    static_cast<double>(live) / (1024.0 * 1024.0), (unsigned long long)slabs);
    }

    const Summary motion = summarise(motionFrame);
    std::printf("peak rss %.0f MiB\n", peak_rss_mib());
    std::printf("bench rss.peak_mib=%.0f fps.motion_p50=%.1f fps.motion_p99=%.1f\n", peak_rss_mib(),
                motion.p50 > 0 ? 1000.0 / motion.p50 : 0.0,
                motion.p99 > 0 ? 1000.0 / motion.p99 : 0.0);

    if (const char* csv = std::getenv("TSF_BENCH_CSV")) {
        if (std::FILE* out = std::fopen(csv, "w")) {
            std::fprintf(out, "phase,frame,tick_ms,frame_ms\n");
            for (std::size_t i = 0; i < stillTick.size(); i++) {
                std::fprintf(out, "still,%zu,%.4f,%.4f\n", i, stillTick[i], stillFrame[i]);
            }
            for (std::size_t i = 0; i < soloTick.size(); i++) {
                std::fprintf(out, "solo,%zu,%.4f,%.4f\n", i, soloTick[i], soloFrame[i]);
            }
            for (std::size_t i = 0; i < motionTick.size(); i++) {
                std::fprintf(out, "motion,%zu,%.4f,%.4f\n", i, motionTick[i], motionFrame[i]);
            }
            for (std::size_t i = 0; i < sweepTick.size(); i++) {
                std::fprintf(out, "sweep,%zu,%.4f,%.4f\n", i, sweepTick[i], sweepFrame[i]);
            }
            std::fclose(out);
            std::printf("samples %s\n", csv);
        }
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
