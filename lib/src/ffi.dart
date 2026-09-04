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

@Native<Uint64 Function(Uint32)>()
external int tessella_fluorite_pending(int slot);

@Native<Int32 Function(Uint32)>()
external int tessella_fluorite_attached(int slot);

@Native<Int32 Function(Uint32, Pointer<Char>, Int32)>()
external int tessella_fluorite_readiness(int slot, Pointer<Char> reason, int reasonCapacity);
