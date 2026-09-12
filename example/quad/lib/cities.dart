// SPDX-License-Identifier: Apache-2.0

import 'dart:io';

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

/// How far every pane leans back, in degrees from straight down.
///
/// Fifteen. Pitch is an angle, and the producer clamps it at 89.25 -- mbgl's
/// `maxMercatorHorizonAngle`, past which the far plane runs to the horizon.
const double _pitch = 15.0;

/// The quad, in reading order: Seattle, Tokyo, Liestal, Shanghai.
///
/// All four sit at street zoom. A country-scale pane exercises a different part
/// of the pipeline -- coarse tiles, few labels -- and is worth having, but not
/// at the cost of the quad showing what a map looks like where it is used.
const List<MapCamera> kQuad = <MapCamera>[
  MapCamera(name: 'Seattle', latitude: 47.6062, longitude: -122.3321, zoom: 15, pitch: _pitch),
  MapCamera(name: 'Tokyo', latitude: 35.6812, longitude: 139.7671, zoom: 15, pitch: _pitch),
  // Basel-Landschaft: the town itself rather than the Jura around it.
  MapCamera(name: 'Liestal', latitude: 47.4839, longitude: 7.7345, zoom: 14, pitch: _pitch),
  // People's Square, so the Bund and both banks of the Huangpu are in frame.
  MapCamera(name: 'Shanghai', latitude: 31.2304, longitude: 121.4737, zoom: 14, pitch: _pitch),
];

/// How many of [kQuad] the app *starts* with, from `TESSELLA_PANES`.
///
/// Four by default, which is the app. The count is changed while it runs (keys
/// 1 to 4, or any other key to cycle); this only seeds it, so a board with no
/// keyboard can still be told what to open with.
///
/// One is how a fault gets cornered: the four panes are four platform views and
/// four Filament render targets, and a crash that only appears with all four is
/// a different fault from one that appears with one -- the quad crashed the V3D
/// driver inside `vkCreateImageView` where fluorite's own single-view example
/// did not, and this is what separates "tessella" from "four of anything".
int get kPanes {
  final int asked = int.tryParse(Platform.environment['TESSELLA_PANES'] ?? '') ?? kQuad.length;
  return asked.clamp(1, kQuad.length);
}
