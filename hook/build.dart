// SPDX-License-Identifier: Apache-2.0
//
// Builds libtessella_fluorite.so and publishes it as a bundled CodeAsset under
// the id lib/src/ffi.dart names in its @DefaultAsset.
//
// Four things this cannot know and the app must say, since user-defines are the
// only channel that reaches a hook -- hooks_runner strips the environment down
// to PATH, HOME, TMPDIR and a short allow-list:
//
//   hooks:
//     user_defines:
//       tessella_fluorite:
//         tessella_dir: ...     # tessella checkout (for the capture ABI header)
//         filament_include: ... # Filament headers
//         fluorite_include: ... # fluorite's include/, for view_extension.h
//         cargo_profile: release
//
// Each also reads an environment variable of the same name upcased and
// TESSELLA_-prefixed, which only helps a hook run directly with --config; under
// a real build the runner has already stripped it. Relative paths resolve
// against this package, because the protocol does not specify a working
// directory.

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) return;

    final packageRoot = input.packageRoot.toFilePath();
    final tessellaDir =
        _dir(input, 'tessella_dir', 'TESSELLA_DIR') ?? '${packageRoot}../tessella';
    final filamentInclude = _dir(input, 'filament_include', 'TESSELLA_FILAMENT_INCLUDE');
    final fluoriteInclude = _dir(input, 'fluorite_include', 'TESSELLA_FLUORITE_INCLUDE');
    final profile = _string(input, 'cargo_profile', 'TESSELLA_CARGO_PROFILE') ?? 'release';

    if (filamentInclude == null || fluoriteInclude == null) {
      throw StateError(
        'set filament_include and fluorite_include under '
        'hooks: user_defines: tessella_fluorite: in the app pubspec',
      );
    }
    if (!File('$tessellaDir/Cargo.toml').existsSync()) {
      throw StateError('no tessella checkout at $tessellaDir (set tessella_dir)');
    }

    // The producer. A staticlib with no C++ runtime of its own, which is why
    // this whole link can be clang + libc++ without a second standard library.
    await _run('cargo', [
      'build',
      '-p',
      'tessella-ffi',
      if (profile == 'release') '--release',
    ], workingDirectory: tessellaDir);
    final tessellaLib = '$tessellaDir/target/$profile/libtessella_ffi.a';
    if (!File(tessellaLib).existsSync()) {
      throw StateError('cargo produced no $tessellaLib');
    }

    final buildDir = '${input.outputDirectory.toFilePath()}native';
    await _run('cmake', [
      '-S', packageRoot,
      '-B', buildDir,
      '-DCMAKE_BUILD_TYPE=${profile == 'release' ? 'Release' : 'Debug'}',
      '-DTESSELLA_DIR=$tessellaDir',
      '-DTESSELLA_LIB=$tessellaLib',
      '-DFILAMENT_INCLUDE_DIR=$filamentInclude',
      '-DFLUORITE_INCLUDE_DIR=$fluoriteInclude',
      if (await _which('ninja')) ...['-G', 'Ninja'],
    ]);
    await _run('cmake', ['--build', buildDir, '--parallel']);

    final library = File('$buildDir/libtessella_fluorite.so');
    if (!library.existsSync()) {
      throw StateError('no libtessella_fluorite.so at ${library.path}');
    }

    output.assets.code.add(
      CodeAsset(
        package: input.packageName,
        name: 'src/ffi.dart',
        linkMode: DynamicLoadingBundled(),
        file: library.uri,
      ),
    );

    // Re-run when any of the native half changes.
    for (final directory in ['native/src', 'native/include']) {
      final handle = Directory('$packageRoot$directory');
      if (!handle.existsSync()) continue;
      for (final entity in handle.listSync(recursive: true)) {
        if (entity is File && _isSource(entity.path)) {
          output.dependencies.add(entity.uri);
        }
      }
    }
    output.dependencies.add(Uri.file('${packageRoot}CMakeLists.txt'));
  });
}

bool _isSource(String path) =>
    path.endsWith('.cc') || path.endsWith('.h') || path.endsWith('.cpp');

/// A string knob: the user-define if the app set one, else the environment.
String? _string(BuildInput input, String define, String envVar) {
  final defined = input.userDefines[define];
  if (defined != null) return '$defined';
  final value = Platform.environment[envVar];
  return (value == null || value.isEmpty) ? null : value;
}

/// [_string] resolved against the package root, since the hook protocol does
/// not specify a working directory.
String? _dir(BuildInput input, String define, String envVar) {
  final value = _string(input, define, envVar);
  if (value == null) return null;
  return value.startsWith('/') ? value : input.packageRoot.resolve(value).toFilePath();
}

Future<bool> _which(String executable) async {
  final result = await Process.run('sh', ['-c', 'command -v $executable']);
  return result.exitCode == 0;
}

Future<void> _run(String executable, List<String> args, {String? workingDirectory}) async {
  final process = await Process.start(
    executable,
    args,
    workingDirectory: workingDirectory,
    mode: ProcessStartMode.inheritStdio,
  );
  final code = await process.exitCode;
  if (code != 0) {
    throw ProcessException(executable, args, 'exit code $code', code);
  }
}
