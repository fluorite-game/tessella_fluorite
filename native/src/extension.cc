// SPDX-License-Identifier: Apache-2.0

#include <tsf/extension.h>

#include <tsf/map_view.h>

#include <fluorite/view_extension.h>

#include <filament/Engine.h>
#include <filament/Renderer.h>
#include <filament/View.h>
#include <filament/Viewport.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

/// Where one slot looks, and whether that has been sent to its map yet.
struct Camera {
  double latitude = 0.0;
  double longitude = 0.0;
  double zoom = 0.0;
  double bearing = 0.0;
  double pitch = 0.0;
  bool set = false;
  bool applied = false;
};

/// Which surface a slot's map draws on, and whether that has reached it yet.
///
/// The same shape as `Camera` above and for the same reason: a slot is configured before its map
/// exists, so the value is held and applied on the first frame after it does.
struct Projection {
  int32_t mode = TESSELLA_PROJECTION_MERCATOR;
  /// True at rest: a map is created flat, so the default needs no applying.
  bool applied = true;
};

/// A slot's recent frame history, for the HUD.
///
/// A fixed ring rather than a growing list: a HUD wants "the worst of the last
/// couple of seconds", and keeping every frame of a session to answer that
/// would be the largest allocation in the extension.
struct History {
  static constexpr std::size_t kWindow = 120;
  std::array<double, kWindow> produce{};
  std::array<double, kWindow> drain{};
  std::array<double, kWindow> delta{};
  std::size_t next = 0;
  std::uint64_t frames = 0;

  void record(double produceMs, double drainMs, double deltaS) {
    produce[next] = produceMs;
    drain[next] = drainMs;
    delta[next] = deltaS;
    next = (next + 1) % kWindow;
    frames++;
  }

  [[nodiscard]] std::size_t held() const {
    return frames < kWindow ? static_cast<std::size_t>(frames) : kWindow;
  }
  [[nodiscard]] double worst(const std::array<double, kWindow>& of) const {
    double out = 0.0;
    for (std::size_t i = 0; i < held(); i++) out = out > of[i] ? out : of[i];
    return out;
  }
  /// Frames per second over the window. From the intervals fluorite passes
  /// rather than from a clock here, so it is the view's own rate.
  [[nodiscard]] double fps() const {
    double total = 0.0;
    std::size_t counted = 0;
    for (std::size_t i = 0; i < held(); i++) {
      if (delta[i] > 0.0) {
        total += delta[i];
        counted++;
      }
    }
    return counted > 0 && total > 0.0 ? static_cast<double>(counted) / total : 0.0;
  }
};

struct Slot {
  std::unique_ptr<tsf::MapView> map;
  History history;
  Camera camera;
  Projection projection;
  /// The view this slot draws into, kept so the camera can be re-asserted. Not
  /// owned; valid between attach and detach.
  filament::View* view = nullptr;
  /// Whether the atlas mismatch has already been reported for this slot.
  bool reportedMismatch = false;
  /// Whether a failing tick has already been reported for this slot.
  bool reportedFailure = false;
  /// Frames this slot has been ticked, for the periodic report below.
  std::uint64_t frames = 0;
  /// Whether the per-frame drop counters have been reported for this slot.
  ///
  /// Here rather than in a probe for the reason the atlas mismatch is: a probe settles at one
  /// camera and these are the counters that only move while one is turning, so the only place
  /// they can be read is a running pane.
  bool reportedDrops = false;
  /// One view per offscreen pass this map needs, in `offscreenPasses()` order.
  ///
  /// A heatmap sums its kernels into a target and the second pass samples it, so without these
  /// the target is never drawn into and the layer reads an empty texture -- a heatmap that fails
  /// by drawing nothing, with no error anywhere. Owned here because the engine they are created
  /// on outlives them only until detach.
  std::vector<filament::View*> offscreen;
  /// The layer bits those passes draw on, which the pane's own view must stop showing: the
  /// kernels belong in the target, and a view that shows them paints them over the map as well.
  std::uint8_t offscreenBits = 0;
  /// How many passes `offscreen` was built for, so a style or a zoom that adds one is noticed.
  std::size_t offscreenBuilt = 0;
};

