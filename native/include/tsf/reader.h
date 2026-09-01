// SPDX-License-Identifier: Apache-2.0
//
// Walks tessella's capture ring and drives a `tsf::FrameSink`.

#ifndef TSF_READER_H
#define TSF_READER_H

#include <tsf/frame.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace tsf {

/// A byte range the producer owns and this process maps.
struct Region {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

/// Reads records off the ring and calls a sink.
///
/// # What it is responsible for, and what it is not
///
/// It joins: a `tsl_geometry_add` and each `tsl_view_use` naming it become one `DrawableAdd`,
/// which is the shape the Filament half was written against. It resolves: a slab reference
/// becomes borrowed bytes in the mapped region. And it brackets: a frame's records are delivered
/// between `beginFrame` and `endFrame`.
///
/// It does not own, copy, or outlive anything. Every `Bytes` handed to the sink points into the
/// producer's region and is valid for the duration of the call; a sink that needs them longer --
/// which one uploading to a GPU does -- copies them or holds the slab, and holding is what
/// §11.7 asks for.
class Reader {
public:
    Reader(Region ring, Region slabs) noexcept
        : ring_(ring), slabs_(slabs) {}

    /// Drains everything the producer has published, calling `sink`.
    ///
    /// Returns the number of records consumed. Advancing the ring's tail is the caller's, and
    /// deliberately: the bytes a pass consumed are the producer's to reuse only once the sink is
    /// done with them, and the sink is what knows that.
    std::size_t drain(FrameSink& sink);

    /// Where the reader has read to, for the caller to publish as the tail.
    [[nodiscard]] std::uint64_t cursor() const noexcept { return cursor_; }

    /// Records seen whose kind this build does not know.
    ///
    /// Not an error: the ABI is revisioned and a newer producer may send a kind this consumer
    /// predates. Counted rather than ignored, because a stream that is *mostly* unknown records
    /// is a version mismatch presenting as a blank map.
    [[nodiscard]] std::uint64_t unknownRecords() const noexcept { return unknown_; }

    /// Resolves a slab reference into borrowed bytes, or an empty range.
    [[nodiscard]] Bytes resolve(tsl_slab_ref ref) const noexcept;

    /// Points the reader at the producer's ranges again, keeping everything it has read.
    ///
    /// The slab range moves: the producer repacks its table after each frame that allocates, so
    /// the pointer a consumer was given last frame is not the one to resolve against this frame.
    /// Safe to do mid-stream because nothing the reader remembers points into either range --
    /// a geometry announcement's spans are copied out when it arrives, precisely so that a view
    /// using it many frames later does not read bytes the producer has since reused.
    void rebind(Region ring, Region slabs) noexcept {
        ring_ = ring;
        slabs_ = slabs;
    }

private:
    void dispatch(const tsl_record_header& header,
                  const std::uint8_t* fixed,
                  const std::uint8_t* payload,
                  std::uint32_t payloadLen,
                  FrameSink& sink);

    /// Reads a span of `T` out of a record's payload, bounded.
    template <typename T>
    std::vector<T> span(const std::uint8_t* payload, std::uint32_t payloadLen, tsl_span at) const;

    Region ring_;
    Region slabs_;
    std::uint64_t cursor_ = 0;
    std::uint64_t unknown_ = 0;
    bool inFrame_ = false;

    /// A geometry announcement with its spans read out.
    ///
    /// The record's spans point into the *ring*, which the producer reuses as soon as the tail
    /// advances past them. A view may use this geometry many frames later, so the lists are
    /// copied when the announcement arrives rather than read when the use does.
    struct Geometry {
        tsl_geometry_add record{};
        /// Ring position just past this announcement, carried onto every drawable joined from
        /// it so the consumer can acknowledge the upload against it.
        std::uint64_t announcedAt = 0;
        std::vector<tsl_attribute_desc> attrs;
        std::vector<tsl_attribute_desc> instanceAttrs;
        std::vector<tsl_segment> segments;
        std::vector<tsl_texture_ref> textureRefs;
    };

    /// The shared half of every drawable, until a view uses it.
    ///
    /// Kept across frames, not per frame: geometry is announced once and used by any number of
    /// views over any number of frames, which is the whole point of the split. Retired on
    /// `tsl_geometry_remove`.
    std::unordered_map<std::uint64_t, Geometry> geometry_;

    /// The order awaiting the camera that commits it.
    FrameOrder pending_;
    bool havePending_ = false;
};

} // namespace tsf

#endif // TSF_READER_H
