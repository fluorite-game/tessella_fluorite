// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tsf/filament_renderer.h>
#include <tsf/host.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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

  /// Resizes the map in place. The tiles, the style, the atlases and the camera
  /// are all kept: the viewport is what the cover and every screen-space
  /// placement are computed from, so a resize is this and a frame.
  ///
  /// This is what a platform view's resize should reach, rather than building a
  /// second map -- which refetches and redecodes every tile on screen and holds
  /// two of everything while it does.
  bool setViewport(std::uint32_t width, std::uint32_t height);

  /// Drains one frame's records into the scene. Call before rendering.
  /// Tells the map how much time has passed, so its labels can fade.
  ///
  /// A map that is never told behaves as a still picture: a fade completes in one
  /// step, so a label appears and disappears outright. That is what a capture
  /// wants and what `mbgl-render` does; on a moving map it is what makes a label
  /// that stops being placed at one anchor and starts at another read as text
  /// that moved.
  /// Sets the surface this map's tiles are drawn on.
  ///
  /// The bend from normalized Mercator onto the sphere belongs to the renderer's vertex stage, so
  /// a consumer that ignores it draws a flat map for a producer that asked for a round one.
  bool setProjection(tessella_projection projection);

  /// Sets how many copies of the world the cover asks for.
  ///
  /// A globe wants one: every wrap of a tile bends to the same patch, so a repeated cover draws
  /// that patch twice and z-fights with itself.
  bool setWorldCopies(tessella_world_copies copies);

  /// Replaces this map's annotations from a GeoJSON feature collection. See
  /// `Host::setAnnotations`; before the first `tick`.
  bool setAnnotations(std::string_view geojson);

  /// Adds an encoded image a symbol annotation's `icon` can name. See
  /// `Host::addAnnotationImage`; before the first `tick`.
  bool addAnnotationImage(std::string_view id, std::string_view image, double pixelRatio = 1.0,
                          bool sdf = false);

  void advance(double elapsed_millis);

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
