// SPDX-License-Identifier: Apache-2.0
//
// A style in, a PNG out, through tessella's own pipeline.
//
// The whole chain: a map is created and ticked, its ring drained, its order batched, the batches
// turned into Filament renderables, and the result rendered offscreen and written out. No window
// and no compositor -- a headless swap chain and `readPixels` -- which is what makes this runnable
// where the goldens are produced rather than only on a desktop.
//
//   render_probe <style.json> <materialDir> <out.ppm> [lat lon zoom width height pitch bearing]

#include <tsf/filament_renderer.h>
#include <tsf/host.h>
#include <tsf/map_view.h>

#include <filament/Camera.h>
#include <math/mat4.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

/// How long a tick waits, in milliseconds.
///
/// Five by default, which measures the producer as fast as a consumer can drive it. A consumer
/// locked to a frame clock is a different question and gets a different answer: there, what
/// costs the user is the *number* of ticks a map takes to fill in, not what each one costs, and
/// a change that makes ticks cheaper while needing more of them looks like a win at five
/// milliseconds and a loss at sixteen. `TSF_PROBE_TICK_MS` is how that gets measured rather than
/// argued about.
long tick_ms() {
    static const long ms = std::getenv("TSF_PROBE_TICK_MS")
                               ? std::atol(std::getenv("TSF_PROBE_TICK_MS"))
                               : 5;
    return ms;
}

void pause_ms(long ms) {
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&req, nullptr);
}

/// One JSON value, from `at`, as the bytes it occupies.
///
/// Enough of a reader to take the operations apart and no more: what is inside a `setData`
/// document is the producer's to parse, and this hands it over exactly as written. Strings are
/// walked rather than scanned for a closing quote, because a URL with an escaped character in it
/// would otherwise end the value early.
std::string_view json_value(std::string_view text, std::size_t& at) {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) {
        at++;
    }
    const std::size_t start = at;
    if (at >= text.size()) {
        return {};
    }
    if (text[at] == '"') {
        for (at++; at < text.size(); at++) {
            if (text[at] == '\\') {
                at++;
            } else if (text[at] == '"') {
                at++;
                break;
            }
        }
    } else if (text[at] == '{' || text[at] == '[') {
        int depth = 0;
        bool quoted = false;
        for (; at < text.size(); at++) {
            const char c = text[at];
            if (quoted) {
                if (c == '\\') {
                    at++;
                } else if (c == '"') {
                    quoted = false;
                }
                continue;
            }
            if (c == '"') {
                quoted = true;
            } else if (c == '{' || c == '[') {
                depth++;
            } else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) {
                    at++;
                    break;
                }
            }
        }
    } else {
        while (at < text.size() && text[at] != ',' && text[at] != ']' && text[at] != '}'
               && !std::isspace(static_cast<unsigned char>(text[at]))) {
            at++;
        }
    }
    return text.substr(start, at - start);
}

/// Steps past whatever separates two values.
void json_gap(std::string_view text, std::size_t& at) {
    while (at < text.size()
           && (std::isspace(static_cast<unsigned char>(text[at])) || text[at] == ',')) {
        at++;
    }
}

