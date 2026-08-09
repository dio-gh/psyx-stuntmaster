#include "stuntmaster/psx/gpu_command_decoder.hpp"

#include "stuntmaster/core/state_archive.hpp"

#include <algorithm>
#include <utility>

namespace stuntmaster::psx {
namespace {

constexpr std::uint8_t image_upload_opcode = 0xA0U;

} // namespace

GpuCommandDecoder::GpuCommandDecoder(PacketSink packet_sink)
    : packet_sink_(std::move(packet_sink)),
      vram_(vram_width * vram_height) {
    packet_.reserve(16U);
}

void GpuCommandDecoder::writeState(core::StateWriter& writer) const {
    writer.vectorPod(upload_payload_);
    writer.vectorPod(packet_);
    writer.vectorPod(vram_);
    writer.pod(expected_packet_words_);
    writer.pod(polyline_minimum_words_);
    writer.pod(image_x_);
    writer.pod(image_y_);
    writer.pod(image_width_);
    writer.pod(image_height_);
    writer.pod(image_pixel_index_);
    writer.pod(image_pixels_remaining_);
    writer.pod(packet_count_);
    writer.pod(primitive_count_);
    writer.pod(environment_count_);
    writer.pod(image_upload_count_);
    writer.pod(image_pixel_count_);
    writer.pod(vram_copy_count_);
    writer.pod(vram_copies_applied_);
    writer.pod(apply_vram_copies_);
    writer.pod(vram_revision_);
    writer.pod(last_image_upload_);
    writer.pod(largest_image_upload_);
    writer.vectorPod(upload_coverage_);
    writer.pod(last_vram_copy_);
    writer.pod(polyline_);
}

bool GpuCommandDecoder::readState(core::StateReader& reader) {
    constexpr auto vram_pixels =
        static_cast<std::uint64_t>(vram_width) * vram_height;
    if (!reader.vectorPod(upload_payload_, vram_pixels) ||
        !reader.vectorPod(packet_, 1U << 20U) ||
        !reader.vectorPod(vram_, vram_pixels) ||
        !reader.pod(expected_packet_words_) ||
        !reader.pod(polyline_minimum_words_) || !reader.pod(image_x_) ||
        !reader.pod(image_y_) || !reader.pod(image_width_) ||
        !reader.pod(image_height_) || !reader.pod(image_pixel_index_) ||
        !reader.pod(image_pixels_remaining_) || !reader.pod(packet_count_) ||
        !reader.pod(primitive_count_) || !reader.pod(environment_count_) ||
        !reader.pod(image_upload_count_) || !reader.pod(image_pixel_count_) ||
        !reader.pod(vram_copy_count_) ||
        !reader.pod(vram_copies_applied_) ||
        !reader.pod(apply_vram_copies_) ||
        !reader.pod(vram_revision_) || !reader.pod(last_image_upload_) ||
        !reader.pod(largest_image_upload_) ||
        !reader.vectorPod(upload_coverage_,
            static_cast<std::uint64_t>(coverage_pitch) *
                (vram_height / coverage_block)) ||
        !reader.pod(last_vram_copy_) || !reader.pod(polyline_)) {
        return false;
    }
    return vram_.size() == vram_pixels &&
        upload_coverage_.size() ==
            static_cast<std::size_t>(coverage_pitch) *
                (vram_height / coverage_block) &&
        image_pixels_remaining_ <=
            static_cast<std::uint64_t>(image_width_) * image_height_;
}

std::uint32_t GpuCommandDecoder::fixedPacketWords(
    std::uint8_t opcode) noexcept {
    // Fill Rectangle is three words: colour, top-left, extent. Sizing it as one
    // dropped retail's per-frame framebuffer clear and re-read its two
    // parameter words as commands of their own. Everything downstream then saw
    // a page nobody had cleared, so wherever a frame did not cover every pixel
    // — behind transparent texels, in a region the backdrop should own — the
    // previous frame stayed on screen. That is one missing table entry behind
    // the stale backdrop, the flashing, and the HUD sitting on old pixels, and
    // it sat below both renderers, which is why replacing one changed nothing.
    if (opcode == 0x02U) {
        return 3U;
    }
    if (opcode >= 0x20U && opcode <= 0x3FU) {
        const auto vertices = (opcode & 0x08U) != 0U ? 4U : 3U;
        const auto textured = (opcode & 0x04U) != 0U;
        const auto gouraud = (opcode & 0x10U) != 0U;
        return 1U + vertices * (1U + (textured ? 1U : 0U)) +
            (gouraud ? vertices - 1U : 0U);
    }
    if (opcode >= 0x40U && opcode <= 0x5FU) {
        if (isPolyline(opcode)) {
            return 0U;
        }
        return (opcode & 0x10U) != 0U ? 4U : 3U;
    }
    if (opcode >= 0x60U && opcode <= 0x7FU) {
        const auto textured = (opcode & 0x04U) != 0U;
        const auto variable_size = (opcode & 0x18U) == 0U;
        return 2U + (textured ? 1U : 0U) +
            (variable_size ? 1U : 0U);
    }
    if (opcode >= 0x80U && opcode <= 0x9FU) {
        return 4U;
    }
    if (opcode >= 0xA0U && opcode <= 0xDFU) {
        return 3U;
    }
    return 1U;
}

bool GpuCommandDecoder::isPolyline(std::uint8_t opcode) noexcept {
    return opcode >= 0x40U && opcode <= 0x5FU &&
        (opcode & 0x08U) != 0U;
}

bool GpuCommandDecoder::isPolylineTerminator(
    std::uint32_t word) noexcept {
    return (word & 0xF000F000U) == 0x50005000U;
}

void GpuCommandDecoder::pushGp0(std::uint32_t word) {
    if (image_pixels_remaining_ != 0U) {
        writeImagePixel(static_cast<std::uint16_t>(word));
        if (image_pixels_remaining_ != 0U) {
            writeImagePixel(static_cast<std::uint16_t>(word >> 16U));
        }
        return;
    }

    if (polyline_) {
        if (packet_.size() >= polyline_minimum_words_ &&
            isPolylineTerminator(word)) {
            finishPacket();
            return;
        }
        packet_.push_back(word);
        return;
    }

    if (packet_.empty()) {
        const auto opcode = static_cast<std::uint8_t>(word >> 24U);
        expected_packet_words_ = fixedPacketWords(opcode);
        polyline_ = isPolyline(opcode);
        polyline_minimum_words_ =
            (opcode & 0x10U) != 0U ? 4U : 3U;
    }
    packet_.push_back(word);
    if (!polyline_ && packet_.size() == expected_packet_words_) {
        finishPacket();
    }
}

void GpuCommandDecoder::finishPacket() {
    if (packet_.empty()) {
        return;
    }
    const auto opcode = static_cast<std::uint8_t>(packet_.front() >> 24U);
    ++packet_count_;
    if (opcode >= 0x20U && opcode <= 0x7FU) {
        ++primitive_count_;
    } else if (opcode >= 0xE0U) {
        ++environment_count_;
    }
    if (packet_sink_) {
        packet_sink_(packet_);
    }
    if (opcode == image_upload_opcode) {
        beginImageUpload();
    } else if (opcode >= 0x80U && opcode <= 0x9FU) {
        copyVramRectangle();
    }
    packet_.clear();
    expected_packet_words_ = 0U;
    polyline_minimum_words_ = 0U;
    polyline_ = false;
}

void GpuCommandDecoder::beginImageUpload() {
    if (packet_.size() != 3U) {
        return;
    }
    image_x_ = packet_[1] & 0x3FFU;
    image_y_ = (packet_[1] >> 16U) & 0x1FFU;
    const auto raw_width = packet_[2] & 0xFFFFU;
    const auto raw_height = packet_[2] >> 16U;
    image_width_ = ((raw_width - 1U) & 0x3FFU) + 1U;
    image_height_ = ((raw_height - 1U) & 0x1FFU) + 1U;
    last_image_upload_ =
        {image_x_, image_y_, image_width_, image_height_};
    if (static_cast<std::uint64_t>(image_width_) * image_height_ >
        static_cast<std::uint64_t>(largest_image_upload_.width) *
            largest_image_upload_.height) {
        largest_image_upload_ = last_image_upload_;
    }
    image_pixel_index_ = 0U;
    image_pixels_remaining_ = image_width_ * image_height_;
    ++image_upload_count_;

    markUploadCoverage(image_x_, image_y_, image_width_, image_height_);
}

void GpuCommandDecoder::markUploadCoverage(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    if (width == 0U || height == 0U) {
        return;
    }
    const auto block_x1 = (x + width - 1U) / coverage_block;
    const auto block_y1 = (y + height - 1U) / coverage_block;
    for (auto block_y = y / coverage_block;
         block_y <= block_y1 && block_y < vram_height / coverage_block;
         ++block_y) {
        for (auto block_x = x / coverage_block;
             block_x <= block_x1 && block_x < coverage_pitch;
             ++block_x) {
            upload_coverage_[block_y * coverage_pitch + block_x] = 1U;
        }
    }
}

bool GpuCommandDecoder::uploadCovers(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height) const noexcept {
    if (width == 0U || height == 0U) {
        return true;
    }
    const auto block_x1 = (x + width - 1U) / coverage_block;
    const auto block_y1 = (y + height - 1U) / coverage_block;
    for (auto block_y = y / coverage_block;
         block_y <= block_y1 && block_y < vram_height / coverage_block;
         ++block_y) {
        for (auto block_x = x / coverage_block;
             block_x <= block_x1 && block_x < coverage_pitch;
             ++block_x) {
            if (upload_coverage_[block_y * coverage_pitch + block_x] == 0U) {
                return false;
            }
        }
    }
    return true;
}

void GpuCommandDecoder::writeImagePixel(std::uint16_t pixel) noexcept {
    if (image_pixels_remaining_ == 0U) {
        return;
    }
    const auto x =
        (image_x_ + image_pixel_index_ % image_width_) & (vram_width - 1U);
    const auto y =
        (image_y_ + image_pixel_index_ / image_width_) & (vram_height - 1U);
    vram_[y * vram_width + x] = pixel;
    ++image_pixel_index_;
    --image_pixels_remaining_;
    ++image_pixel_count_;
    if (image_pixels_remaining_ == 0U) {
        ++vram_revision_;
        if (upload_sink_) {
            // Lift the finished rectangle out of VRAM in row order, applying
            // the same wrap the writes used, so the payload is exactly the
            // pixels this upload placed. Reuses the scratch buffer.
            upload_payload_.resize(
                static_cast<std::size_t>(image_width_) * image_height_);
            std::size_t index = 0U;
            for (std::uint32_t row = 0U; row < image_height_; ++row) {
                const auto y = (image_y_ + row) & (vram_height - 1U);
                for (std::uint32_t column = 0U; column < image_width_;
                     ++column) {
                    const auto x = (image_x_ + column) & (vram_width - 1U);
                    upload_payload_[index++] = vram_[y * vram_width + x];
                }
            }
            upload_sink_(last_image_upload_, upload_payload_);
        }
    }
}

void GpuCommandDecoder::copyVramRectangle() {
    if (packet_.size() != 4U) {
        return;
    }
    const auto source_x = packet_[1] & 0x3FFU;
    const auto source_y = (packet_[1] >> 16U) & 0x1FFU;
    const auto destination_x = packet_[2] & 0x3FFU;
    const auto destination_y = (packet_[2] >> 16U) & 0x1FFU;
    const auto width =
        ((packet_[3] & 0xFFFFU) - 1U) % vram_width + 1U;
    const auto height =
        ((packet_[3] >> 16U) - 1U) % vram_height + 1U;
    last_vram_copy_ =
        {source_x,
         source_y,
         destination_x,
         destination_y,
         width,
         height};
    ++vram_copy_count_;

    // This decoder tracks CPU uploads but does not rasterize GP0 primitives
    // into vram_, so a MoveImage reading a region the guest rendered would
    // copy stale bytes over live assets. That is not hypothetical: one such
    // copy overwrote the level-one torii's facade tile.
    //
    // Upload coverage is the exact test for it. A source region every pixel of
    // which arrived by CPU transfer holds bytes this decoder knows, so moving
    // them is faithful; a source with any uncovered pixel is refused, which is
    // the case that caused the regression. Retail needs this for the loading
    // screen, where the background is uploaded into one display page and
    // MoveImage'd into the other so both buffers carry it.
    //
    if (!apply_vram_copies_ ||
        !uploadCovers(source_x, source_y, width, height)) {
        return;
    }
    for (std::uint32_t row = 0U; row < height; ++row) {
        const auto source_row = source_y + row;
        const auto destination_row = destination_y + row;
        if (source_row >= vram_height || destination_row >= vram_height) {
            break;
        }
        for (std::uint32_t column = 0U; column < width; ++column) {
            const auto source_column = source_x + column;
            const auto destination_column = destination_x + column;
            if (source_column >= vram_width ||
                destination_column >= vram_width) {
                break;
            }
            vram_[static_cast<std::size_t>(destination_row) * vram_width +
                  destination_column] =
                vram_[static_cast<std::size_t>(source_row) * vram_width +
                      source_column];
        }
    }
    // The destination now holds bytes of known provenance, so a later copy may
    // read from it in turn.
    markUploadCoverage(destination_x, destination_y, width, height);
    ++vram_copies_applied_;
    ++vram_revision_;
}

} // namespace stuntmaster::psx
