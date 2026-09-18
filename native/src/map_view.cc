// SPDX-License-Identifier: Apache-2.0

#include <tsf/map_view.h>

#include <cstdlib>

#include <filament/Camera.h>

namespace tsf {

std::unique_ptr<MapView> MapView::create(filament::Engine* engine,
                                         filament::Scene* scene,
                                         const std::string& materialDir,
                                         const std::string& styleJson,
                                         const std::uint32_t width,
                                         const std::uint32_t height,
                                         const double latitude,
                                         const double longitude,
                                         const double zoom,
                                         std::string* error,
                                         const std::uint8_t layer) {
  if (engine == nullptr || scene == nullptr) {
    if (error != nullptr) {
      *error = "map view needs an engine and a scene";
    }
    return nullptr;
  }

  auto renderer =
      std::make_unique<FilamentRenderer>(engine, scene, materialDir, width, height, layer);

  tessella_config config{};
  config.style_json = reinterpret_cast<const uint8_t*>(styleJson.data());
  config.style_json_len = styleJson.size();
  config.width = width;
  config.height = height;
  // Sixty-four mebibytes, about six times the largest frame anything has
  // measured. The worst is not the densest style: an all-families scene at
  // 900x700 peaks at 11.0 MiB of unread ring, liberty's hundred and eleven
  // layers at 1920x1080 reach 6.0, and the quad's four street-level views at
  // 640x480 sit between 0.3 and 1.8. It was 256 MiB on the grounds that a cover
  // had never come close -- which was true, and was not a measurement, and four
  // views of it were most of a gigabyte of resident memory.
  //
  // The margin is asymmetric on purpose. A ring that fills mid-pan drops a frame
  // and the next one retries, but a ring that cannot hold *one* frame can never
  // make progress -- a frame is emitted whole or not at all. So this is sized
  // for the largest single frame rather than for the average, and a viewport far
  // beyond 1080p should raise it: the records scale with the tiles on screen.
  config.ring_capacity = static_cast<std::size_t>(64) << 20;
  // Zero takes the producer's default, which is 64 MiB. The region holds the
  // geometry of everything on screen plus what compaction has not reclaimed;
  // liberty at 1920x1080 reaches 13.8 MiB of it, and the quad 3 to 10.
  config.slab_capacity = 0;

  std::unique_ptr<Host> host = Host::create(config, latitude, longitude, zoom, error);
  if (!host) {
    return nullptr;
  }
  // A globe, if the environment asks for one. An env knob rather than a `create` parameter for
  // the reason `TSF_NO_FADES` and `TSF_NO_STENCIL` are: this is how a behavior is turned on for
  // a probe or a bug report without every caller having to carry an argument it does not use.
  // One world copy goes with it -- every wrap of a tile bends to the same patch, so a repeated
  // cover draws that patch twice and z-fights with itself.
  if (std::getenv("TSF_GLOBE") != nullptr) {
    host->setProjection(TESSELLA_PROJECTION_GLOBE);
    host->setWorldCopies(TESSELLA_WORLD_COPIES_ONE);
  }
  return std::unique_ptr<MapView>(new MapView(std::move(renderer), std::move(host)));
}

MapView::MapView(std::unique_ptr<FilamentRenderer> renderer, std::unique_ptr<Host> host)
    : renderer_(std::move(renderer)), host_(std::move(host)) {}

// Out of line, and the order matters: the renderer holds Filament buffers the
// engine made, so it goes before anything the caller destroys the engine with.
MapView::~MapView() = default;

void MapView::configureCamera(filament::Camera& camera, bool flipY) {
  FilamentRenderer::configureCamera(camera, flipY);
}

bool MapView::setCamera(const double latitude,
                        const double longitude,
                        const double zoom,
                        const double bearing,
                        const double pitch) {
  return host_->setCamera(latitude, longitude, zoom, bearing, pitch);
}

bool MapView::setViewport(const std::uint32_t width, const std::uint32_t height) {
  if (width == 0 || height == 0) {
    return false;
  }
  // The renderer first: it reads these when it turns a tile's clip-space box
  // into a scissor rectangle, and the frame the producer emits for the new
  // viewport is drawn through it.
  renderer_->setViewportSize(width, height);
  return host_->setViewport(width, height);
}

void MapView::advance(double elapsed_millis) {
  host_->advance(elapsed_millis);
}

void MapView::tick() {
  const std::uint64_t seen = host_->tick(*renderer_);
  host_->retire(seen);
}

std::uint64_t MapView::pending() const { return host_->pending(); }

std::uint64_t MapView::slabUsed() const { return host_->slabUsed(); }

std::pair<std::uint64_t, std::uint64_t> MapView::ringPeak() const { return host_->ringPeak(); }

std::pair<std::uint64_t, std::uint64_t> MapView::slabOccupancy() const {
  return host_->slabOccupancy();
}

std::uint64_t MapView::orphanedOrders() const { return host_->orphanedOrders(); }

std::uint64_t MapView::lastOrderEntries() const { return host_->lastOrderEntries(); }

std::uint64_t MapView::lastBatches() const { return host_->lastBatches(); }

bool MapView::setProjection(const tessella_projection projection) {
  return host_->setProjection(projection);
}

bool MapView::setWorldCopies(const tessella_world_copies copies) {
  return host_->setWorldCopies(copies);
}

bool MapView::setAnnotations(std::string_view geojson) {
  return host_->setAnnotations(geojson);
}

bool MapView::setGeojsonData(std::string_view source, std::string_view geojson) {
  return host_->setGeojsonData(source, geojson);
}

bool MapView::addAnnotationImage(std::string_view id, std::string_view image,
                                 const double pixelRatio, const bool sdf) {
  return host_->addAnnotationImage(id, image, pixelRatio, sdf);
}

tessella_result MapView::lastResult() const { return host_->lastResult(); }

std::uint64_t MapView::produceNs() const { return host_->produceNs(); }

std::uint64_t MapView::drainNs() const { return host_->drainNs(); }

std::uint64_t MapView::records() const { return host_->records(); }

tessella_readiness MapView::readiness(std::string* reason) const { return host_->readiness(reason); }

}  // namespace tsf
