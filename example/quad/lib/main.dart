// SPDX-License-Identifier: Apache-2.0

import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:fluorite/fluorite.dart';

import 'cities.dart';

void main() {
  FlutterError.onError = FlutterError.presentError;
  runZonedGuarded<Future<void>>(
    () async {
      WidgetsFlutterBinding.ensureInitialized();
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
/// pane is its own `FluoriteView`, so each is its own ihs platform view with its
/// own swapchain and scene.
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
              Expanded(child: _row(kQuad[0], kQuad[1])),
              const SizedBox(height: 2),
              Expanded(child: _row(kQuad[2], kQuad[3])),
            ],
          ),
        ),
      ),
    );
  }

  Widget _row(final MapCamera left, final MapCamera right) => Row(
        children: <Widget>[
          Expanded(child: MapPane(engine: engine, camera: left)),
          const SizedBox(width: 2),
          Expanded(child: MapPane(engine: engine, camera: right)),
        ],
      );
}

/// One pane: a platform view with a map in it, and the city's name over it.
class MapPane extends StatelessWidget {
  const MapPane({super.key, required this.engine, required this.camera});

  final FluoriteEngine engine;
  final MapCamera camera;

  @override
  Widget build(final BuildContext context) {
    return Stack(
      fit: StackFit.expand,
      children: <Widget>[
        FluoriteView(
          engine: engine,
          onCreated: (final FluoriteEngine engine) {
            // The seam the map goes through. `FilamentRenderer` takes an engine
            // and a scene rather than making them, so the bridge attaches one to
            // this view's scene and drives `tessella_tick` against it. Not built
            // yet -- see plan.md §18.
          },
        ),
        Positioned(
          left: 12,
          top: 10,
          child: IgnorePointer(
            child: Text(
              camera.name,
              style: const TextStyle(
                color: Colors.white,
                fontSize: 20,
                fontWeight: FontWeight.w600,
                shadows: <Shadow>[Shadow(blurRadius: 6, color: Colors.black87)],
              ),
            ),
          ),
        ),
      ],
    );
  }
}
