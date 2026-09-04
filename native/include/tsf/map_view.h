// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tsf/filament_renderer.h>
#include <tsf/host.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace filament {
class Camera;
class Engine;
class Scene;
}  // namespace filament

namespace tsf {

/// One map drawn into a Filament scene somebody else owns.
///
/// The whole of the tessella-to-Filament integration, which is a `Host` for the
/// producer, a `FilamentRenderer` for the consumer, and a tick that moves
/// records from one to the other. `render_probe` did this inline; a platform
/// view needs the same five lines against a scene it did not create, so they
/// live here instead of being written twice.
///
/// Takes an engine and a scene rather than making them. Fluorite's `EngineHost`
/// is refcounted so several views share one engine, and a map is a guest in a
/// view's scene either way -- the probe building its own is incidental.
class MapView {
 public:
  /// Builds a map at a camera. Returns null and fills `error` when the style
  /// will not parse; a style whose *sources* will not resolve succeeds here and
  /// reports through `readiness()`, which is the round trip `create` avoids.
  static std::unique_ptr<MapView> create(filament::Engine* engine,
                                         filament::Scene* scene,
                                         const std::string& materialDir,
                                         const std::string& styleJson,
                                         std::uint32_t width,
                                         std::uint32_t height,
                                         double latitude,
                                         double longitude,
                                         double zoom,
                                         std::string* error,
                                         std::uint8_t layer = 0x01);

  ~MapView();

  MapView(const MapView&) = delete;
  MapView& operator=(const MapView&) = delete;

  /// Points a camera the way the capture stream expects. Call once per camera;
  /// it is a property of the camera rather than of this map, which is why it
  /// stays static on `FilamentRenderer`.
  ///
  /// `flipY` is whether the render target's first row is the bottom of the
  /// image. It is for an offscreen target read back with `readPixels`, which is
  /// what every probe here uses and what the PPM writers then flip again. It is
  /// not for a swapchain handed straight to a compositor -- a platform view's
  /// dma-buf is presented with its first row at the top, so flipping puts the
  /// map on its head.
  static void configureCamera(filament::Camera& camera, bool flipY = true);

  /// Moves the map's camera. Degrees for bearing and pitch, as the C API takes
  /// them. Emits nothing when nothing moved.
  bool setCamera(double latitude, double longitude, double zoom, double bearing, double pitch);

  /// Drains one frame's records into the scene. Call before rendering.
  void tick();

  /// Tiles asked for and not yet answered, plus an unfinished glyph fetch. Zero
  /// means nothing further arrives without another tick.
  [[nodiscard]] std::uint64_t pending() const;

  [[nodiscard]] tessella_readiness readiness(std::string* reason = nullptr) const;

  /// Records read since the map was created. A frame that changes nothing adds
  /// none, so a caller waiting for a settled frame watches this alongside
  /// `pending()` -- neither alone is sufficient.
  [[nodiscard]] std::uint64_t records() const;

  /// How far the slab region extends, in bytes. See `Host::slabUsed`.
  [[nodiscard]] std::uint64_t slabUsed() const;

  /// The most bytes the ring has ever held unread, and its capacity. See
  /// `Host::ringPeak`.
  [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> ringPeak() const;

  /// Bytes the slab table still claims, and how many slabs claim them. See
  /// `Host::slabOccupancy`.
  [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> slabOccupancy() const;

  /// Frame orders dropped for want of a camera. See `Host::orphanedOrders`.
  [[nodiscard]] std::uint64_t orphanedOrders() const;
  [[nodiscard]] std::uint64_t lastOrderEntries() const;
  [[nodiscard]] std::uint64_t lastBatches() const;

  /// The producer's own word on the last call. A tick that emitted nothing
  /// because the ring or the region was full says so here and nowhere else.
  [[nodiscard]] tessella_result lastResult() const;

  /// What the last `tick` spent producing and draining, in nanoseconds. See
  /// `Host::produceNs`.
  [[nodiscard]] std::uint64_t produceNs() const;
  [[nodiscard]] std::uint64_t drainNs() const;

  [[nodiscard]] FilamentRenderer& renderer() { return *renderer_; }

 private:
  MapView(std::unique_ptr<FilamentRenderer> renderer, std::unique_ptr<Host> host);

  std::unique_ptr<FilamentRenderer> renderer_;
  std::unique_ptr<Host> host_;
};

}  // namespace tsf
