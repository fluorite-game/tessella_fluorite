// SPDX-License-Identifier: Apache-2.0

import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';
import 'package:fluorite/fluorite.dart';
import 'package:tessella_fluorite/tessella_fluorite.dart';

import 'cities.dart';
import 'hud.dart';
import 'zoom_sweep.dart';

/// The style every pane draws, and the compiled Filament materials the consumer
/// binds. Both are host layout, so both come from the environment rather than
/// being baked in: the same app runs against a local tile server and against a
/// remote one, and the material directory is a build output.
const String _styleEnv = 'TESSELLA_STYLE';
const String _materialsEnv = 'TESSELLA_MATERIALS';

/// Which surface the panes draw on: `globe` for a sphere, anything else for the
/// plane. An environment variable rather than a control, for the reason the
/// style and the material directory are: this app is configured by its host, and
/// a pane can still be switched at runtime through [TessellaMaps.setProjection].
const String _projectionEnv = 'TESSELLA_PROJECTION';

/// Whether this run draws globes. Read once: the panes are configured before any
/// of them attaches, and the sweep's ceiling depends on it.
final bool _globe =
    (Platform.environment[_projectionEnv] ?? '').toLowerCase() == 'globe';

void main() {
  FlutterError.onError = FlutterError.presentError;
  runZonedGuarded<Future<void>>(
    () async {
      WidgetsFlutterBinding.ensureInitialized();

      final String stylePath = Platform.environment[_styleEnv] ?? '';
      final String materials = Platform.environment[_materialsEnv] ?? '';
      if (stylePath.isEmpty || materials.isEmpty) {
        stderr.writeln('set $_styleEnv and $_materialsEnv');
        exit(2);
      }

      // Cameras before views. The native side holds a slot's camera until that
      // view attaches, so setting all four here means no pane depends on when
      // its platform view happens to come up -- and a slot with no camera gets
      // no map, which is what keeps a stray view from drawing a fifth one.
      //
      // Slot order is view creation order, which for this tree is reading
      // order: the panes are built top row then bottom, left then right.
      TessellaMaps.install(
        styleJson: File(stylePath).readAsStringSync(),
        materialDirectory: materials,
      );
      for (int slot = 0; slot < kQuad.length; slot++) {
        final MapCamera city = kQuad[slot];
        TessellaMaps.setProjection(
          slot,
          _globe ? MapProjection.globe : MapProjection.mercator,
        );
        TessellaMaps.setCamera(
          slot,
          MapPosition(
            latitude: city.latitude,
            longitude: city.longitude,
            zoom: city.zoom,
            bearing: city.bearing,
            pitch: city.pitch,
          ),
        );
      }

      runApp(QuadApp(engine: FluoriteEngine()));
    },
    (final Object error, final StackTrace stack) {
      stdout.write('runZonedGuarded caught: $error\n$stack');
    },
  );
}

/// Four maps, four platform views, one app.
///
/// One [FluoriteEngine]: it is a singleton over the native `EngineHost`, which
/// is refcounted precisely so several views can share one Filament engine. Each
/// pane is its own `FluoriteView`, so each is its own ihs platform view -- and
/// they share the engine's one scene, which is why each map is on its own
/// Filament layer and each view is narrowed to it.
class QuadApp extends StatefulWidget {
  const QuadApp({super.key, required this.engine});

  final FluoriteEngine engine;

  @override
  State<QuadApp> createState() => _QuadAppState();
}

class _QuadAppState extends State<QuadApp> with SingleTickerProviderStateMixin {
  /// One ticker for all four panes, not one each: they sweep together, and four
  /// tickers would each wake the frame pipeline to set one camera.
  late final Ticker _ticker;

  /// How many panes are on screen now.
  ///
  /// Seeded from `TESSELLA_PANES` and changed while the app runs -- keys 1 to 4
  /// pick a count, and any other key cycles. Runtime rather than startup-only
  /// because the question it answers is a comparison: the same build, the same
  /// frame, one view against four, with nothing else moved. Dropping a pane
  /// tears its platform view down and the slot's map with it; adding one brings
  /// both back against the camera the slot has held all along.
  int _panes = kPanes;

  /// Takes the keyboard, so a count can be picked on a board with no pointer.
  final FocusNode _keys = FocusNode();

  /// When the current pass began, or null while still waiting to start.
  Duration? _passBegan;

  /// The last readiness check, so waiting does not poll the native side at the
  /// frame rate for an answer that changes on a network round trip.
  Duration _lastCheck = Duration.zero;
  static const Duration _checkEvery = Duration(milliseconds: 250);

  @override
  void initState() {
    super.initState();
    _ticker = createTicker(_onTick)..start();
  }