struct State {
  std::mutex mutex;
  std::string style;
  std::string materials;
  bool configured = false;
  bool installed = false;
  std::array<Slot, FLUORITE_VIEW_EXTENSION_MAX_SLOTS> slots;
  /// The engine every slot was attached with. One per process: fluorite's `EngineHost` is a
  /// refcounted singleton precisely so the views can share it.
  filament::Engine* engine = nullptr;
  /// What the offscreen passes are rendered with. Fluorite owns the renderer that draws the
  /// panes and the extension ABI does not hand it over, so the passes get their own.
  filament::Renderer* offscreenRenderer = nullptr;
};

State& state() {
  static State singleton;
  return singleton;
}

/// The layer this slot's renderables go on.
///
/// Bit `slot + 1`: layer 0 is the ECS content, which every view draws. Fluorite
/// narrows the view to `0x01 | (1 << (slot + 1))` on its side, so these two have
/// to agree and the agreement is the header's.
std::uint8_t layer_for(std::uint32_t slot) {
  return static_cast<std::uint8_t>(1u << (slot + 1));
}

void attach(void* /*user*/,
            std::uint32_t slot,
            void* engine,
            void* scene,
            void* view,
            std::uint32_t width,
            std::uint32_t height) {
  // A map is a display-referred sRGB layer, like the UI over it. Filament's
  // post-processing treats what a shader wrote as linear scene-referred light
  // and tone-maps it, and the styles come out of that as pale washes: roads at
  // a couple of percent contrast against their background, water gray. It is
  // not subtle and it is not a color to argue about -- it is a pipeline that
  // does not apply to this content.
  //
  // View-wide, so a pane that also draws ECS content loses tone mapping for
  // that too. That is the right default for a view whose job is a map, and an
  // app that wants both should say so; there is one filament::View per platform
  // view and no way to want the pipeline for half of it.
  auto* filamentView = static_cast<filament::View*>(view);
  if (filamentView != nullptr) {
    filamentView->setPostProcessingEnabled(false);
    // Tessella clips tiles with a stencil pass. Without a stencil attachment
    // the test has nothing to read and a tile is clipped to a band across the
    // middle of the map rather than to itself.
    //
    // Asking for one here is not this side's call, and asking was a latent
    // abort. Whoever owns the view decides: fluorite derives the swapchain's
    // depth format, enables the stencil on the view when that format carries
    // one, and only then calls attach -- `filament_producer.cc`, where the
    // comment says an external renderer that turned it on for a view whose
    // swapchain lacks the attachment "would abort the process the first time it
    // rendered with post-processing off", which is exactly this view's mode.
    // Filament's panic names it: "View has stencil buffer enabled, but SwapChain
    // does not have SwapChain::CONFIG_HAS_STENCIL_BUFFER flag set."
    //
    // So enabling it unconditionally did nothing when the attachment was there
    // and aborted when it was not. Read the decision instead; `extension_probe`
    // sets it before attach too, for the same reason.
    const bool stencil = filamentView->isStencilBufferEnabled();
    // TSF_NO_STENCIL turns it off, which is a diagnostic and not a mode: the
    // clipping goes wrong. It exists because allocating a stencil is what makes
    // a driver that cannot make an image view over the format it picks crash
    // inside vkCreateImageView with the render target half-built -- which is
    // what the Pi 5's V3D does, in the one place fluorite's own examples never
    // reach because none of them ask for a stencil. Disabling cannot panic, so
    // this direction is always safe. Whether it still helps on V3D is untested:
    // the attachment is allocated by whoever enabled it, which is now the other
    // side of attach.
    static const bool noStencil = std::getenv("TSF_NO_STENCIL") != nullptr;
    if (stencil && noStencil) {
      filamentView->setStencilBufferEnabled(false);
    }
    if (!stencil) {
      std::fprintf(stderr,
                   "tessella_fluorite: the view has no stencil, so tile clipping "
                   "will band across the map rather than clip each tile\n");
    }
  }
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  // Both early returns below are ordinary startup states rather than faults, and
  // both used to be silent. That is survivable when they resolve on a later
  // attach and expensive when they do not: a board where the map never appears
  // looks identical to one where it draws nothing, with no error anywhere and
  // the shell running at full speed. Said once per slot, so a view that attaches
  // before its camera does not repeat it every frame.
  if (slot >= shared.slots.size() || !shared.configured) {
    static std::array<bool, FLUORITE_VIEW_EXTENSION_MAX_SLOTS> told{};
    if (slot < told.size() && !told[slot]) {
      told[slot] = true;
      std::fprintf(stderr,
                   "[tessella_fluorite] slot %u: attach before configure; no map built\n",
                   slot);
    }
    return;
  }
  Slot& held = shared.slots[slot];
  // One engine for every slot -- fluorite's `EngineHost` is refcounted so the views can share
  // it -- so the offscreen renderer below is made once against this.
  shared.engine = static_cast<filament::Engine*>(engine);

  // A camera set before the view existed is the one it comes up at. Without a
  // camera the map has nowhere to point, so there is nothing to build yet.
  if (!held.camera.set) {
    static std::array<bool, FLUORITE_VIEW_EXTENSION_MAX_SLOTS> told{};
    if (slot < told.size() && !told[slot]) {
      told[slot] = true;
      std::fprintf(stderr,
                   "[tessella_fluorite] slot %u: attach before its camera; no map built\n",
                   slot);
    }
    return;
  }

  std::string error;
  held.map = tsf::MapView::create(static_cast<filament::Engine*>(engine),
                                  static_cast<filament::Scene*>(scene),
                                  shared.materials,
                                  shared.style,
                                  width,
                                  height,
                                  held.camera.latitude,
                                  held.camera.longitude,
                                  held.camera.zoom,
                                  &error,
                                  layer_for(slot));
  if (!held.map) {
    std::fprintf(stderr, "[tessella_fluorite] slot %u: %s\n", slot, error.c_str());
    return;
  }
  held.view = filamentView;
  // The scissor boxes have to be carried across the same way the geometry is.
  held.map->renderer().setFlipY(false);
  // The projection the capture stream is drawn through -- identity but for the
  // Y flip, because the producer's matrices already reach clip space. Fluorite
  // leaves the camera to the ECS, which has no opinion about a view drawing a
  // map, so this is the map's own.
  if (filamentView != nullptr) {
    tsf::MapView::configureCamera(filamentView->getCamera(), /*flipY=*/false);
  }
  // create() took the position but not the orientation.
  held.camera.applied = false;
}

