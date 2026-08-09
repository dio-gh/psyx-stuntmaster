#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace stuntmaster::core {
class StateReader;
class StateWriter;
}

namespace stuntmaster::psx {

struct GpuImageUpload {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct GpuVramCopy {
    std::uint32_t source_x{};
    std::uint32_t source_y{};
    std::uint32_t destination_x{};
    std::uint32_t destination_y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

// Stateful decoder for the 32-bit GP0 stream after DMA traversal. It preserves
// PS1 VRAM upload semantics and packet boundaries but does not render.
class GpuCommandDecoder final {
public:
    static constexpr std::uint32_t vram_width = 1024U;
    static constexpr std::uint32_t vram_height = 512U;
    using PacketSink = std::function<void(std::span<const std::uint32_t>)>;
    // Fired once, in stream order, when a CPU-to-VRAM upload finishes consuming
    // its pixels. The span is the finished rectangle in row order. This is the
    // upload pixel payload the packet stream lacks: `PacketSink` forwards only
    // the 3-word `0xA0` header, while the pixels are consumed separately into
    // VRAM. The persistent-framebuffer presenter needs both, interleaved in
    // order, so it can composite a screen retail *writes* into the same
    // framebuffer geometry is drawn into. See docs/PRESENTATION_REDESIGN.md.
    using UploadSink =
        std::function<void(const GpuImageUpload&, std::span<const std::uint16_t>)>;

    explicit GpuCommandDecoder(PacketSink packet_sink = {});

    void pushGp0(std::uint32_t word);

    // Install the upload payload sink. Off by default; only the
    // persistent-framebuffer path needs it, and building the row-major payload
    // copy has a cost the reconstruct path should not pay.
    void setUploadSink(UploadSink upload_sink) {
        upload_sink_ = std::move(upload_sink);
    }

    // Whether to apply a VRAM-to-VRAM copy whose source is entirely
    // upload-backed. Off by default: this half of the framebuffer composite is
    // opt-in until the pair is validated together, because a copy that moves
    // the wrong bytes is exactly what once corrupted the level-one torii.
    void setApplyVramCopies(bool enable) noexcept {
        apply_vram_copies_ = enable;
    }

    [[nodiscard]] std::uint64_t packetCount() const noexcept {
        return packet_count_;
    }
    [[nodiscard]] std::uint64_t primitiveCount() const noexcept {
        return primitive_count_;
    }
    [[nodiscard]] std::uint64_t environmentCount() const noexcept {
        return environment_count_;
    }
    [[nodiscard]] std::uint64_t imageUploadCount() const noexcept {
        return image_upload_count_;
    }
    [[nodiscard]] std::uint64_t imagePixelCount() const noexcept {
        return image_pixel_count_;
    }
    [[nodiscard]] std::uint64_t vramCopyCount() const noexcept {
        return vram_copy_count_;
    }
    // Copies whose source was entirely upload-backed and were therefore
    // applied. The difference against `vramCopyCount` is the copies refused
    // for reading pixels this decoder never saw written.
    [[nodiscard]] std::uint64_t vramCopiesApplied() const noexcept {
        return vram_copies_applied_;
    }
    [[nodiscard]] std::uint64_t vramRevision() const noexcept {
        return vram_revision_;
    }
    // Which 8x8 VRAM blocks arrived by CPU transfer, or by a copy out of a
    // region that had. This decoder never rasterizes primitives, so anything
    // the guest builds on the GPU leaves its region uncovered here and samples
    // stale bytes. Coverage is the direct test for that: a textured primitive
    // reading an uncovered region cannot be rendered correctly no matter what
    // the presenter does, and a VRAM copy reading one must be refused.
    static constexpr std::uint32_t coverage_block = 8U;
    static constexpr std::uint32_t coverage_pitch =
        vram_width / coverage_block;
    [[nodiscard]] bool uploadCovers(
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) const noexcept;

    // True while a CPU-to-VRAM transfer is still consuming words. Any GP0
    // written in this state is taken as pixel data, so a host-injected
    // callback that touches GP0 here corrupts the texture being uploaded.
    [[nodiscard]] bool imageTransferInProgress() const noexcept {
        return image_pixels_remaining_ != 0U;
    }
    // The largest single CPU-to-VRAM transfer seen. A runaway upload floods
    // the texture snapshot with whatever words follow it, so its size is the
    // signature to watch when textures turn to noise.
    [[nodiscard]] const GpuImageUpload& largestImageUpload() const noexcept {
        return largest_image_upload_;
    }
    [[nodiscard]] const GpuImageUpload& lastImageUpload() const noexcept {
        return last_image_upload_;
    }
    [[nodiscard]] const GpuVramCopy& lastVramCopy() const noexcept {
        return last_vram_copy_;
    }
    [[nodiscard]] bool awaitingPacketWords() const noexcept {
        return !packet_.empty() || polyline_;
    }
    [[nodiscard]] bool awaitingImageData() const noexcept {
        return image_pixels_remaining_ != 0U;
    }
    [[nodiscard]] std::span<const std::uint16_t> vram() const noexcept {
        return vram_;
    }
    void writeState(core::StateWriter& writer) const;
    [[nodiscard]] bool readState(core::StateReader& reader);

private:
    [[nodiscard]] static std::uint32_t fixedPacketWords(
        std::uint8_t opcode) noexcept;
    [[nodiscard]] static bool isPolyline(std::uint8_t opcode) noexcept;
    [[nodiscard]] static bool isPolylineTerminator(
        std::uint32_t word) noexcept;
    void finishPacket();
    void beginImageUpload();
    void writeImagePixel(std::uint16_t pixel) noexcept;
    void copyVramRectangle();
    void markUploadCoverage(
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) noexcept;

    PacketSink packet_sink_;
    UploadSink upload_sink_;
    // Row-major scratch for the upload payload handed to `upload_sink_`, reused
    // across uploads so a busy load does not reallocate per transfer.
    std::vector<std::uint16_t> upload_payload_;
    std::vector<std::uint32_t> packet_;
    std::vector<std::uint16_t> vram_;
    std::uint32_t expected_packet_words_{};
    std::uint32_t polyline_minimum_words_{};
    std::uint32_t image_x_{};
    std::uint32_t image_y_{};
    std::uint32_t image_width_{};
    std::uint32_t image_height_{};
    std::uint32_t image_pixel_index_{};
    std::uint32_t image_pixels_remaining_{};
    std::uint64_t packet_count_{};
    std::uint64_t primitive_count_{};
    std::uint64_t environment_count_{};
    std::uint64_t image_upload_count_{};
    std::uint64_t image_pixel_count_{};
    std::uint64_t vram_copy_count_{};
    std::uint64_t vram_copies_applied_{};
    bool apply_vram_copies_{};
    std::uint64_t vram_revision_{};
    GpuImageUpload last_image_upload_{};
    GpuImageUpload largest_image_upload_{};
    std::vector<std::uint8_t> upload_coverage_ =
        std::vector<std::uint8_t>(
            static_cast<std::size_t>(coverage_pitch) *
                (vram_height / coverage_block),
            0U);
    GpuVramCopy last_vram_copy_{};
    bool polyline_{};
};

} // namespace stuntmaster::psx
