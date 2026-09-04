// SPDX-License-Identifier: Apache-2.0

#include <tsf/map_view.h>

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
  config.style_json = styleJson.c_str();
  config.width = width;
  config.height = height;
  // 256 MB, which is what the probe has used throughout. A cover's worth of
  // records has never come close; the headroom is for a pan that outruns the
  // consumer, where a full ring drops a frame rather than corrupting one.
  config.ring_capacity = static_cast<std::size_t>(256) << 20;

  std::unique_ptr<Host> host = Host::create(config, latitude, longitude, zoom, error);
  if (!host) {
    return nullptr;
  }
  return std::unique_ptr<MapView>(new MapView(std::move(renderer), std::move(host)));
}

MapView::MapView(std::unique_ptr<FilamentRenderer> renderer, std::unique_ptr<Host> host)
    : renderer_(std::move(renderer)), host_(std::move(host)) {}

// Out of line, and the order matters: the renderer holds Filament buffers the
// engine made, so it goes before anything the caller destroys the engine with.
MapView::~MapView() = default;

void MapView::configureCamera(filament::Camera& camera) { FilamentRenderer::configureCamera(camera); }

bool MapView::setCamera(const double latitude,
                        const double longitude,
                        const double zoom,
                        const double bearing,
                        const double pitch) {
  return host_->setCamera(latitude, longitude, zoom, bearing, pitch);
}

void MapView::tick() {
  const std::uint64_t seen = host_->tick(*renderer_);
  host_->retire(seen);
}

std::uint64_t MapView::pending() const { return host_->pending(); }

std::uint64_t MapView::produceNs() const { return host_->produceNs(); }

std::uint64_t MapView::drainNs() const { return host_->drainNs(); }

std::uint64_t MapView::records() const { return host_->records(); }

tessella_readiness MapView::readiness(std::string* reason) const { return host_->readiness(reason); }

}  // namespace tsf