/// Brings `held.offscreen` into agreement with what this map's renderer now asks for.
///
/// Rebuilt whole rather than diffed when the count changes: the passes are a handful, they only
/// change when a style or a zoom adds or drops a heatmap, and a target that moved under a view
/// that kept pointing at the old one is a class of bug not worth the saving.
void syncOffscreen(State& shared, Slot& held) {
  if (shared.engine == nullptr) {
    return;
  }
  const auto& passes = held.map->renderer().offscreenPasses();
  if (passes.size() == held.offscreenBuilt) {
    return;
  }
  for (filament::View* view : held.offscreen) {
    shared.engine->destroy(view);
  }
  held.offscreen.clear();
  held.offscreenBits = 0;
  held.offscreenBuilt = passes.size();
  if (passes.empty() || held.view == nullptr) {
    return;
  }
  for (const auto& pass : passes) {
    filament::View* off = shared.engine->createView();
    off->setScene(held.view->getScene());
    // The map's own camera. The kernels are placed in the same clip space the rest of the map
    // is, which is the whole reason a kernel that lands wrong on screen lands wrong in the
    // target for the same reason.
    off->setCamera(&held.view->getCamera());
    off->setRenderTarget(pass.target);
    off->setViewport({0, 0, pass.width, pass.height});
    off->setVisibleLayers(0xFF, pass.layer);
    off->setPostProcessingEnabled(false);
    // Nothing that resamples, and no depth or stencil: the kernels are meant to overlap and sum.
    off->setMultiSampleAntiAliasingOptions({.enabled = false, .sampleCount = 1});
    off->setAntiAliasing(filament::View::AntiAliasing::NONE);
    off->setDithering(filament::View::Dithering::NONE);
    off->setStencilBufferEnabled(false);
    held.offscreen.push_back(off);
    held.offscreenBits = static_cast<std::uint8_t>(held.offscreenBits | pass.layer);
  }
  std::fprintf(stderr, "[tessella_fluorite] slot %u: %zu offscreen pass(es) on layers 0x%02x\n",
               static_cast<unsigned>(&held - shared.slots.data()), passes.size(),
               held.offscreenBits);
}

