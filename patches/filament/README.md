<!-- SPDX-License-Identifier: BSD-2-Clause -->

# Filament patches

Changes this consumer needs that are not in a released Filament, each carried only until the
upstream change it mirrors is merged and a release containing it is the one we link.

The Filament a parity run links is named by `FILAMENT_STAGING` in tessella's `tools/parity/env.sh`.
Nothing here is applied automatically: the staging directory is built elsewhere, and these are the
diffs that build has to carry.

Apply to a 1.75.0 source tree with:

    git apply --directory=. patches/filament/0001-fix-vulkan-depth-target-format-queries.patch

Then build and install it, and point `FILAMENT_STAGING` at the result.

## 0001-fix-vulkan-depth-target-format-queries

Upstream [PR #10424](https://github.com/google/filament/pull/10424) for
[issue #10418](https://github.com/google/filament/issues/10418), rebased onto 1.75.0. Open upstream
as of 2026-09-15.

`VulkanDriver::isRenderTargetFormatSupported` tested `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT`, which
no depth or stencil format carries, so every one of them answered unsupported whatever the device
reports. The caller choosing a depth/stencil format has no other way to ask and allocated
`DEPTH32F_STENCIL8` regardless. A device without it -- the Raspberry Pi 5's V3D reports
`optimalTilingFeatures` of zero for `VK_FORMAT_D32_SFLOAT_S8_UINT` and `0xce01` for
`VK_FORMAT_D24_UNORM_S8_UINT` -- segfaults inside `vkCreateImageView` on the first attachment view
over that image.

A desktop GPU that supports `DEPTH32F_STENCIL8` never sees it, which is why the parity set did not:
the stencil buffer the map's view asks for is allocated in the format Filament assumed, the masks
work, and every stencil number measured on this machine stands. It is the Pi that crashes.

`verify-depth-stencil-query.c` beside it asks the device what the patch decides, so the fix can be
shown rather than argued from the diff:

    cc -O1 -o verify-depth-stencil-query verify-depth-stencil-query.c -lvulkan
    ./verify-depth-stencil-query

On an AMD RADV desktop (RAPHAEL_MENDOCINO) the old test is wrong about both formats the device
really supports, and the new one agrees with the driver on all four:

    DEPTH32F_STENCIL8   0x0001d601   old=false  new=true
    DEPTH24_STENCIL8    0x00000000   old=false  new=false
    STENCIL8            0x0001ce01   old=false  new=true
    RGBA8 (control)     0x0001dd83   old=true   new=false

That machine is the mirror of the Pi -- it has `DEPTH32F_STENCIL8` and not `DEPTH24_STENCIL8` --
which is why the hardcoded choice happened to work there and killed the Pi.
