// SPDX-License-Identifier: Apache-2.0

#include <tsf/extension.h>

#include <tsf/map_view.h>

#include <fluorite/view_extension.h>

#include <filament/View.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

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
  /// The view this slot draws into, kept so the camera can be re-asserted. Not
  /// owned; valid between attach and detach.
  filament::View* view = nullptr;
  /// Whether the atlas mismatch has already been reported for this slot.
  bool reportedMismatch = false;
};

struct State {
  std::mutex mutex;
  std::string style;
  std::string materials;
  bool configured = false;
  bool installed = false;
  std::array<Slot, FLUORITE_VIEW_EXTENSION_MAX_SLOTS> slots;
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
  // a couple of percent contrast against their background, water grey. It is
  // not subtle and it is not a colour to argue about -- it is a pipeline that
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
    filamentView->setStencilBufferEnabled(true);
  }
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size() || !shared.configured) {
    return;
  }
  Slot& held = shared.slots[slot];

  // A camera set before the view existed is the one it comes up at. Without a
  // camera the map has nowhere to point, so there is nothing to build yet.
  if (!held.camera.set) {
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
  // against, and a settled probe that stops mid-fade reads a label at part of its colour.
  static const bool noFades = std::getenv("TSF_NO_FADES") != nullptr;
  if (!noFades) {
    held.map->advance(delta_s * 1000.0);
  }
  held.map->tick();
  // Said once per slot. A `texsize` that disagrees with the atlas bound for it draws every glyph
  // as a magnified corner of itself, and the only place that has been seen is here -- so the
  // report is here rather than in a probe that cannot reproduce it.
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
