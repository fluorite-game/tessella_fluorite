// SPDX-License-Identifier: Apache-2.0

#include <tsf/extension.h>

#include <tsf/map_view.h>

#include <fluorite/view_extension.h>

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

struct Slot {
  std::unique_ptr<tsf::MapView> map;
  Camera camera;
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
  static_cast<void>(view);
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
  // create() took the position but not the orientation.
  held.camera.applied = false;
}

void frame(void* /*user*/, std::uint32_t slot, double /*delta_s*/) {
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size()) {
    return;
  }
  Slot& held = shared.slots[slot];
  if (!held.map) {
    return;
  }
  if (!held.camera.applied) {
    held.map->setCamera(held.camera.latitude, held.camera.longitude, held.camera.zoom,
                        held.camera.bearing, held.camera.pitch);
    held.camera.applied = true;
  }
  held.map->tick();
}

void detach(void* /*user*/, std::uint32_t slot) {
  State& shared = state();
  const std::lock_guard<std::mutex> lock(shared.mutex);
  if (slot >= shared.slots.size()) {
    return;
  }
  // The engine, scene and view this map was built against are about to go, so
  // the map goes first. The camera stays: a resize detaches and attaches again,
  // and the view should come back where it was.
  shared.slots[slot].map.reset();
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