/// Draws this slot's offscreen passes into their targets, before fluorite draws the pane.
///
/// `renderStandaloneView` rather than a `beginFrame`/`endFrame` pair: it renders a view that has
/// a render target and needs no swap chain, which is the only way this works here. A second
/// chain is not an option -- fluorite's Vulkan backend builds its chains from the imported
/// platform-view buffers and refuses a headless one outright, at any size, with
/// `VulkanSwapChain: Unable to create swapchain`.
///
/// Filament requires the call to be outside a begun frame and on the engine's thread. Both hold:
/// the extension's `frame` runs on the Filament API thread, inside the producer's strand, and
/// before fluorite has begun the pane's frame.
void renderOffscreen(State& shared, Slot& held) {
  if (held.offscreen.empty() || shared.engine == nullptr) {
    return;
  }
  if (shared.offscreenRenderer == nullptr) {
    shared.offscreenRenderer = shared.engine->createRenderer();
    if (shared.offscreenRenderer == nullptr) {
      return;
    }
    // Cleared every pass, to nothing. Filament's default is `clear = false`, which for a target
    // that is *summed into* means this frame's kernels land on the last frame's and the one
    // before it, for as long as the app runs. A heatmap with a hundred frames behind it is a
    // field whose every lit pixel is far past the ramp's top stop, so what it draws is a flat
    // disc of the last color with a one-pixel rim where the kernel finally falls below it --
    // which is exactly what the harbor demo showed and what `render_probe` does not, because a
    // probe captures two frames in and sets this on its own renderer.
    //
    // Transparent black rather than opaque: the density *is* the color here, and a target
    // cleared to alpha one starts every pixel at the top of the ramp.
    shared.offscreenRenderer->setClearOptions(
        {.clearColor = {0.0f, 0.0f, 0.0f, 0.0f}, .clear = true});
  }
  for (filament::View* off : held.offscreen) {
    shared.offscreenRenderer->renderStandaloneView(off);
  }
}

