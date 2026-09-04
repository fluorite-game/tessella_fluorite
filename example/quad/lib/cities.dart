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

/// The quad, in reading order: Seattle, Tokyo, Liestal, Shanghai.
///
/// All four sit at street zoom. A country-scale pane exercises a different part
/// of the pipeline -- coarse tiles, few labels -- and is worth having, but not
/// at the cost of the quad showing what a map looks like where it is used.
const List<MapCamera> kQuad = <MapCamera>[
  MapCamera(name: 'Seattle', latitude: 47.6062, longitude: -122.3321, zoom: 15),
  MapCamera(name: 'Tokyo', latitude: 35.6812, longitude: 139.7671, zoom: 15),
  // Basel-Landschaft: the town itself rather than the Jura around it.
  MapCamera(name: 'Liestal', latitude: 47.4839, longitude: 7.7345, zoom: 14),
  // People's Square, so the Bund and both banks of the Huangpu are in frame.
  MapCamera(name: 'Shanghai', latitude: 31.2304, longitude: 121.4737, zoom: 14),
];
