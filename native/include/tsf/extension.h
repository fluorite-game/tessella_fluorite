// SPDX-License-Identifier: Apache-2.0
//
// The C surface Dart calls to put a map in every Fluorite platform view.
//
// One style, N views. `tessella_fluorite_configure` names the style and the
// material directory; `tessella_fluorite_install` registers a
// `FluoriteViewExtension` with fluorite, and from then on every platform view
// that comes up gets a `tsf::MapView` on its own Filament layer, ticked on that
// view's frame path. Nothing here creates a view or an engine -- Fluorite owns
// both, and this is a guest in the scene it already has.
//
// Cameras are per slot and are held whether or not the slot has attached yet,
// so Dart can set a camera before the platform view exists. Slot order is view
// creation order, which is what `FluoriteView` hands out.
//
// Thread safety: Dart calls these from the Dart thread while fluorite calls
// attach, frame and detach from the Filament thread. Everything shared is
// behind one lock, and the Filament-side calls are the only ones that touch a
// `MapView`.

#ifndef TSF_EXTENSION_H
#define TSF_EXTENSION_H

#include <stdint.h>

#ifdef _WIN32
#define TSF_API __declspec(dllexport)
#else
#define TSF_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Names the style and the compiled materials every map will use. Copied.
/// Returns 0, or -1 for a null argument.
///
/// Call before `tessella_fluorite_install`. Calling it after replaces what the
/// *next* view gets; views already attached keep the style they were built
/// with, because rebuilding a map under a live scene would drop its tiles.
TSF_API int32_t tessella_fluorite_configure(const char* style_json, const char* material_dir);

/// Registers the extension with fluorite. Idempotent. Returns 0, or -1 when
/// `tessella_fluorite_configure` has not been called.
TSF_API int32_t tessella_fluorite_install(void);

/// Unregisters it and tears down every live map. Safe to call twice.
TSF_API void tessella_fluorite_uninstall(void);

/// Where slot `slot` looks. Degrees for bearing and pitch. Applied on that
/// view's next frame; held until then, and held across an attach, so a camera
/// set before the view exists is the one it comes up at.
TSF_API void tessella_fluorite_set_camera(uint32_t slot,
                                          double latitude,
                                          double longitude,
                                          double zoom,
                                          double bearing,
                                          double pitch);

/// Tiles asked for and not yet answered on this slot, plus an unfinished glyph
/// fetch. Zero with a live map means it has settled; zero with no map means
/// there is nothing there.
TSF_API uint64_t tessella_fluorite_pending(uint32_t slot);

/// Whether slot `slot` has a map attached.
TSF_API int32_t tessella_fluorite_attached(uint32_t slot);

/// This slot's readiness, and its reason into `reason` when there is room.
/// Returns the `tessella_readiness` value, or -1 when nothing is attached.
TSF_API int32_t tessella_fluorite_readiness(uint32_t slot, char* reason, int32_t reason_capacity);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // TSF_EXTENSION_H
