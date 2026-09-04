// SPDX-License-Identifier: Apache-2.0

/// Where one pane of the quad looks.
///
/// Zoom is per city rather than shared: a country wants a different one from a
/// downtown, and the quad is meant to show that a view is a camera over a store
/// rather than a fixed scale.
class MapCamera {
  const MapCamera({
    required this.name,
    required this.latitude,
    required this.longitude,
    required this.zoom,
    this.bearing = 0.0,
    this.pitch = 0.0,
  });

  final String name;
  final double latitude;
  final double longitude;
  final double zoom;

  /// Degrees clockwise from north. Negated on the way into the transform, which
  /// is mbgl's convention -- see camera.rs `bearing_radians`.
  final double bearing;

  /// Degrees from straight down.
  final double pitch;
}

/// The quad, in reading order: Seattle, Tokyo, Switzerland, China.
const List<MapCamera> kQuad = <MapCamera>[
  MapCamera(name: 'Seattle', latitude: 47.6062, longitude: -122.3321, zoom: 13),
  MapCamera(name: 'Tokyo', latitude: 35.6812, longitude: 139.7671, zoom: 13),
  // The country, not a city: centred on the Bernese Alps at a zoom that holds
  // the whole of it.
  MapCamera(name: 'Switzerland', latitude: 46.8182, longitude: 8.2275, zoom: 8),
  // Likewise -- centred inland so the coast and the interior are both in frame.
  MapCamera(name: 'China', latitude: 35.8617, longitude: 104.1954, zoom: 4),
];
