#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__STUNTMASTER_PSX
#include "stuntmaster_psx.c"

#include <cstdlib>

int main() {
    auto* info = wuffs_stuntmaster_psx__build_info__alloc();
    if (info == nullptr) {
        return 1;
    }
    const auto version =
        wuffs_stuntmaster_psx__build_info__codec_abi_version(info);
    std::free(info);
    return version == WUFFS_STUNTMASTER_PSX__CODEC_ABI_VERSION ? 0 : 1;
}
