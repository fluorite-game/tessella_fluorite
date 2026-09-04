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

/// How far along a slot's map is.
enum MapReadiness {
  /// Nothing attached: the view has not come up, or was given no camera.
  absent,

  /// Attached and still fetching.
  loading,

  /// Everything the camera covers has arrived.
  ready,

  /// Attached but not going to finish. [TessellaMaps.reasonFor] says why.
  failed,
}

/// The maps, one per platform view.
///
/// Static because the native side is: one style and one extension per process,
/// which is what fluorite's `fluorite_set_view_extension` takes.
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
    final value = ffi.tessella_fluorite_readiness(slot, nullptr, 0);
    return switch (value) {
      0 => MapReadiness.loading,
      1 => MapReadiness.ready,
      2 => MapReadiness.failed,
      _ => MapReadiness.absent,
    };
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
