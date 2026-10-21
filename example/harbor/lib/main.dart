// SPDX-License-Identifier: Apache-2.0

import 'dart:async';
import 'dart:io';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';
import 'package:fluorite/fluorite.dart';
import 'package:tessella_fluorite/tessella_fluorite.dart';

import 'hud.dart';
import 'panes.dart';

/// Six styles over Victoria Harbour, one engine, one tile store.
void main() {
  FlutterError.onError = FlutterError.presentError;
  runZonedGuarded<Future<void>>(
    () async {
      WidgetsFlutterBinding.ensureInitialized();

      final List<Pane> panes = activePanes;
      if (styleDirectory.isEmpty || materialDirectory.isEmpty) {
        stderr.writeln('set TESSELLA_STYLE_DIR and TESSELLA_MATERIALS');
        exit(2);
      }

      // Cameras before views, for the reason the quad example gives: the native
      // side holds a slot's camera until that view attaches, so a pane does not
      // depend on when its platform view happens to come up, and a slot with no
      // camera gets no map -- which is what keeps a stray view from drawing one
      // this app did not ask for.
      for (int slot = 0; slot < panes.length; slot++) {
        final Pane pane = panes[slot];
        TessellaMaps.setProjection(slot, pane.projection);
        TessellaMaps.setCamera(
          slot,
          MapPosition(
            latitude: pane.latitude,
            longitude: pane.longitude,
            zoom: pane.zoom,
            bearing: pane.bearing,
            pitch: pane.pitch,
          ),
        );
      }

      // Only the first pane's style is installed here. The rest follow as each
      // pane attaches -- see [_HarborAppState._mountNext].
      installStyle(0);
      runApp(HarborApp(engine: FluoriteEngine()));
    },
    (final Object error, final StackTrace stack) {
      stdout.write('runZonedGuarded caught: $error\n$stack');
    },
  );
}

/// Points the extension at pane [slot]'s style.
///
/// `install` is idempotent and configure is not: calling it again names the
/// style the *next* view that attaches will read, and leaves the maps already
/// built alone. That is the whole mechanism behind a style per pane.
void installStyle(final int slot) {
  TessellaMaps.install(
    styleJson: File('$styleDirectory/${activePanes[slot].style}').readAsStringSync(),
    materialDirectory: materialDirectory,
  );
}

class HarborApp extends StatefulWidget {
  const HarborApp({super.key, required this.engine});

  final FluoriteEngine engine;

  @override
  State<HarborApp> createState() => _HarborAppState();
}

class _HarborAppState extends State<HarborApp> with SingleTickerProviderStateMixin {
  /// How long one orbit takes.
  ///
  /// Twenty-five seconds, so the panes are visibly turning. At ninety a pane moved about a third
  /// of a degree a second, which is below the rate at which anything reads as motion -- the map
  /// looked stopped, and a real pause was indistinguishable from it. Faster also makes the
  /// residual label churn easier to judge, because the camera crosses more of the bearings where
  /// placement's tile order flips.
  static const Duration _pass = Duration(seconds: 25);

  /// How often readiness is polled while panes are still coming up. Not every
  /// frame: the answer changes on a network round trip, and the call takes the
  /// same lock the render thread does.
  static const Duration _checkEvery = Duration(milliseconds: 200);

  late final Ticker _ticker;

  /// One poll for every HUD. See [MapHud.poll].
  final ChangeNotifier _poll = ChangeNotifier();
  Timer? _pollTimer;

  /// Panes whose view has been put in the tree. One at a time, because the next
  /// pane's style is installed only once the previous pane has read its own.
  int _mounted = 1;

  Duration _lastCheck = Duration.zero;

  /// Where the orbit is, in passes. Advanced by the ticker and held across a
  /// pause, so resuming does not jump.
  double _phase = 0.0;

  /// Ticks since the orbit started, for the heartbeat above.
  int _ticks = 0;
  Duration? _advancedAt;
  bool _running = true;

  @override
  void initState() {
    super.initState();
    _ticker = createTicker(_onTick)..start();
    // A handler on the hardware keyboard, not a focused widget.
    //
    // `KeyboardListener` only hears keys while its `FocusNode` has focus, and a platform view
    // takes focus the moment the window is clicked or moved -- after which the pause key does
    // nothing, and nothing on screen says it is the focus rather than the app. This is the
    // whole window's keyboard, for as long as the app is up.
    HardwareKeyboard.instance.addHandler(_onKey);
    _pollTimer = Timer.periodic(
      const Duration(milliseconds: 500),
      (final _) => _poll.notifyListeners(),
    );
  }

  @override
  void dispose() {
    HardwareKeyboard.instance.removeHandler(_onKey);
    _pollTimer?.cancel();
    _poll.dispose();
    _ticker.dispose();
    super.dispose();
  }

