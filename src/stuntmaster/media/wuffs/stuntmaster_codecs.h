#pragma once

// Wuffs generates header-style C. Each codec is a separate translation unit,
// but C++ callers need the declarations from all four files. They deliberately
// share one Wuffs package namespace, so release the per-package include guard
// between includes while the Wuffs base guard remains intact.
#include "stuntmaster/media/wuffs/generated/stuntmaster_psx.c"
#undef WUFFS_INCLUDE_GUARD__STUNTMASTER_PSX
#include "stuntmaster/media/wuffs/generated/stuntmaster_str.c"
#undef WUFFS_INCLUDE_GUARD__STUNTMASTER_PSX
#include "stuntmaster/media/wuffs/generated/stuntmaster_xa.c"
#undef WUFFS_INCLUDE_GUARD__STUNTMASTER_PSX
#include "stuntmaster/media/wuffs/generated/stuntmaster_mdec.c"
