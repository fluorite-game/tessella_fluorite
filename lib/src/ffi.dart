// SPDX-License-Identifier: Apache-2.0
//
// The C surface of libtessella_fluorite.so. See native/include/tsf/extension.h.

@DefaultAsset('package:tessella_fluorite/src/ffi.dart')
library;

import 'dart:ffi';

@Native<Int32 Function(Pointer<Char>, Pointer<Char>)>()
external int tessella_fluorite_configure(Pointer<Char> styleJson, Pointer<Char> materialDir);

@Native<Int32 Function()>()
external int tessella_fluorite_install();

@Native<Void Function()>()
external void tessella_fluorite_uninstall();

@Native<Void Function(Uint32, Double, Double, Double, Double, Double)>()
external void tessella_fluorite_set_camera(
  int slot,
  double latitude,
  double longitude,
  double zoom,
  double bearing,
  double pitch,
);

@Native<Void Function(Uint32, Int32)>()
external void tessella_fluorite_set_projection(int slot, int mode);

@Native<Uint64 Function(Uint32)>()
external int tessella_fluorite_pending(int slot);

@Native<Int32 Function(Uint32)>()
external int tessella_fluorite_attached(int slot);

@Native<Int32 Function(Uint32, Pointer<Char>, Int32)>()
external int tessella_fluorite_readiness(int slot, Pointer<Char> reason, int reasonCapacity);

/// Mirrors `tessella_fluorite_stats`. Field order and types are the ABI.
final class TessellaStats extends Struct {
  @Uint64()
  external int frames;
  @Double()
  external double fps;
  @Double()
  external double produceMs;
  @Double()
  external double drainMs;
  @Double()
  external double produceMsMax;
  @Double()
  external double drainMsMax;
  @Uint64()
  external int pending;
  @Uint64()
  external int records;
  @Uint64()
  external int primitives;
  @Double()
  external double slabMib;
  @Double()
  external double slabLiveMib;
  @Double()
  external double ringPeakMib;
  @Double()
  external double latitude;
  @Double()
  external double longitude;
  @Double()
  external double zoom;
  @Double()
  external double bearing;
  @Double()
  external double pitch;
  @Int32()
  external int readiness;
}

@Native<Int32 Function(Uint32, Pointer<TessellaStats>)>()
external int tessella_fluorite_stats_of(int slot, Pointer<TessellaStats> out);
