// SPDX-License-Identifier: Apache-2.0

import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:fluorite/fluorite.dart';
import 'package:tessella_fluorite/tessella_fluorite.dart';

import 'cities.dart';
import 'hud.dart';

/// The style every pane draws, and the compiled Filament materials the consumer
/// binds. Both are host layout, so both come from the environment rather than
/// being baked in: the same app runs against a local tile server and against a
/// remote one, and the material directory is a build output.
const String _styleEnv = 'TESSELLA_STYLE';
const String _materialsEnv = 'TESSELLA_MATERIALS';

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
class QuadApp extends StatelessWidget {
  const QuadApp({super.key, required this.engine});

  final FluoriteEngine engine;

  @override
  Widget build(final BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      home: Scaffold(
        backgroundColor: Colors.black,
        body: SafeArea(
          child: Column(
            children: <Widget>[
              Expanded(child: _row(0)),
              const SizedBox(height: 2),
              Expanded(child: _row(2)),
            ],
          ),
        ),
      ),
    );
  }

  Widget _row(final int first) => Row(
        children: <Widget>[
          Expanded(child: MapPane(engine: engine, slot: first)),
          const SizedBox(width: 2),
          Expanded(child: MapPane(engine: engine, slot: first + 1)),
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