  /// Puts the next pane in the tree, having first named the style it will read.
  ///
  /// Gated on the pane before it being attached rather than on a timer. Two
  /// views that attach between one configure and the next would both read the
  /// same style, and the pane that lost would be a duplicate with no clue as to
  /// why -- so the app waits for the evidence that the style was consumed.
  void _mountNext(final Duration elapsed) {
    if (elapsed - _lastCheck < _checkEvery) return;
    _lastCheck = elapsed;
    if (!TessellaMaps.attached(_mounted - 1)) return;
    installStyle(_mounted);
    setState(() => _mounted++);
  }

  void _onTick(final Duration elapsed) {
    if (_mounted < activePanes.length) {
      _mountNext(elapsed);
      return;
    }

    final Duration? previous = _advancedAt;
    _advancedAt = elapsed;
    if (previous == null || !_running) return;
    _phase += (elapsed - previous).inMicroseconds / _pass.inMicroseconds;
    // Said once a second, so "it stopped" can be told from "it is turning slowly" without
    // guessing from tile traffic -- a cached cover asks for nothing however fast it turns.
    _ticks++;
    if (_ticks % 60 == 0) {
      final Pane first = activePanes.first;
      stdout.write('[harbor] tick=$_ticks running=$_running phase='
          '${_phase.toStringAsFixed(3)} bearing='
          '${((first.bearing + first.orbit * _phase) % 360.0).toStringAsFixed(2)}\n');
    }

    for (int slot = 0; slot < activePanes.length; slot++) {
      final Pane pane = activePanes[slot];
      if (pane.orbit == 0.0) continue;
      TessellaMaps.setCamera(
        slot,
        MapPosition(
          latitude: pane.latitude,
          longitude: pane.longitude,
          zoom: pane.zoom,
          // Wrapped rather than left to grow: the transform takes it modulo a
          // turn either way, but a bearing that counts up forever loses its
          // low bits, and this runs for as long as anybody watches it.
          bearing: (pane.bearing + pane.orbit * _phase) % 360.0,
          pitch: pane.pitch,
        ),
      );
    }
  }

  /// Space pauses the orbit, so a pane can be read rather than watched.
  ///
  /// The pane *count* is deliberately not a control here, unlike the quad: the
  /// slot a view takes is its creation order, and the styles were installed
  /// against that order. Tearing a pane down and bringing it back would hand it
  /// whichever style was configured last, which is a pane drawing somebody
  /// else's colors with nothing to say why. `TESSELLA_PANES` seeds the count
  /// instead, before any view exists.
  bool _onKey(final KeyEvent event) {
    if (event is! KeyDownEvent || event.logicalKey != LogicalKeyboardKey.space) {
      return false;
    }
    setState(() => _running = !_running);
    return true;
  }

  @override
  Widget build(final BuildContext context) {
    // Three across, so six panes are two rows. The grid is built at its full
    // size from the first frame and the cells fill in as the panes attach: a
    // tree that changed shape would rebuild the views already in it, and a view
    // that re-attaches takes a new slot.
    const int columns = 3;
    final int shown = activePanes.length;
    final int across = math.min(columns, shown);
    final int rows = (shown + across - 1) ~/ across;
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      home: Scaffold(
        backgroundColor: Colors.black,
        body: SafeArea(
          child: Column(
              children: <Widget>[
                for (int row = 0; row < rows; row++) ...<Widget>[
                  if (row > 0) const SizedBox(height: 2),
                  Expanded(
                    child: Row(
                      children: <Widget>[
                        // Only the cells that exist. A short last row -- seven panes three
                        // across -- would otherwise carry two `Expanded` blanks and leave the
                        // pane on them at a third of the width with black beside it.
                        for (int column = 0;
                            column < across && row * across + column < shown;
                            column++) ...<Widget>[
                          if (column > 0) const SizedBox(width: 2),
                          Expanded(child: _cell(row * across + column)),
                        ],
                      ],
                    ),
                  ),
                ],
              ],
          ),
        ),
      ),
    );
  }

  Widget _cell(final int slot) {
    if (slot >= activePanes.length) return const SizedBox.shrink();
    if (slot >= _mounted) {
      return ColoredBox(
        color: const Color(0xFF0B0D12),
        child: Center(
          child: Text(
            activePanes[slot].name,
            textAlign: TextAlign.center,
            style: const TextStyle(color: Color(0xFF3C4350), fontSize: 12),
          ),
        ),
      );
    }
    return MapPane(engine: widget.engine, slot: slot, poll: _poll);
  }
}

/// One pane: a platform view with a map in it, and its style's name over it.
class MapPane extends StatelessWidget {
  const MapPane({
    super.key,
    required this.engine,
    required this.slot,
    required this.poll,
  });

  final FluoriteEngine engine;
  final int slot;
  final Listenable poll;

  @override
  Widget build(final BuildContext context) => Stack(
        fit: StackFit.expand,
        children: <Widget>[
          FluoriteView(engine: engine),
          Positioned(
            left: 10,
            top: 8,
            child: MapHud(
              slot: slot,
              name: activePanes[slot].name,
              poll: poll,
            ),
          ),
        ],
      );
}
