#include "licenses.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "resource.h"
#endif

namespace stuntmaster::app {

#ifdef _WIN32

namespace {

struct LicenseEntry {
    int id;
    const char* name;
};

constexpr LicenseEntry license_entries[] = {
    {IDR_LICENSE_STUNTMASTER, "Stuntmaster PC (MIT)"},
    {IDR_LICENSE_THIRD_PARTY, "Third-party notices"},
    {IDR_LICENSE_FFMPEG_LGPL21, "FFmpeg (LGPL v2.1)"},
    {IDR_LICENSE_FFMPEG_LGPL3, "FFmpeg (LGPL v3)"},
    {IDR_LICENSE_FFMPEG_MAIN, "FFmpeg (LICENSE)"},
    {IDR_LICENSE_SDL2, "SDL2"},
    {IDR_LICENSE_OPENAL, "OpenAL Soft"},
    {IDR_LICENSE_FMT, "fmt"},
};

[[nodiscard]] std::string_view loadResourceText(int id) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(id), reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (resource == nullptr) {
        return {};
    }
    const DWORD size = SizeofResource(module, resource);
    const HGLOBAL handle = LoadResource(module, resource);
    if (handle == nullptr || size == 0U) {
        return {};
    }
    const void* data = LockResource(handle);
    if (data == nullptr) {
        return {};
    }
    return std::string_view{
        static_cast<const char*>(data), static_cast<std::size_t>(size)};
}

} // namespace

std::vector<EmbeddedLicense> embeddedLicenses() {
    std::vector<EmbeddedLicense> licenses;
    for (const auto& entry : license_entries) {
        const auto text = loadResourceText(entry.id);
        if (!text.empty()) {
            licenses.push_back({entry.name, text});
        }
    }
    return licenses;
}

std::string_view embeddedDefaultInputConfig() {
    return loadResourceText(IDR_INPUT_DEFAULT);
}

#else

std::vector<EmbeddedLicense> embeddedLicenses() { return {}; }

std::string_view embeddedDefaultInputConfig() { return {}; }

#endif

} // namespace stuntmaster::app
