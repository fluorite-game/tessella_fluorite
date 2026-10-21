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
    name: 'raster under vector',
    style: 'raster.json',
    latitude: 22.2820,
    longitude: _harbourLon,
    zoom: 13,
    pitch: 30,
    orbit: 24,
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