void frame(void* /*user*/, std::uint32_t slot, double delta_s) {
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size()) {
    return;
  }
  Slot& held = shared.slots[slot];
  if (!held.map) {
    return;
  }
  // Re-asserted rather than set once: `ApplyEcsCamera` runs on this view
  // immediately before this callback and will overwrite the projection whenever
  // an ECS camera is bound to the slot. Two matrix stores against a frame.
  if (held.view != nullptr) {
    tsf::MapView::configureCamera(held.view->getCamera(), /*flipY=*/false);
  }
  if (!held.projection.applied) {
    held.map->setProjection(static_cast<tessella_projection>(held.projection.mode));
    // One world copy with a globe: every wrap of a tile bends to the same patch, so a repeated
    // cover draws that patch twice and z-fights with itself. Set together here rather than left
    // to the caller, because a caller that forgot would see a globe that flickers on the
    // antimeridian and nothing naming the cause.
    held.map->setWorldCopies(held.projection.mode == TESSELLA_PROJECTION_GLOBE
                                 ? TESSELLA_WORLD_COPIES_ONE
                                 : TESSELLA_WORLD_COPIES_REPEATED);
    held.projection.applied = true;
  }
  if (!held.camera.applied) {
    held.map->setCamera(held.camera.latitude, held.camera.longitude, held.camera.zoom,
                        held.camera.bearing, held.camera.pitch);
    held.camera.applied = true;
  }
  // Before the tick, so this frame's fades are stepped by the time this frame took. Without it a
  // fade completes in one step and a label that stops being placed at one anchor and starts at
  // another along the same road switches rather than crossfading.
  //
  // TSF_NO_FADES turns them off, which is what a capture wants: `mbgl-render` runs in static map
  // mode where `symbolFadeChange` returns one, so instant fades are what a parity comparison is
  // against, and a settled probe that stops mid-fade reads a label at part of its color.
  static const bool noFades = std::getenv("TSF_NO_FADES") != nullptr;
  if (!noFades) {
    held.map->advance(delta_s * 1000.0);
  }
  held.map->tick();
  // A tick that fails leaves the consumer holding the previous frame, and the map looks frozen
  // while every counter above it keeps moving -- the camera advances, the producer renders, and
  // nothing says why. Said once per slot, like the atlas mismatch, because a failure that
  // repeats every frame repeats forever.
  if (!held.reportedFailure) {
    const tessella_result result = held.map->lastResult();
    if (result != TESSELLA_OK) {
      held.reportedFailure = true;
      std::string why;
      (void)held.map->readiness(&why);
      std::fprintf(stderr,
                   "[tessella_fluorite] slot %u: tick failed with result %d (%s); the frame is "
                   "not published and the view holds the last one\n",
                   slot, static_cast<int>(result), why.c_str());
    }
  }
  // After the tick, which is what announces a target, and before fluorite begins the pane's
  // frame, which is what reads it.
  syncOffscreen(shared, held);
  if (held.offscreenBits != 0 && held.view != nullptr) {
    // Re-asserted every frame for the reason the camera above is: the pane's view is fluorite's
    // and it narrows the visible layers on its own side. Only the offscreen bits are cleared;
    // the rest are left as fluorite set them.
    held.view->setVisibleLayers(held.offscreenBits, 0);
  }
  renderOffscreen(shared, held);
  // Said once per slot. A `texsize` that disagrees with the atlas bound for it draws every glyph
  // as a magnified corner of itself, and the only place that has been seen is here -- so the
  // report is here rather than in a probe that cannot reproduce it.
  // TSF_DROP_EVERY=N reports the per-frame drop counters every N frames rather than once. The
  // counters that matter under a turning camera are the ones that move from frame to frame, and
  // a single sighting of the first nonzero frame says nothing about how often.
  static const unsigned long dropEvery = std::getenv("TSF_DROP_EVERY") != nullptr
                                             ? std::strtoul(std::getenv("TSF_DROP_EVERY"), nullptr, 10)
                                             : 0;
  held.frames++;
  if (dropEvery != 0 && held.frames % dropEvery == 0) {
    const auto& r = held.map->renderer();
    std::fprintf(stderr,
                 "[tessella_fluorite] slot %u frame %llu: unplaced=%llu no_uniforms=%llu "
                 "no_block=%llu shared_slots=%llu draw_once_skipped=%llu redrawn=%llu "
                 "unmasked=%llu masked=%llu glyphs=%llu/%llu placements=%zu\n",
                 slot, (unsigned long long)held.frames, (unsigned long long)r.unplaced(),
                 (unsigned long long)r.noUniforms(), (unsigned long long)r.noDrawableBlock(),
                 (unsigned long long)r.sharedSlots(), (unsigned long long)r.drawOnceSkipped(),
                 (unsigned long long)r.redrawn(), (unsigned long long)r.unmasked(),
                 (unsigned long long)r.masked(), (unsigned long long)r.glyphsDrawn(),
                 (unsigned long long)r.glyphsHidden(), r.placements());
    // What the order actually issued this frame, by render pass. `glyphs` above counts a symbol
    // geometry *arriving*, so it is zero on every frame whose labels did not change -- these are
    // the numbers that say whether anything was drawn.
    std::string byPass;
    for (const auto& [pass, count] : r.passes()) {
      byPass += " p" + std::to_string(pass) + "=" + std::to_string(count);
    }
    std::fprintf(stderr, "[tessella_fluorite] slot %u frame %llu: passes%s prims=%llu\n", slot,
                 (unsigned long long)held.frames, byPass.c_str(),
                 (unsigned long long)r.primitives());
  }
  if (!held.reportedDrops) {
    const auto& r = held.map->renderer();
    if (r.unplaced() + r.noUniforms() + r.noDrawableBlock() + r.sharedSlots() +
            r.drawOnceSkipped() + r.redrawn() >
        0) {
      held.reportedDrops = true;
      std::fprintf(stderr,
                   "[tessella_fluorite] slot %u: drops unplaced=%llu no_uniforms=%llu "
                   "no_block=%llu shared_slots=%llu draw_once_skipped=%llu redrawn=%llu "
                   "glyphs=%llu/%llu\n",
                   slot, (unsigned long long)r.unplaced(), (unsigned long long)r.noUniforms(),
                   (unsigned long long)r.noDrawableBlock(), (unsigned long long)r.sharedSlots(),
                   (unsigned long long)r.drawOnceSkipped(), (unsigned long long)r.redrawn(),
                   (unsigned long long)r.glyphsDrawn(), (unsigned long long)r.glyphsHidden());
    }
  }
  if (!held.reportedMismatch && held.map->renderer().atlasMismatched() > 0) {
    held.reportedMismatch = true;
    std::fprintf(stderr, "[tessella_fluorite] slot %u: %llu drawables with a texsize the bound "
                         "atlas does not match\n",
                 slot, (unsigned long long)held.map->renderer().atlasMismatched());
  }
  held.history.record(static_cast<double>(held.map->produceNs()) / 1.0e6,
                      static_cast<double>(held.map->drainNs()) / 1.0e6, delta_s);
}

