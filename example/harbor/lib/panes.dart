// SPDX-License-Identifier: Apache-2.0

import 'dart:io';

import 'package:tessella_fluorite/tessella_fluorite.dart';

/// One pane: a style, a camera over Victoria Harbour, and a surface.
///
/// A style per pane rather than one for all of them. The extension reads the
/// configured style when a view attaches, so the app installs a style, lets the
/// pane it belongs to come up, and installs the next -- which is what makes six
/// styles in one process possible without six engines.
class Pane {
  const Pane({
    required this.name,
    required this.style,
    required this.latitude,
    required this.longitude,
    required this.zoom,
    this.bearing = 0.0,
    this.pitch = 0.0,
    this.orbit = 0.0,
    this.projection = MapProjection.mercator,
  });

  /// What the HUD calls it.
  final String name;

  /// File name under the style directory.
  final String style;

  final double latitude;
  final double longitude;
  final double zoom;

  /// Degrees clockwise from north, at the start of a pass.
  final double bearing;

  /// Degrees from straight down.
  final double pitch;

  /// Degrees of bearing this pane turns through in one pass. Zero holds still,
  /// which is what a heatmap wants: the density is the subject and a turning
  /// camera only makes it hard to read.
  final double orbit;

  final MapProjection projection;
}

/// Where the harbour is: the water between Central and Tsim Sha Tsui.
const double _harbourLat = 22.2945;
const double _harbourLon = 114.1700;

/// The six panes, in reading order.
///
/// Every one of them looks at the same water. What differs is the style, which
/// is the point -- the archive, the tile store and the producer are shared, and
/// what reaches the screen is decided by the style alone.
///
/// Zoom stays at or under fifteen because that is the archive's maximum; past it
/// a pane is drawing overzoomed tiles and the detail it appears to gain is the
/// renderer's, not the data's.
const List<Pane> kPanes = <Pane>[
  Pane(
    name: 'day — 3D, bilingual',
    style: 'day.json',
    latitude: 22.2885,
    longitude: 114.1660,
    zoom: 15,
    bearing: 10,
    pitch: 62,
    orbit: 36,
  ),
  Pane(
    name: 'night — towers, glow',
    style: 'night.json',
    latitude: 22.2822,
    longitude: 114.1595,
    zoom: 15,
    bearing: 335,
    pitch: 66,
    orbit: -30,
  ),
  Pane(
    name: 'heatmap — nightlife',
    style: 'heat.json',
    latitude: 22.2930,
    longitude: 114.1700,
    zoom: 13.8,
  ),
  Pane(
    name: 'patterns, dashes, icons',
    style: 'patterns.json',
    latitude: 22.2935,
    longitude: 114.1725,
    zoom: 15,
    bearing: 45,
    pitch: 22,
    orbit: 20,
  ),
  Pane(
    name: 'terrain — the Peak',
    style: 'terrain.json',
    latitude: 22.2960,
    longitude: _harbourLon,
    // 13, and an overzoomed raster-dem draws: past its maxzoom the ground is built from the
    // coarser tile standing in for it. At this camera, declaring the DEM's maxzoom 15, 14, 13
    // and 12 leaves 5, 2, 0 and 0 sky pixels of 1,024,000 with every frame fully lit, and z13
    // over the native z14 source draws 635 primitives with no sky and no needles.
    //
    // The 14.6 that stood here was a workaround for a blank that does not reproduce. The
    // needles it also blamed were real, and were the mesh at a zoom with no native tiles under
    // it -- `tools/dem.py` answers for anywhere it is asked.
    //
    // It is not free: z13 covers far more ground for the one producer the six panes share, and
    // the whole wall reads about 41 fps here against 51 at 14.6 -- interleaved A/B on an idle
    // host, three pairs at the same point in the orbit. Worth it for the view, and worth
    // knowing before blaming a frame rate on something else.
    zoom: 13,
    bearing: 195,
    pitch: 68,
    orbit: 12,
  ),
  Pane(
    name: 'globe — Pearl River Delta',
    style: 'globe.json',
    latitude: 22.3600,
    longitude: 114.0800,
    zoom: 8.4,
    pitch: 12,
    orbit: 48,
    projection: MapProjection.globe,
  ),
];

/// A place the whole wall flies to, keeping every pane's style and framing.
///
/// "Same styles, different location": a pane carries a zoom, a pitch, a bearing and an orbit
/// that were chosen to show its style off, and none of that is about Hong Kong. Only the center
/// is. So a city is a center, and a pane's camera over it is that center plus the offset the
/// pane already has from [kCities] `.first` -- which keeps `kPanes` readable as the absolute
/// cameras they were written as, and keeps the six panes the same distance apart in every city.
///
/// The offsets are hundredths of a degree, so every pane lands inside the city it names. They
/// are not landmarks: the terrain pane looks at The Peak in Hong Kong and at whatever is a
/// kilometer south-west of the center elsewhere, which is what showing a *style* in three places
/// means.
class City {
  const City({required this.name, required this.latitude, required this.longitude});

