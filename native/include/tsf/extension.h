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

/// One slot's frame statistics, for a HUD.
///
/// Everything a pane can say about itself without asking Filament: what the
/// producer and the consumer each cost on the last tick, what they have cost
/// recently, how much is still in flight, and how much memory the two regions
/// hold. Sampled rather than streamed -- a HUD reads this a couple of times a
/// second, and a callback per frame would cost more than the thing it measures.
typedef struct tessella_fluorite_stats {
    /// Frames this slot has been ticked.
    uint64_t frames;
    /// Frames per second, smoothed over the recent window.
    double fps;
    /// The last tick, split at the FFI boundary: `produce` is tessella's frame
    /// (cover, layout, placement), `drain` is walking the ring into Filament.
    double produce_ms;
    double drain_ms;
    /// The worst of the last 120 ticks, which is what says whether a pane is
    /// dropping frames rather than merely slow on average.
    double produce_ms_max;
    double drain_ms_max;
    /// Tiles asked for and not yet answered, plus an unfinished glyph fetch.
    uint64_t pending;
    /// Records read since the map was created, and primitives now in the scene.
    uint64_t records;
    uint64_t primitives;
    /// The slab region: bytes the bump cursor has reached, and bytes the table
    /// still claims. The gap between them is what compaction has yet to take.
    double slab_mib;
    double slab_live_mib;
    /// The most the ring has ever held unread.
    double ring_peak_mib;
    /// Where this slot is looking, as last applied. Degrees for bearing and
    /// pitch. A HUD wants the zoom beside the frame cost, because what a pane
    /// costs is mostly a function of how much world is on it.
    double latitude;
    double longitude;
    double zoom;
    double bearing;
    double pitch;
    /// `tessella_readiness`, or -1 when nothing is attached.
    int32_t readiness;
} tessella_fluorite_stats;

/// Fills `out` for slot `slot`. Returns 0, or -1 when nothing is attached or
/// `out` is null.
TSF_API int32_t tessella_fluorite_stats_of(uint32_t slot, tessella_fluorite_stats* out);

/// This slot's readiness, and its reason into `reason` when there is room.
/// Returns the `tessella_readiness` value, or -1 when nothing is attached.
TSF_API int32_t tessella_fluorite_readiness(uint32_t slot, char* reason, int32_t reason_capacity);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // TSF_EXTENSION_H
