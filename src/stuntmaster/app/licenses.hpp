#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace stuntmaster::app {

// One license text embedded in the executable's resources.
struct EmbeddedLicense {
    std::string name;        // display name, e.g. "Wuffs (Apache 2.0)"
    std::string_view text;   // resource bytes; valid for the whole process
};

// The license texts compiled into stuntmaster.exe (project + third-party),
// resolved from RCDATA resources. Empty on non-Windows or if a resource is
// missing. The in-game license viewer renders these.
[[nodiscard]] std::vector<EmbeddedLicense> embeddedLicenses();

// The default input.ini contents embedded in the executable (from
// input.example.ini). Empty if unavailable. Used to seed the user's editable
// input.ini on first run.
[[nodiscard]] std::string_view embeddedDefaultInputConfig();

} // namespace stuntmaster::app
