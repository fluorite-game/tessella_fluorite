// SPDX-License-Identifier: Apache-2.0
//
// A tessella map in every Fluorite platform view.
//
// Fluorite hands every platform view the same Filament scene and differs them
// by camera, so a map per view is a map per *layer* in one scene. That is what
// the native half arranges; from here it is: say what style, install, and give
// each slot a camera.
//
//   await TessellaMaps.install(styleJson: style, materialDirectory: materials);
//   TessellaMaps.setCamera(0, const MapPosition(latitude: 47.6, longitude: -122.3, zoom: 13));
//
// Slots are view creation order, which is the order `FluoriteView`s are built
// in. Seven fit; Filament has eight layers and the first belongs to the ECS
// content, which draws in every view.

import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'src/ffi.dart' as ffi;

/// Where one view looks.
class MapPosition {
  const MapPosition({
    required this.latitude,
    required this.longitude,
    required this.zoom,
    this.bearing = 0.0,
    this.pitch = 0.0,
  });

  final double latitude;
  final double longitude;
  final double zoom;

  /// Degrees clockwise from north.
  final double bearing;

  /// Degrees from straight down.
  final double pitch;
}

/// What one pane cost on its last frames.
///
/// Read rather than pushed: a HUD samples this a couple of times a second, and
/// a callback per frame would cost more than the thing it measures.
class MapStats {
  const MapStats({
    required this.frames,
    required this.fps,
    required this.produceMs,
    required this.drainMs,
    required this.produceMsMax,
    required this.drainMsMax,
    required this.pending,
    required this.records,
    required this.primitives,
    required this.slabMib,
    required this.slabLiveMib,
    required this.ringPeakMib,
    required this.position,
    required this.readiness,
  });

  /// Frames this pane has been ticked, and its rate over the recent window.
  final int frames;
  final double fps;

  /// The last tick, split at the FFI boundary: [produceMs] is tessella's frame
  /// -- cover, layout, placement -- and [drainMs] is walking the ring into
  /// Filament. A slow pane is one or the other, never both.
  final double produceMs;
  final double drainMs;

  /// The worst of the last 120 ticks, which is what says whether a pane drops
  /// frames rather than merely running slow on average.
  final double produceMsMax;
  final double drainMsMax;

  /// Tiles asked for and not yet answered, plus an unfinished glyph fetch.
  final int pending;

  /// Records read since the map was created, and primitives now in the scene.
  final int records;
  final int primitives;

  /// The slab region: what the bump cursor has reached, and what the table
  /// still claims. The gap is what compaction has yet to take back.
  final double slabMib;
  final double slabLiveMib;

  /// The most the ring has ever held unread.
  final double ringPeakMib;

  /// Where this pane is looking, as last applied.
  final MapPosition position;

  final MapReadiness readiness;

  /// Total frame cost on the last tick.
  double get tickMs => produceMs + drainMs;
}

/// How far along a slot's map is.
///
/// The four live values mirror `tessella_readiness`, which is about the
/// *sources* rather than about the tiles: a map is ready once its manifests
/// have resolved, and goes on filling in afterwards. [MapStats.pending] is what
/// says whether anything is still arriving.
enum MapReadiness {
  /// Nothing attached: the view has not come up, or was given no camera.
  absent,

  /// Attached; the first tick has not started resolution yet.
  idle,

  /// The style's sources are resolving. No tile can be asked for until they do.
  resolving,

  /// Resolved. Tiles are built as they are wanted and land as they finish.
  ready,

  /// A source did not resolve, and nothing retries.
  /// [TessellaMaps.reasonFor] says why.
  failed,
}

/// `tessella_readiness` as this package spells it. Anything else is [absent],
/// which is what a slot with no map reports.
MapReadiness _readinessOf(final int value) => switch (value) {
      0 => MapReadiness.idle,
      1 => MapReadiness.resolving,
      2 => MapReadiness.ready,
      3 => MapReadiness.failed,
      _ => MapReadiness.absent,
    };

/// The maps, one per platform view.
///
/// Static because the native side is: one style and one extension per process,
/// which is what fluorite's `fluorite_set_view_extension` takes.
/// The surface a map's tiles are drawn on.
///
/// The producer's whole part in a globe is two matrices and a flag; the bend from normalized
/// Mercator onto the sphere is the renderer's vertex stage. Indices are the wire's, so the order
/// here is protocol rather than taste.
enum MapProjection {
  /// A plane. The default, and correct at every zoom.
  mercator,

  /// A sphere. Correct below zoom eleven -- see [TessellaMaps.setProjection].
  globe,
}

abstract final class TessellaMaps {
  /// Slots with a Filament layer of their own. Layer 0 is the ECS content.
  static const int maxSlots = 7;

  static bool _installed = false;

  /// Whether [install] has run and not been undone by [uninstall].
  static bool get installed => _installed;