  /// Whether every pane has its sources and nothing outstanding.
  ///
  /// The sweep starts from here rather than from the first frame. Started cold,
  /// the whole of the first leg runs against a cache that has nothing in it --
  /// the camera is through a zoom before its tiles land -- so the one pass
  /// anybody watches from the beginning is the one pass with no map in it.
  bool _settled() {
    for (int slot = 0; slot < _panes; slot++) {
      final MapStats? stats = TessellaMaps.statsFor(slot);
      if (stats == null ||
          stats.readiness != MapReadiness.ready ||
          stats.pending != 0) {
        return false;
      }
    }
    return true;
  }

  /// How far in the sweep goes.
  ///
  /// A globe's bend is computed in 32-bit float over a position in `0..1` across the whole world,
  /// and at z12 one tile unit is smaller than that number can express -- so geometry quantizes and
  /// the planet comes apart. The sweep stops at eleven there rather than at eighteen, because a
  /// demo that drives past its own limit looks like a bug in the demo. plan.md's sixth globe item
  /// is what lifts it.
  double get _ceiling => _globe ? 11.0 : 18.0;

  void _onTick(final Duration elapsed) {
    final Duration? began = _passBegan;
    if (began == null) {
      if (elapsed - _lastCheck < _checkEvery) return;
      _lastCheck = elapsed;
      if (_settled()) _passBegan = elapsed;
      return;
    }

    Duration into = elapsed - began;
    // One sweep is a pass, and the passes run on: this is a demo of a camera
    // that never stops, and stopping it would leave the quad wherever the last
    // leg happened to end.
    final ZoomSweep first = ZoomSweep(home: kQuad.first.zoom, maxZoom: _ceiling);
    if (into >= first.total) {
      _passBegan = elapsed;
      into = Duration.zero;
    }

    for (int slot = 0; slot < _panes; slot++) {
      final MapCamera city = kQuad[slot];
      final ZoomSweep sweep = ZoomSweep(home: city.zoom, maxZoom: _ceiling);
      TessellaMaps.setCamera(
        slot,
        MapPosition(
          latitude: city.latitude,
          longitude: city.longitude,
          zoom: sweep.zoomAt(into),
          bearing: city.bearing,
          pitch: city.pitch,
        ),
      );
    }
  }

  @override
  void dispose() {
    _ticker.dispose();
    _keys.dispose();
    super.dispose();
  }

  FluoriteEngine get engine => widget.engine;

  /// Picks a count from a key: 1 to 4 name one, anything else steps 1-2-4-1.
  void _onKey(final KeyEvent event) {
    if (event is! KeyDownEvent) return;
    const Map<String, int> named = <String, int>{'1': 1, '2': 2, '3': 3, '4': 4};
    final int? asked = named[event.character];
    setState(() {
      _panes = asked ?? switch (_panes) { 1 => 2, 2 => 4, _ => 1 };
    });
  }

  @override
  Widget build(final BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      home: Scaffold(
        backgroundColor: Colors.black,
        body: SafeArea(
          child: KeyboardListener(
            focusNode: _keys,
            autofocus: true,
            onKeyEvent: _onKey,
            // One pane fills the window; two share a row; three or four fill
            // the grid, with the bottom row short when there are three.
            child: _panes == 1
                ? MapPane(engine: engine, slot: 0)
                : Column(
                    children: <Widget>[
                      Expanded(child: _row(0)),
                      if (_panes > 2) ...<Widget>[
                        const SizedBox(height: 2),
                        Expanded(child: _row(2)),
                      ],
                    ],
                  ),
          ),
        ),
      ),
    );
  }

  Widget _row(final int first) => Row(
        children: <Widget>[
          Expanded(child: MapPane(engine: engine, slot: first)),
          if (first + 1 < _panes) ...<Widget>[
            const SizedBox(width: 2),
            Expanded(child: MapPane(engine: engine, slot: first + 1)),
          ],
        ],
      );
}

/// One pane: a platform view with a map in it, and the city's name over it.
///
/// Nothing here creates the map. The view comes up, fluorite calls the
/// extension's attach with this view's engine, scene and size, and the map is
/// built against the camera the slot already holds. The widget's job is the
/// platform view and the label.
class MapPane extends StatelessWidget {
  const MapPane({super.key, required this.engine, required this.slot});

  final FluoriteEngine engine;
  final int slot;

  @override
  Widget build(final BuildContext context) {
    final MapCamera camera = kQuad[slot];
    return Stack(
      fit: StackFit.expand,
      children: <Widget>[
        FluoriteView(engine: engine),
        Positioned(
          left: 10,
          top: 8,
          child: MapHud(slot: slot, name: camera.name),
        ),
      ],
    );
  }
}