/// A quoted string's contents, for the two places an operation carries a name.
std::string_view unquoted(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
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

/// The camera a script may move, so each operation changes one property and keeps the rest.
struct Camera {
    double lat;
    double lon;
    double zoom;
    double bearing;
    double pitch;
};

/// Replays `--script`'s operations, which are the render tests' own.
///
/// The same file the oracle reads, byte for byte, because a harness that lowered it into
/// something else would be free to lower it differently for each renderer -- and two renderers
/// handed different instructions is the one failure a parity number cannot show.
///
/// `["setData", id, <document>|<url>]` and the four camera properties. A URL is refused here
/// rather than fetched: the probe has no file source of its own, and a fixture that wants one is
/// a fixture whose document belongs in the script.
bool apply_script(tsf::MapView& map, Camera& camera, const char* path) {
    const std::string text = slurp(path);
    if (text.empty()) {
        std::fprintf(stderr, "probe: cannot read %s\n", path);
        return false;
    }
    std::string_view rest{text};
    std::size_t at = 0;
    const std::string_view all = json_value(rest, at);
    if (all.size() < 2 || all.front() != '[') {
        std::fprintf(stderr, "probe: %s is not an array of operations\n", path);
        return false;
    }
    std::string_view inner = all.substr(1, all.size() - 2);
    std::size_t step = 0;
    for (json_gap(inner, step); step < inner.size(); json_gap(inner, step)) {
        const std::string_view operation = json_value(inner, step);
        if (operation.size() < 2 || operation.front() != '[') {
            std::fprintf(stderr, "probe: an operation is an array with its name first\n");
            return false;
        }
        std::string_view body = operation.substr(1, operation.size() - 2);
        std::size_t field = 0;
        json_gap(body, field);
        const std::string_view name = unquoted(json_value(body, field));
        const auto next = [&]() {
            json_gap(body, field);
            return json_value(body, field);
        };
        if (name == "setData") {
            const std::string source{unquoted(next())};
            const std::string_view document = next();
            if (document.empty() || document.front() == '"') {
                std::fprintf(stderr, "probe: setData by URL is not read here, %s\n", path);
                return false;
            }
            if (!map.setGeojsonData(source, document)) {
                std::fprintf(stderr, "probe: setData %s refused (%d)\n", source.c_str(),
                             (int)map.lastResult());
                return false;
            }
            continue;
        }
        const std::string value{next()};
        if (name == "setCenter") {
            // `[lon, lat]`, which is the render tests' order and GeoJSON's.
            std::size_t pair = 0;
            std::string_view center{value};
            const std::string_view whole = json_value(center, pair);
            std::string_view coordinates = whole.substr(1, whole.size() - 2);
            std::size_t part = 0;
            json_gap(coordinates, part);
            camera.lon = std::atof(std::string{json_value(coordinates, part)}.c_str());
            json_gap(coordinates, part);
            camera.lat = std::atof(std::string{json_value(coordinates, part)}.c_str());
        } else if (name == "setZoom") {
            camera.zoom = std::atof(value.c_str());
        } else if (name == "setBearing") {
            camera.bearing = std::atof(value.c_str());
        } else if (name == "setPitch") {
            camera.pitch = std::atof(value.c_str());
        } else {
            // Named rather than ignored, as the oracle names it: a frame missing whatever the
            // operation asked for would still be measured.
            std::fprintf(stderr, "probe: this probe does not do %.*s yet\n", (int)name.size(),
                         name.data());
            return false;
        }
        if (!map.setCamera(camera.lat, camera.lon, camera.zoom, camera.bearing, camera.pitch)) {
            std::fprintf(stderr, "probe: camera refused (%d)\n", (int)map.lastResult());
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <style.json> <materialDir> <out.ppm> "
                     "[lat lon zoom w h pitch bearing]\n",
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
    // Degrees, both, and both default to the flat north-up camera every earlier measurement used.
    // `mbgl-render` spells them `--pitch` and `--bearing` and takes degrees too, so a scene can be
    // asked of the oracle and of this with the same two numbers.
    const double pitch = argc > 9 ? std::atof(argv[9]) : 0.0;
    const double bearing = argc > 10 ? std::atof(argv[10]) : 0.0;

    // Annotations are not in the style and cannot be: there is no `"type": "annotation"` and no
    // stylesheet can produce one. The oracle takes them on the command line as `--annotations` and
    // `--annotation-image id=file.png`; these are the same two, named in the environment because
    // this probe's arguments are positional and a camera is what they are for.
    //
    //   TSF_ANNOTATIONS        a GeoJSON feature collection, by path
    //   TSF_ANNOTATION_IMAGES  `id=file.png`, comma separated
    const char* annotations = std::getenv("TSF_ANNOTATIONS");
    const char* annotationImages = std::getenv("TSF_ANNOTATION_IMAGES");

    // What a consumer does between the style loading and the frame settling, which no stylesheet
    // can say: the oracle takes the same file as `--script`, in the render tests' own vocabulary.
    //
    //   TSF_SCRIPT  operations to replay once the map has settled, by path
    const char* script = std::getenv("TSF_SCRIPT");
    if (script != nullptr && *script == '\0') {
        // An empty variable is still a variable. A harness that exports one unconditionally means
        // "no script" by it, and reading it as a path would fail every run that has none.
        script = nullptr;
    }

    auto* engine = filament::Engine::Builder()
                       .backend(filament::Engine::Backend::VULKAN)
                       .build();
    if (engine == nullptr) {
        std::fprintf(stderr, "probe: no engine\n");
        return 1;
    }
    auto* swapChain = engine->createSwapChain(
        W, H,
        filament::SwapChain::CONFIG_READABLE | filament::SwapChain::CONFIG_HAS_STENCIL_BUFFER);
    auto* renderer = engine->createRenderer();
    auto* scene = engine->createScene();
    auto* view = engine->createView();
    auto cameraEntity = utils::EntityManager::get().create();
    auto* camera = engine->createCamera(cameraEntity);
    view->setScene(scene);
    view->setCamera(camera);
    view->setViewport({0, 0, W, H});
    // The camera the capture stream expects: identity but for Filament's Y convention. See
    // `FilamentRenderer::configureCamera`, which owns the reason.
    tsf::FilamentRenderer::configureCamera(*camera);
    // No post-processing. Filament tone maps for photographic rendering by default -- ACES, plus
    // bloom and dithering -- and a map is not a photograph: the style already says exactly what
    // colour each thing is, so anything applied on top of that is a deviation from the oracle by
    // construction. It is what left the first correct frame looking bleached.
    // Off, because a map is display-referred sRGB and Filament's post-processing
    // tone-maps what a shader wrote as though it were scene-referred light.
    // TSF_POSTPROCESS turns it back on, which is how the washed-out platform
    // view was reproduced headlessly: roads vanish, water goes grey.
    view->setPostProcessingEnabled(std::getenv("TSF_POSTPROCESS") != nullptr);
    // The clip masks need somewhere to go.
    view->setStencilBufferEnabled(true);
    renderer->setClearOptions({.clearColor = {0.0f, 0.0f, 0.0f, 1.0f}, .clear = true});

    // Owned rather than stacked, so it can be released *before* the engine. A stack object here
    // is destroyed at the end of main, which is after `Engine::destroy` has already freed every
    // buffer it created -- and the second destroy is a precondition panic that names a vertex
    // buffer rather than the ordering that caused it.
    // The same five lines a platform view needs, so they are written once. See
    // `tsf::MapView`; the probe owning the engine is the only difference.
    std::string error;
    std::unique_ptr<tsf::MapView> map =
        tsf::MapView::create(engine, scene, materialDir, style, W, H, lat, lon, zoom, &error);
    if (!map) {
        std::fprintf(stderr, "probe: %s\n", error.c_str());
        return 1;
    }

    // Before the first tick, which is what starts source resolution -- and resolution is where
    // the layers an annotation draws through are synthesized into the style. Images first: a
    // symbol names one by id, and an id with no image draws nothing.
    if (annotationImages != nullptr) {
        std::string_view rest(annotationImages);
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            const std::string_view spec = rest.substr(0, comma);
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
            const auto equals = spec.find('=');
            if (equals == std::string_view::npos) {
                std::fprintf(stderr, "probe: annotation image wants id=path, got %.*s\n",
                             (int)spec.size(), spec.data());
                return 2;
            }
            const std::string path(spec.substr(equals + 1));
            const std::string bytes = slurp(path.c_str());
            if (bytes.empty()) {
                std::fprintf(stderr, "probe: cannot read %s\n", path.c_str());
                return 2;
            }
            if (!map->addAnnotationImage(spec.substr(0, equals), bytes)) {
                std::fprintf(stderr, "probe: annotation image %s refused (%d)\n", path.c_str(),
                             (int)map->lastResult());
                return 1;
            }
        }
    }
    if (annotations != nullptr) {
        const std::string document = slurp(annotations);
        if (document.empty()) {
            std::fprintf(stderr, "probe: cannot read %s\n", annotations);
            return 2;
        }
        if (!map->setAnnotations(document)) {
            std::fprintf(stderr, "probe: annotations refused (%d)\n", (int)map->lastResult());
            return 1;
        }
    }

    // `create` places the camera flat and north-up; anything else is a second call. Only made
    // when it would change something, so the flat path stays exactly the sequence of calls every
    // measurement so far was taken through.
    if (pitch != 0.0 || bearing != 0.0) {
        if (!map->setCamera(lat, lon, zoom, bearing, pitch)) {
            std::fprintf(stderr, "probe: camera refused pitch %g bearing %g\n", pitch, bearing);
            return 1;
        }
    }

    const int budgetMs = std::getenv("TSF_PROBE_BUDGET_MS")
                             ? std::atoi(std::getenv("TSF_PROBE_BUDGET_MS"))
                             : 60000;
    int waited = 0;
    while (waited < budgetMs) {
        map->tick();
        if (map->renderer().primitives() > 0 && map->readiness() == TESSELLA_READY) {
            break;
        }
        pause_ms(tick_ms());
        waited += tick_ms();
    }
    // Then until the map goes quiet, rather than for a fixed number of ticks.
    //
    // A fixed count captures whatever had arrived when it ran out, and what has arrived decides
    // what is drawn: tiles land in whatever order the network returns them, glyphs fill their
    // atlas as labels ask for them, and symbol placement is a function of both. The same frame
    // measured three times gave 6,502, 8,654 and 9,885 pixels of text -- a spread wide enough to
    // hide any change worth making. Quiescence is the condition that makes it one frame: the
    // producer has stopped emitting records, so there is nothing further to arrive.
    //
    // Bounded, and it says whether it got there. A run that times out is still measurable; it is
    // just not comparable, and saying so beats reporting the number as if it were.
    const int quietTicks = std::getenv("TSF_PROBE_QUIET")
                               ? std::atoi(std::getenv("TSF_PROBE_QUIET"))
                               : 200;
    std::uint64_t held = 0;
    int quiet = 0;
    int settled = 0;
    // Run twice where there is a script: once for the map the style describes, and once for the
    // map after the operations. That is the oracle's order -- it renders, applies, and renders
    // again -- and it is the only order that means anything, because an operation names what the
    // style holds and the style is what the first frame loads.
    const auto settle = [&]() {
    for (held = 0, quiet = 0, settled = 0; settled < 6000 && quiet < quietTicks; settled++) {
        map->tick();
        // Both conditions, and the second is the one that was missing. A silence only means the
        // producer emitted nothing, which a source *blocked* on a fetch satisfies exactly as well
        // as one that has finished -- so the frame could be measured while it was still filling
        // in, and the same scene gave 0, 272 and 9,520 differing pixels across runs that all
        // reported themselves settled. `pending` is what distinguishes the two.
        if (::getenv("TSF_TRACE") && settled % 50 == 0) {
            std::fprintf(stderr, "t=%d records=%llu pending=%llu prims=%llu glyphs=%llu\n",
                         settled, (unsigned long long)map->records(),
                         (unsigned long long)map->pending(),
                         (unsigned long long)map->renderer().primitives(),
                         (unsigned long long)map->renderer().glyphsDrawn());
        }
        if (map->records() == held && map->pending() == 0) {
            quiet++;
        } else {
            held = map->records();
            quiet = 0;
        }
        pause_ms(tick_ms());
    }
    };
    settle();
    if (script != nullptr) {
        Camera camera{lat, lon, zoom, bearing, pitch};
        if (!apply_script(*map, camera, script)) {
            return 1;
        }
        settle();
    }
    std::printf("quiescent %d\n", quiet >= quietTicks ? 1 : 0);
    std::printf("settle_ticks %d\n", settled);
    std::printf("records %llu\n", (unsigned long long)map->records());

    std::string reason;
    std::printf("readiness %d\n", (int)map->readiness(&reason));
    std::printf("renderables %llu\n", (unsigned long long)map->renderer().renderables());
    std::printf("primitives %llu\n", (unsigned long long)map->renderer().primitives());
    std::printf("instances_made %llu\n", (unsigned long long)map->renderer().made());
    std::printf("instances_coloured %llu\n", (unsigned long long)map->renderer().coloured());
    for (const auto& [z, n] : map->renderer().zooms()) {
        std::printf("zoom_%u %llu\n", (unsigned)z, (unsigned long long)n);
    }
    for (const auto& [pass, n] : map->renderer().passes()) {
        std::printf("pass_%u %llu\n", (unsigned)pass, (unsigned long long)n);
    }
    std::printf("redrawn %llu\n", (unsigned long long)map->renderer().redrawn());
    for (const auto& [z, n] : map->renderer().overZooms()) {
        std::printf("overzoom_%u %llu\n", (unsigned)z, (unsigned long long)n);
    }
    std::printf("placements %zu\n", map->renderer().placements());
    std::printf("shared_slots %llu\n", (unsigned long long)map->renderer().sharedSlots());
    std::printf("unmasked %llu\n", (unsigned long long)map->renderer().unmasked());
    for (const auto& [scale, n] : map->renderer().scales()) {
        std::printf("scale %.5f %llu\n", scale, (unsigned long long)n);
    }
    std::printf("masked %llu\n", (unsigned long long)map->renderer().masked());
    std::printf("rebased %llu\n", (unsigned long long)map->renderer().rebased());
    std::printf("anchored %llu of %zu materials\n",
                (unsigned long long)map->renderer().anchoredDrawn(),
                map->renderer().anchoredMaterials());
    std::printf("raised %llu\n", (unsigned long long)map->renderer().raisedDrawn());
    std::printf("glyph_quads_drawn %llu\n", (unsigned long long)map->renderer().glyphsDrawn());
    std::printf("glyph_quads_hidden %llu\n", (unsigned long long)map->renderer().glyphsHidden());
    std::printf("scissored %llu\n", (unsigned long long)map->renderer().scissored());
    std::printf("unplaced %llu\n", (unsigned long long)map->renderer().unplaced());
    std::printf("wall_triangles %llu\n", (unsigned long long)map->renderer().walls());
    std::printf("shared_uploads %llu\n", (unsigned long long)map->renderer().sharedUploads());
    std::printf("shared_bytes %llu\n", (unsigned long long)map->renderer().sharedBytes());
    std::printf("shared_geometries %zu\n", map->renderer().sharedGeometries());
    std::printf("textures %zu\n", map->renderer().textures());
    std::printf("texture_uploads %llu\n", (unsigned long long)map->renderer().textureUploads());
    std::printf("texture_skipped %llu\n", (unsigned long long)map->renderer().textureSkipped());
    std::printf("atlas_mismatched %llu\n",
                (unsigned long long)map->renderer().atlasMismatched());
    // Before anything else is believed. A directory with no usable packages
    // renders black, and a probe that compares two of its own black frames
    // reports agreement -- which is how this was missed for a session.
    {
        const auto [loaded, rejected] = map->renderer().materialsLoaded();
        std::printf("materials_loaded %zu rejected %zu\n", loaded, rejected);
        if (loaded == 0) {
            std::fprintf(stderr,
                         "probe: no materials loaded from %s (%zu packages rejected). "
                         "Compile them for this backend's shader model -- filament resolves "
                         "Vulkan here as mobile, so matc needs -p desktop -p mobile.\n",
                         materialDir.c_str(), rejected);
            return 1;
        }
    }
    std::printf("no_uniforms %llu\n", (unsigned long long)map->renderer().noUniforms());
    std::printf("no_drawable_block %llu\n", (unsigned long long)map->renderer().noDrawableBlock());
    std::printf("missing_atlas %llu\n", (unsigned long long)map->renderer().missingAtlas());
    std::printf("pitched_labels %llu\n", (unsigned long long)map->renderer().pitchedLabels());
    std::printf("missing_batches %llu\n", (unsigned long long)map->renderer().missing());
    for (std::int32_t family : map->renderer().missingFamilies()) {
        std::printf("missing_family_%d 1\n", family);
    }
    if (!reason.empty()) {
        std::fprintf(stderr, "probe: %s\n", reason.c_str());
    }

    // One Filament View per offscreen pass, sharing the scene and the camera and differing in
    // three things: the render target it draws into, the layer bit it shows, and its viewport,
    // which is the target's rather than the frame's.
    //
    // The map's own view has to *stop* showing those bits, or the kernels would be drawn twice
    // -- once into the target where they belong and once over the map, which is the picture the
    // whole offscreen pass exists to avoid.
    std::vector<filament::View*> offscreenViews;
    std::uint8_t offscreenBits = 0;
    for (const auto& pass : map->renderer().offscreenPasses()) {
        filament::View* off = engine->createView();
        off->setScene(scene);
        off->setCamera(camera);
        off->setRenderTarget(pass.target);
        off->setViewport({0, 0, pass.width, pass.height});
        off->setVisibleLayers(0xFF, pass.layer);
        off->setPostProcessingEnabled(false);
        // Nothing that resamples. None of these was the quarter-scale loss the kernels take
        // through the target -- measured, with the quantum unchanged at 0.25 either way -- but
        // an offscreen pass that accumulates has no use for antialiasing or dithering, and
        // leaving them on would put a second suspect beside the first.
        off->setMultiSampleAntiAliasingOptions({.sampleCount = 1, .enabled = false});
        off->setAntiAliasing(filament::View::AntiAliasing::NONE);
        off->setDithering(filament::View::Dithering::NONE);
        // Nothing in this pass tests depth or stencil: the kernels are meant to overlap and sum.
        off->setStencilBufferEnabled(false);
        offscreenViews.push_back(off);
        offscreenBits = static_cast<std::uint8_t>(offscreenBits | pass.layer);
    }
    // TSF_KERNELS_ONSCREEN leaves the kernels visible to the map's own view as well, which is
    // how "is the pass wrong or is the layer wrong" gets answered: a kernel that lands in the
    // wrong place on screen lands in the wrong place in the target for the same reason.
    if (offscreenBits != 0 && std::getenv("TSF_KERNELS_ONSCREEN") == nullptr) {
        view->setVisibleLayers(0xFF, static_cast<std::uint8_t>(0xFF & ~offscreenBits));
    }
    std::fprintf(stderr, "offscreen_passes %zu\n", offscreenViews.size());

    // Rendered once before the capture. A headless swap chain hands out buffers in rotation, and
    // reading back on the very first frame returns one nothing has drawn into -- which produced a
    // constant image that did not move when the scene, the materials, or even the clear colour
    // changed, and cost a long detour before it was noticed.
    for (int warm = 0; warm < 2; warm++) {
        if (renderer->beginFrame(swapChain)) {
            for (filament::View* off : offscreenViews) {
                renderer->render(off);
            }
            renderer->render(view);
            renderer->endFrame();
        }
        engine->flushAndWait();
    }

    // The offscreen target's own contents, for when the layer that samples it looks wrong and
    // the question is which half is at fault. Written as a PPM of the density in all three
    // channels, at the target's own size rather than the frame's.
    if (const char* path = std::getenv("TSF_DUMP_TARGET"); path != nullptr && !offscreenViews.empty()) {
        const auto& pass = map->renderer().offscreenPasses().front();
        std::vector<float> texels(static_cast<std::size_t>(pass.width) * pass.height * 4);
        filament::backend::PixelBufferDescriptor pb(
            texels.data(), texels.size() * sizeof(float),
            filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::FLOAT);
        if (renderer->beginFrame(swapChain)) {
            for (filament::View* off : offscreenViews) {
                renderer->render(off);
            }
            renderer->readPixels(pass.target, 0, 0, pass.width, pass.height, std::move(pb));
            renderer->endFrame();
        }
        engine->flushAndWait();
        float peak = 0.0f;
        for (std::size_t i = 0; i < texels.size(); i += 4) {
            peak = std::max(peak, texels[i]);
        }
        double total = 0.0;
        for (std::size_t i = 0; i < texels.size(); i += 4) {
            total += texels[i];
        }
        std::fprintf(stderr, "target %ux%u peak_density %f integral %.1f\n", pass.width,
                     pass.height, (double)peak, total);
        if (FILE* out = std::fopen(path, "wb")) {
            std::fprintf(out, "P6\n%u %u\n255\n", pass.width, pass.height);
            for (std::size_t i = 0; i < texels.size(); i += 4) {
                const auto byte = static_cast<unsigned char>(
                    std::min(255.0f, std::max(0.0f, texels[i] * 255.0f)));
                std::fputc(byte, out); std::fputc(byte, out); std::fputc(byte, out);
            }
            std::fclose(out);
        }
    }

    std::vector<uint8_t> pixels(W * H * 4);
    const auto capture = [&](std::vector<uint8_t>& into) {
        filament::backend::PixelBufferDescriptor pb(into.data(), into.size(),
                                                    filament::backend::PixelDataFormat::RGBA,
                                                    filament::backend::PixelDataType::UBYTE);
        if (renderer->beginFrame(swapChain)) {
            // The passes that write what the map samples, before the map that samples them.
            for (filament::View* off : offscreenViews) {
                renderer->render(off);
            }
            renderer->render(view);
            renderer->readPixels(0, 0, W, H, std::move(pb));
            renderer->endFrame();
        }
        engine->flushAndWait();
    };

    // Settle on the *image*, which is the thing being measured, rather than on a proxy for it.
    //
    // The record count going quiet and nothing being outstanding are both necessary and neither is
    // sufficient: the same scene at pitch 45 gave 13,757 through 34,121 differing pixels across
    // six runs that all reported themselves settled, with an identical 39 renderables and nothing
    // missing. Rather than keep guessing which producer-side signal is the weak one, this ticks on
    // and re-reads the framebuffer until two consecutive captures agree. A frame that has stopped
    // changing has stopped changing, whatever the counters believe.
    //
    // Bounded, and it says whether it got there, for the reason the settle loop says so.
    int stable = 0;
    std::vector<uint8_t> previous(W * H * 4, 0);
    capture(pixels);
    for (; stable < 60; stable++) {
        previous.swap(pixels);
        for (int i = 0; i < 20; i++) {
            map->tick();
            pause_ms(5);
        }
        capture(pixels);
        if (pixels == previous) break;
    }
    std::printf("image_stable %d\n", stable < 60 ? 1 : 0);
    std::printf("stable_rounds %d\n", stable);

    size_t lit = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i] || pixels[i + 1] || pixels[i + 2]) lit++;
    }
    std::printf("lit_pixels %zu of %u\n", lit, W * H);
    {
        const auto [held, capacity] = map->ringPeak();
        const auto [live, slabs] = map->slabOccupancy();
        std::printf("ring_peak_mib %.3f of %.0f\n", (double)held / (1024.0 * 1024.0),
                    (double)capacity / (1024.0 * 1024.0));
        std::printf("slab_mib %.3f live %.3f in %llu slabs\n",
                    (double)map->slabUsed() / (1024.0 * 1024.0), (double)live / (1024.0 * 1024.0),
                    (unsigned long long)slabs);
    }

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

    map.reset();
    engine->destroy(view);
    engine->destroy(scene);
    engine->destroy(renderer);
    engine->destroyCameraComponent(cameraEntity);
    utils::EntityManager::get().destroy(cameraEntity);
    engine->destroy(swapChain);
    filament::Engine::destroy(&engine);
    return 0;
}