void resize(void* /*user*/,
            std::uint32_t slot,
            std::uint32_t width,
            std::uint32_t height) {
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size()) {
    return;
  }
  Slot& held = shared.slots[slot];
  // No map yet means attach has not built one -- a slot whose camera was never
  // set, most often. The size it is eventually built at is the current one, so
  // there is nothing to carry.
  if (!held.map) {
    return;
  }
  held.map->setViewport(width, height);
}

void detach(void* /*user*/, std::uint32_t slot) {
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size()) {
    return;
  }
  // The engine, scene and view this map was built against are about to go, so
  // the map goes first. The camera stays: it is the slot's, not the view's, and
  // a view that comes back should come back where it was. A resize no longer
  // arrives here at all -- the view survives one and `resize` carries it.
  if (shared.engine != nullptr) {
    for (filament::View* off : shared.slots[slot].offscreen) {
      shared.engine->destroy(off);
    }
  }
  shared.slots[slot].offscreen.clear();
  shared.slots[slot].offscreenBits = 0;
  shared.slots[slot].offscreenBuilt = 0;
  shared.slots[slot].map.reset();
  shared.slots[slot].view = nullptr;
  shared.slots[slot].camera.applied = false;
}

}  // namespace

extern "C" int32_t tessella_fluorite_configure(const char* style_json, const char* material_dir) {
  if (style_json == nullptr || material_dir == nullptr) {
    return -1;
  }
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  shared.style = style_json;
  shared.materials = material_dir;
  shared.configured = true;
  // One line each at startup for the two states that decide whether a map can
  // exist at all. Cheap, and the alternative is what this cost on a Pi 4: the
  // shell running at full speed with four empty panes, no error anywhere, and
  // no way from outside to tell "never configured" from "configured, and the
  // host never attached a view to it".
  std::fprintf(stderr, "[tessella_fluorite] configured\n");
  return 0;
}

extern "C" int32_t tessella_fluorite_install(void) {
  State& shared = state();
  {
    const std::lock_guard<std::mutex> lock(shared.mutex);
    if (!shared.configured) {
      return -1;
    }
    if (shared.installed) {
      return 0;
    }
    shared.installed = true;
  }
  // Outside the lock: fluorite copies the struct under its own, and holding
  // both at once is an ordering nothing else needs.
  const FluoriteViewExtension extension{
      .user = nullptr,
      .attach = attach,
      .frame = frame,
      .detach = detach,
      .resize = resize,
  };
  fluorite_set_view_extension(&extension);
  // The other half of the pair above: from here on, a view the host attaches
  // reaches `attach`. Silence after this line means the host never called it.
  std::fprintf(stderr, "[tessella_fluorite] view extension installed\n");
  return 0;
}

