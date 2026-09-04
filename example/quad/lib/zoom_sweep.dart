// SPDX-License-Identifier: Apache-2.0

import 'dart:math' as math;

/// A zoom that runs the whole range and comes back.
///
/// Three legs from the camera's own zoom: out to [minZoom], in to [maxZoom],
/// then home. Each is eased at both ends, so the sweep starts and stops without
/// a jerk and crosses the middle at speed.
///
/// Zoom is already logarithmic -- one level is a doubling -- so interpolating it
/// linearly is what makes the apparent rate constant. Interpolating the scale
/// instead would crawl at the bottom and tear through the top.
///
/// This is the hardest thing the pipeline is asked to do. Every level crossed
/// throws away a cover and asks for another, every label re-places against a
/// different set of neighbours, and the arena churns a whole screen of geometry
/// per level. A pan moves the camera; this replaces everything it is looking at.
class ZoomSweep {
  const ZoomSweep({
    required this.home,
    this.minZoom = 0.0,
    this.maxZoom = 18.0,
    this.out = const Duration(seconds: 5),
    this.into = const Duration(seconds: 7),
    this.back = const Duration(seconds: 3),
    this.hold = const Duration(seconds: 2),
  });

  /// Where the sweep starts and ends: the pane's own camera.
  final double home;
  final double minZoom;
  final double maxZoom;

  /// How long each leg takes. Out is shorter than in because it covers fewer
  /// levels from a street zoom, and the two read as one movement when their
  /// rates match rather than their durations.
  final Duration out;
  final Duration into;
  final Duration back;

  /// A rest at home before the next pass. A sweep that restarts the instant it
  /// arrives reads as a stutter rather than as a loop, and it is the only part
  /// of the cycle where the map is worth looking at.
  final Duration hold;

  Duration get total => out + into + back + hold;

  /// Whether [zoomAt] has anything left to do.
  bool isDone(final Duration elapsed) => elapsed >= total;

  /// The zoom at [elapsed] into the sweep.
  double zoomAt(final Duration elapsed) {
    final int us = elapsed.inMicroseconds;
    final int outUs = out.inMicroseconds;
    final int intoUs = into.inMicroseconds;
    final int backUs = back.inMicroseconds;

    if (us <= 0) return home;
    if (us < outUs) {
      return _lerp(home, minZoom, _ease(us / outUs));
    }
    if (us < outUs + intoUs) {
      return _lerp(minZoom, maxZoom, _ease((us - outUs) / intoUs));
    }
    if (us < outUs + intoUs + backUs) {
      return _lerp(maxZoom, home, _ease((us - outUs - intoUs) / backUs));
    }
    return home;
  }

  static double _lerp(final double a, final double b, final double t) =>
      a + (b - a) * t;

  /// Cosine ease, in and out. Smooth at both ends and at the joins between
  /// legs, where a linear ramp would change direction abruptly.
  static double _ease(final double t) {
    final double clamped = t.clamp(0.0, 1.0);
    return 0.5 - 0.5 * math.cos(clamped * math.pi);
  }
}
