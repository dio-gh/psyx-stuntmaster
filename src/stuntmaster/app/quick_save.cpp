#include "quick_save.hpp"

#include "stuntmaster/core/error.hpp"
#include "stuntmaster/core/state_archive.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace stuntmaster::app {
namespace {

constexpr std::array<std::byte, 8U> quick_save_magic{
    std::byte{'S'}, std::byte{'T'}, std::byte{'N'}, std::byte{'T'},
    std::byte{'S'}, std::byte{'A'}, std::byte{'V'}, std::byte{'E'}};
constexpr std::uint64_t maximum_quick_save_bytes = 128U * 1024U * 1024U;

[[nodiscard]] std::string pathText(const std::filesystem::path& path) {
    return path.string();
}

void replaceFile(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(
            source.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw stuntmaster::core::Error{
            "could not replace quick save " + pathText(destination) +
            " (Windows error " + std::to_string(GetLastError()) + ")"};
    }
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) {
        throw stuntmaster::core::Error{
            "could not replace quick save " + pathText(destination) +
            ": " + error.message()};
    }
#endif
}

} // namespace

bool quickSaveSettingsCompatible(
    const QuickSaveCompatibility& saved,
    const QuickSaveCompatibility& current) noexcept {
    // The selected guest rate is carried by the save header and restored as a
    // runtime setting. It is deliberately not tied to the command line used
    // to launch this session.
    constexpr auto launch_constrained_flags =
        quick_save_flag_retime_motion |
        quick_save_flag_retime_clock |
        quick_save_flag_eager_high_rate;
    if (saved.guest_cpu_scale != current.guest_cpu_scale ||
        saved.cd_read_speed != current.cd_read_speed ||
        (saved.flags & launch_constrained_flags) !=
            (current.flags & launch_constrained_flags)) {
        return false;
    }
    return true;
}

std::filesystem::path defaultQuickSavePath(
    const std::filesystem::path& saves_dir) {
    return saves_dir / "quick-save.stsm";
}

std::filesystem::path timestampedQuickSavePath(
    const std::filesystem::path& saves_dir,
    std::chrono::system_clock::time_point now) {
    const auto milliseconds = std::chrono::duration_cast<
        std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream name;
    name << "quick-save-" << std::put_time(&local, "%Y%m%d-%H%M%S-")
         << std::setfill('0') << std::setw(3) << milliseconds.count()
         << ".stsm";
    return saves_dir / name.str();
}

void writeQuickSaveFile(
    const std::filesystem::path& destination,
    const stuntmaster::core::Sha256Digest& executable_hash,
    const QuickSaveCompatibility& compatibility,
    std::span<const std::byte> payload) {
    if (payload.size() > maximum_quick_save_bytes) {
        throw stuntmaster::core::Error{"quick save exceeds the 128 MiB limit"};
    }
    stuntmaster::core::StateWriter writer;
    writer.pod(quick_save_magic);
    writer.pod(quick_save_format_version);
    writer.pod(executable_hash);
    writer.pod(compatibility);
    writer.pod(static_cast<std::uint64_t>(payload.size()));
    writer.pod(stuntmaster::core::sha256(payload));
    writer.bytes(payload);

    auto parent = destination.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw stuntmaster::core::Error{
                "could not create quick-save directory " +
                pathText(parent) + ": " + error.message()};
        }
    }
    auto temporary = destination;
    temporary += ".tmp";
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
            throw stuntmaster::core::Error{
                "could not open quick save for writing: " +
                pathText(temporary)};
        }
        const auto& bytes = writer.data();
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output) {
            throw stuntmaster::core::Error{
                "could not write quick save: " + pathText(temporary)};
        }
    }
    try {
        replaceFile(temporary, destination);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

QuickSaveFile readQuickSaveFile(const std::filesystem::path& source) {
    std::ifstream input{source, std::ios::binary | std::ios::ate};
    if (!input) {
        throw stuntmaster::core::Error{
            "could not open quick save: " + pathText(source)};
    }
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) >
            maximum_quick_save_bytes + 4096U) {
        throw stuntmaster::core::Error{
            "quick save has an invalid size: " + pathText(source)};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        throw stuntmaster::core::Error{
            "could not read quick save: " + pathText(source)};
    }

    stuntmaster::core::StateReader reader{bytes};
    std::array<std::byte, 8U> magic{};
    std::uint32_t version{};
    QuickSaveFile result;
    std::uint64_t payload_size{};
    stuntmaster::core::Sha256Digest payload_hash{};
    if (!reader.pod(magic) || magic != quick_save_magic ||
        !reader.pod(version) || version != quick_save_format_version ||
        !reader.pod(result.executable_hash) ||
        !reader.pod(result.compatibility) || !reader.pod(payload_size) ||
        payload_size > maximum_quick_save_bytes ||
        !reader.pod(payload_hash) || payload_size != reader.remaining()) {
        throw stuntmaster::core::Error{
            "unsupported or corrupt quick save: " + pathText(source)};
    }
    result.payload.resize(static_cast<std::size_t>(payload_size));
    if (!reader.bytes(result.payload) || !reader.finished() ||
        stuntmaster::core::sha256(result.payload) != payload_hash) {
        throw stuntmaster::core::Error{
            "quick-save checksum failed: " + pathText(source)};
    }
    return result;
}

} // namespace stuntmaster::app