extern "C" void tessella_fluorite_uninstall(void) {
  fluorite_set_view_extension(nullptr);
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  shared.installed = false;
  // Clearing the extension gets no detach for views already attached, so the
  // maps are dropped here. Their views outlive them and draw the ECS content
  // alone, which is what a view with no extension has always done.
  for (Slot& held : shared.slots) {
    if (shared.engine != nullptr) {
      for (filament::View* off : held.offscreen) {
        shared.engine->destroy(off);
      }
    }
    held.offscreen.clear();
    held.offscreenBits = 0;
    held.offscreenBuilt = 0;
    held.map.reset();
    held.view = nullptr;
    held.camera.applied = false;
  }
}

extern "C" void tessella_fluorite_set_camera(uint32_t slot,
                                             double latitude,
                                             double longitude,
                                             double zoom,
                                             double bearing,
                                             double pitch) {
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size()) {
    return;
  }
  shared.slots[slot].camera = Camera{
      .latitude = latitude,
      .longitude = longitude,
      .zoom = zoom,
      .bearing = bearing,
      .pitch = pitch,
      .set = true,
      .applied = false,
  };
}

extern "C" void tessella_fluorite_set_projection(uint32_t slot, int32_t mode) {
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size()) {
    return;
  }
  // Anything this build does not know is the plane. A projection nobody agreed on would draw a
  // map through a matrix for a space nobody agreed on, and flat is the honest failure.
  const int32_t known =
      mode == TESSELLA_PROJECTION_GLOBE ? TESSELLA_PROJECTION_GLOBE : TESSELLA_PROJECTION_MERCATOR;
  if (shared.slots[slot].projection.mode != known) {
    shared.slots[slot].projection = Projection{.mode = known, .applied = false};
  }
}

extern "C" uint64_t tessella_fluorite_pending(uint32_t slot) {
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size() || !shared.slots[slot].map) {
    return 0;
  }
  return shared.slots[slot].map->pending();
}

extern "C" int32_t tessella_fluorite_attached(uint32_t slot) {
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  return slot < shared.slots.size() && shared.slots[slot].map ? 1 : 0;
}

extern "C" int32_t tessella_fluorite_stats_of(uint32_t slot, tessella_fluorite_stats* out) {
  if (out == nullptr) {
    return -1;
  }
  *out = tessella_fluorite_stats{};
  out->readiness = -1;
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size() || !shared.slots[slot].map) {
    return -1;
  }
  Slot& held = shared.slots[slot];
  const History& history = held.history;
  const auto [live, slabs] = held.map->slabOccupancy();
  static_cast<void>(slabs);
  const auto [ringHeld, ringCapacity] = held.map->ringPeak();
  static_cast<void>(ringCapacity);
  constexpr double kMib = 1024.0 * 1024.0;

  out->frames = history.frames;
  out->fps = history.fps();
  out->produce_ms = static_cast<double>(held.map->produceNs()) / 1.0e6;
  out->drain_ms = static_cast<double>(held.map->drainNs()) / 1.0e6;
  out->produce_ms_max = history.worst(history.produce);
  out->drain_ms_max = history.worst(history.drain);
  out->pending = held.map->pending();
  out->records = held.map->records();
  out->primitives = held.map->renderer().primitives();
  out->slab_mib = static_cast<double>(held.map->slabUsed()) / kMib;
  out->slab_live_mib = static_cast<double>(live) / kMib;
  out->ring_peak_mib = static_cast<double>(ringHeld) / kMib;
  out->latitude = held.camera.latitude;
  out->longitude = held.camera.longitude;
  out->zoom = held.camera.zoom;
  out->bearing = held.camera.bearing;
  out->pitch = held.camera.pitch;
  out->readiness = static_cast<int32_t>(held.map->readiness());
  return 0;
}

extern "C" int32_t tessella_fluorite_readiness(uint32_t slot,
                                               char* reason,
                                               int32_t reason_capacity) {
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size() || !shared.slots[slot].map) {
    return -1;
  }
  std::string text;
  const tessella_readiness ready = shared.slots[slot].map->readiness(&text);
  if (reason != nullptr && reason_capacity > 0) {
    const std::size_t room = static_cast<std::size_t>(reason_capacity) - 1;
    const std::size_t taken = text.size() < room ? text.size() : room;
    std::memcpy(reason, text.data(), taken);
    reason[taken] = '\0';
  }
  return static_cast<int32_t>(ready);
}
