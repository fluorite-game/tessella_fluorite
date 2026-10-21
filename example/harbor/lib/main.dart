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
        // The first city, which is the pane's own camera: `cameraFor` offsets from
        // `kCities.first`, so over that one it is the identity and `kPanes` still reads as the
        // absolute cameras it is written as.
        TessellaMaps.setCamera(slot, cameraFor(pane, kCities.first, pane.bearing));
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
  /// How long one orbit takes, and how long the wall stays in one city.
  ///
  /// Both from the environment -- `TESSELLA_ORBIT_S` and `TESSELLA_DWELL_S`, read once here
  /// rather than per frame -- and both documented where they are parsed. A dwell of zero is one
  /// city for ever, which is what this was before cities existed.
  final Duration _pass = orbitPass;
  final Duration _dwell = dwell;

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

  /// Which of [kCities] the wall is over, and when it arrived.
  ///
  /// The clock is the ticker's own elapsed time rather than a wall clock, so a pause stops the
  /// dwell with the orbit: pausing to read a pane and finding it in another city a moment later
  /// is not what the pause is for.
  int _city = 0;
  Duration _held = Duration.zero;
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
    _held += elapsed - previous;

    // The swap. Every pane moves, including the ones that do not orbit -- a still pane is still
    // in a city -- which is why this is its own loop rather than a flag in the one below.
    final bool swapped = _dwell > Duration.zero && _held >= _dwell && kCities.length > 1;
    if (swapped) {
      _held = Duration.zero;
      _city = (_city + 1) % kCities.length;
      for (int slot = 0; slot < activePanes.length; slot++) {
        final Pane pane = activePanes[slot];
        TessellaMaps.setCamera(
          slot,
          cameraFor(pane, kCities[_city], (pane.bearing + pane.orbit * _phase) % 360.0),
        );
      }
      stdout.write('[harbor] city=${kCities[_city].name}\n');
    }
    // Said once a second, so "it stopped" can be told from "it is turning slowly" without
    // guessing from tile traffic -- a cached cover asks for nothing however fast it turns.
    _ticks++;
    if (_ticks % 60 == 0) {
      final Pane first = activePanes.first;
      stdout.write('[harbor] tick=$_ticks running=$_running city=${kCities[_city].name} '
          'phase=${_phase.toStringAsFixed(3)} bearing='
          '${((first.bearing + first.orbit * _phase) % 360.0).toStringAsFixed(2)}\n');
    }

    for (int slot = 0; slot < activePanes.length; slot++) {
      final Pane pane = activePanes[slot];
      // A pane that does not orbit has nothing to say here: the swap above already moved it,
      // and sending the same camera every frame is what a settled map asks not to be sent --
      // every one of those is a published frame and a redraw of a picture that did not change.
      if (pane.orbit == 0.0 || swapped) continue;
      TessellaMaps.setCamera(
        slot,
        cameraFor(
          pane,
          kCities[_city],
          // Wrapped rather than left to grow: the transform takes it modulo a
          // turn either way, but a bearing that counts up forever loses its
          // low bits, and this runs for as long as anybody watches it.
          (pane.bearing + pane.orbit * _phase) % 360.0,
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