  /// Names the style and registers the extension. From here on every platform
  /// view that comes up gets a map, provided its slot has a camera.
  ///
  /// Idempotent. Calling it again with a different style changes what the
  /// *next* view gets; views already up keep theirs, because rebuilding a map
  /// under a live scene would drop its tiles.
  static void install({required String styleJson, required String materialDirectory}) {
    final style = styleJson.toNativeUtf8();
    final materials = materialDirectory.toNativeUtf8();
    try {
      if (ffi.tessella_fluorite_configure(style.cast(), materials.cast()) != 0) {
        throw StateError('tessella_fluorite_configure refused the arguments');
      }
      if (ffi.tessella_fluorite_install() != 0) {
        throw StateError('tessella_fluorite_install found no configuration');
      }
      _installed = true;
    } finally {
      calloc.free(style);
      calloc.free(materials);
    }
  }

  /// Unregisters the extension and tears down every live map. The views stay
  /// and draw the ECS content alone, which is what a view without an extension
  /// has always done.
  static void uninstall() {
    ffi.tessella_fluorite_uninstall();
    _installed = false;
  }

  /// Points slot [slot] at [position]. Applied on that view's next frame, and
  /// held until then -- so a camera set before the view exists is the one it
  /// comes up at, and a slot with no camera gets no map.
  static void setCamera(int slot, MapPosition position) {
    _checkSlot(slot);
    ffi.tessella_fluorite_set_camera(
      slot,
      position.latitude,
      position.longitude,
      position.zoom,
      position.bearing,
      position.pitch,
    );
  }

  /// Tiles asked for and not yet answered on this slot, plus an unfinished
  /// glyph fetch. Zero and [attached] means the view has settled.
  /// Sets the surface a slot's map draws its tiles on.
  ///
  /// Held until the slot's map exists, as a camera is, so it can be set before the platform view
  /// attaches. [MapProjection.globe] also asks the cover for one copy of the world -- every wrap
  /// of a tile bends to the same patch, so a repeated cover draws that patch twice.
  ///
  /// A globe is only correct below zoom eleven. The bend is computed in 32-bit float over a
  /// position in `0..1` across the whole world, and at z12 one tile unit is smaller than that
  /// number can express, so geometry quantizes. Above it a map should be [MapProjection.mercator].
  static void setProjection(int slot, MapProjection projection) {
    _checkSlot(slot);
    ffi.tessella_fluorite_set_projection(slot, projection.index);
  }

  static int pending(int slot) {
    _checkSlot(slot);
    return ffi.tessella_fluorite_pending(slot);
  }

  /// Whether this slot has a map.
  static bool attached(int slot) {
    _checkSlot(slot);
    return ffi.tessella_fluorite_attached(slot) == 1;
  }

  /// How far along this slot is.
  static MapReadiness readiness(int slot) {
    _checkSlot(slot);
    return _readinessOf(ffi.tessella_fluorite_readiness(slot, nullptr, 0));
  }

  /// What this pane cost on its last frames, or null when nothing is attached.
  static MapStats? statsFor(int slot) {
    _checkSlot(slot);
    final buffer = calloc<ffi.TessellaStats>();
    try {
      if (ffi.tessella_fluorite_stats_of(slot, buffer) != 0) return null;
      final s = buffer.ref;
      return MapStats(
        frames: s.frames,
        fps: s.fps,
        produceMs: s.produceMs,
        drainMs: s.drainMs,
        produceMsMax: s.produceMsMax,
        drainMsMax: s.drainMsMax,
        pending: s.pending,
        records: s.records,
        primitives: s.primitives,
        slabMib: s.slabMib,
        slabLiveMib: s.slabLiveMib,
        ringPeakMib: s.ringPeakMib,
        position: MapPosition(
          latitude: s.latitude,
          longitude: s.longitude,
          zoom: s.zoom,
          bearing: s.bearing,
          pitch: s.pitch,
        ),
        readiness: _readinessOf(s.readiness),
      );
    } finally {
      calloc.free(buffer);
    }
  }

  /// Why this slot is where it is, or null when nothing is attached. Worth
  /// reading when [readiness] is [MapReadiness.failed]: an unreachable tile
  /// source and a style with no layers look the same from outside.
  static String? reasonFor(int slot) {
    _checkSlot(slot);
    const capacity = 512;
    final buffer = calloc<Char>(capacity);
    try {
      if (ffi.tessella_fluorite_readiness(slot, buffer, capacity) < 0) return null;
      return buffer.cast<Utf8>().toDartString();
    } finally {
      calloc.free(buffer);
    }
  }

  static void _checkSlot(int slot) {
    if (slot < 0 || slot >= maxSlots) {
      throw RangeError.range(slot, 0, maxSlots - 1, 'slot');
    }
  }
}