  /// What the heartbeat and the HUD call it.
  final String name;
  final double latitude;
  final double longitude;
}

/// Where the wall goes, in order, returning to the first after the last.
///
/// Each needs an archive behind it or its panes draw background: `tools/cities.py` routes one
/// source over `hongkong_z15`, `tokyo_z15` and `sanfrancisco_z15`, and the README says how to
/// cut another. A city with no archive is not an error anywhere -- the tiles simply 404 and the
/// map draws nothing -- so the failure looks like an empty pane rather than a message.
const List<City> kCities = <City>[
  City(name: 'Hong Kong', latitude: _harbourLat, longitude: _harbourLon),
  City(name: 'Tokyo', latitude: 35.6800, longitude: 139.7600),
  // The north shore rather than downtown: Fort Mason to the east, the Presidio to the west and
  // the Golden Gate off the north-west corner. Downtown is a street grid, which the day and
  // night panes already have in Hong Kong and Tokyo; this is the one city of the three with a
  // bridge, a headland and a waterfront in one frame, and the terrain pane has something to
  // climb. Every pane's offset stays inside the archive from here -- the globe pane reaches
  // furthest, to -122.545, against a cut that starts at -122.60.
  City(name: 'San Francisco', latitude: 37.8060, longitude: -122.4550),
];

/// [pane]'s camera over [city]: its own framing, moved by the city's offset from the first.
MapPosition cameraFor(final Pane pane, final City city, final double bearing) {
  final City home = kCities.first;
  return MapPosition(
    latitude: pane.latitude + (city.latitude - home.latitude),
    longitude: pane.longitude + (city.longitude - home.longitude),
    zoom: pane.zoom,
    bearing: bearing,
    pitch: pane.pitch,
  );
}

/// How long the wall stays in one city, from `TESSELLA_DWELL_S`.
///
/// Zero, or anything that does not parse, stays in [kCities] `.first` for ever -- which is what
/// this demo did before cities existed, and what a parity capture or a single-pane investigation
/// wants. Forty-five seconds by default: long enough for a pane to finish most of an orbit and
/// for its tiles to settle, short enough that a passer-by sees the swap.
Duration get dwell {
  final double asked =
      double.tryParse(Platform.environment['TESSELLA_DWELL_S'] ?? '') ?? 45.0;
  if (!asked.isFinite || asked <= 0.0) return Duration.zero;
  return Duration(microseconds: (asked * Duration.microsecondsPerSecond).round());
}

/// How long one orbit takes, from `TESSELLA_ORBIT_S`.
///
/// Twenty-five seconds by default, so the panes are visibly turning. At ninety a pane moved
/// about a third of a degree a second, which is below the rate at which anything reads as
/// motion -- the map looked stopped, and a real pause was indistinguishable from it. Faster also
/// makes the residual label churn easier to judge, because the camera crosses more of the
/// bearings where placement's tile order flips.
///
/// Clamped off zero rather than taken literally: an orbit of no duration divides by it.
Duration get orbitPass {
  final double asked =
      double.tryParse(Platform.environment['TESSELLA_ORBIT_S'] ?? '') ?? 25.0;
  final double seconds = (!asked.isFinite || asked <= 0.0) ? 25.0 : asked;
  return Duration(microseconds: (seconds * Duration.microsecondsPerSecond).round());
}

/// Where the style files are, from `TESSELLA_STYLE_DIR`. The styles are host
/// layout like the materials: the same app runs against a local archive and a
/// remote one, and only the files say which.
String get styleDirectory => Platform.environment['TESSELLA_STYLE_DIR'] ?? '';

/// The compiled Filament material packages, from `TESSELLA_MATERIALS`.
String get materialDirectory => Platform.environment['TESSELLA_MATERIALS'] ?? '';

/// How many of [kPanes] the app starts with, from `TESSELLA_PANES`.
///
/// Six by default. One is how a fault gets cornered, for the reason the quad
/// example gives: six panes are six platform views and six Filament render
/// targets, and a crash that needs all six is a different fault from one that
/// appears with a single view.
/// One pane, by index, from `TESSELLA_PANE_ONLY`, or null for all of them.
///
/// A pane on its own is how a fault gets attributed. The watch instrumentation keys on the
/// label's text and its frame counter is process-global, so with several maps running the lines
/// interleave and the same label carries a different id per map -- which reads as id instability
/// and is nothing of the kind. Running one pane removes the ambiguity rather than working around
/// it.
int? get paneOnly {
  final int? asked = int.tryParse(Platform.environment['TESSELLA_PANE_ONLY'] ?? '');
  if (asked == null || asked < 0 || asked >= kPanes.length) return null;
  return asked;
}

/// The panes this run draws, in slot order.
List<Pane> get activePanes {
  final int? only = paneOnly;
  return only == null ? kPanes.sublist(0, paneCount) : <Pane>[kPanes[only]];
}

int get paneCount {
  final int asked =
      int.tryParse(Platform.environment['TESSELLA_PANES'] ?? '') ?? kPanes.length;
  return asked.clamp(1, kPanes.length);
}
