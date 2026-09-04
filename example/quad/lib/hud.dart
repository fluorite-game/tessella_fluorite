// SPDX-License-Identifier: Apache-2.0

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:tessella_fluorite/tessella_fluorite.dart';

/// One pane's frame statistics, over the map.
///
/// Polled rather than pushed. The numbers come from a lock-guarded snapshot on
/// the native side, and reading them at the frame rate would put the HUD in the
/// same lock the render thread takes every frame -- which is a HUD that changes
/// what it measures.
class MapHud extends StatefulWidget {
  const MapHud({super.key, required this.slot, required this.name});

  final int slot;
  final String name;

  @override
  State<MapHud> createState() => _MapHudState();
}

class _MapHudState extends State<MapHud> {
  static const Duration _interval = Duration(milliseconds: 500);

  Timer? _timer;
  MapStats? _stats;

  @override
  void initState() {
    super.initState();
    _timer = Timer.periodic(_interval, (final _) {
      final MapStats? stats = TessellaMaps.statsFor(widget.slot);
      if (mounted) setState(() => _stats = stats);
    });
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  @override
  Widget build(final BuildContext context) {
    final MapStats? stats = _stats;
    return IgnorePointer(
      child: Container(
        padding: const EdgeInsets.fromLTRB(8, 6, 8, 6),
        decoration: BoxDecoration(
          color: Colors.black.withValues(alpha: 0.62),
          borderRadius: BorderRadius.circular(4),
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Text(widget.name, style: _title),
            if (stats == null)
              Text('waiting for a view', style: _body)
            else ...<Widget>[
              // Frame cost first, and split: a slow pane is the producer or the
              // consumer, and which one it is decides what to do about it.
              _row('zoom', stats.position.zoom.toStringAsFixed(2)),
              _row('fps', stats.fps.toStringAsFixed(1)),
              _row('tick', '${stats.tickMs.toStringAsFixed(2)} ms'),
              _row(
                'prod/drain',
                '${stats.produceMs.toStringAsFixed(2)} / '
                    '${stats.drainMs.toStringAsFixed(2)} ms',
              ),
              _row(
                'worst',
                '${stats.produceMsMax.toStringAsFixed(2)} / '
                    '${stats.drainMsMax.toStringAsFixed(2)} ms',
              ),
              _row('prims', '${stats.primitives}'),
              _row('records', '${stats.records}'),
              _row('pending', '${stats.pending}'),
              _row(
                'slab',
                '${stats.slabLiveMib.toStringAsFixed(1)} / '
                    '${stats.slabMib.toStringAsFixed(1)} MiB',
              ),
              _row('ring', '${stats.ringPeakMib.toStringAsFixed(2)} MiB'),
              _row('state', stats.readiness.name),
            ],
          ],
        ),
      ),
    );
  }

  Widget _row(final String label, final String value) => Padding(
        padding: const EdgeInsets.only(top: 1),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            SizedBox(width: 72, child: Text(label, style: _label)),
            Text(value, style: _body),
          ],
        ),
      );
}

const TextStyle _title = TextStyle(
  color: Colors.white,
  fontSize: 13,
  fontWeight: FontWeight.w700,
  height: 1.3,
);

const TextStyle _label = TextStyle(
  color: Color(0xFF9AA4B2),
  fontSize: 11,
  fontFamily: 'monospace',
  height: 1.25,
);

// Tabular figures, so a number that changes every poll does not move the ones
// beside it.
const TextStyle _body = TextStyle(
  color: Colors.white,
  fontSize: 11,
  fontFamily: 'monospace',
  fontFeatures: <FontFeature>[FontFeature.tabularFigures()],
  height: 1.25,
);
