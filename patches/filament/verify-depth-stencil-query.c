/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * What 0001-fix-vulkan-depth-target-format-queries.patch changes, asked of the device directly.
 *
 * `VulkanDriver::isRenderTargetFormatSupported` answers from `optimalTilingFeatures`. The
 * unpatched test is `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT`, which no depth or stencil format
 * carries, so it calls all of them unsupported; the patched test is
 * `VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT` for those formats. This prints what the driver
 * reports and what each test would answer, so the fix can be shown on whatever device is at hand
 * rather than argued from the diff.
 *
 * Run it where the bug bites. On an AMD RADV desktop (RAPHAEL_MENDOCINO, 2026-09-15):
 *
 *     DEPTH32F_STENCIL8   0x0001d601   old=false  new=true
 *     DEPTH24_STENCIL8    0x00000000   old=false  new=false
 *     STENCIL8            0x0001ce01   old=false  new=true
 *     RGBA8 (control)     0x0001dd83   old=true   new=false
 *
 * The old test is wrong about the two formats that device really supports, and the new one agrees
 * with the driver on all four -- including refusing DEPTH24_STENCIL8, which RADV does not have.
 * A Raspberry Pi 5's V3D reports the opposite pair, which is why it allocated a format it lacked
 * and died inside `vkCreateImageView`.
 *
 *     cc -O1 -o verify-depth-stencil-query verify-depth-stencil-query.c -lvulkan && ./verify-depth-stencil-query
 */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

static void report(VkPhysicalDevice device, VkFormat format, const char* name) {
    VkFormatProperties properties;
    memset(&properties, 0, sizeof properties);
    vkGetPhysicalDeviceFormatProperties(device, format, &properties);

    const int unpatched = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    const int patched = (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;

    printf("%-28s optimalTilingFeatures=0x%08x  old=%-5s  new=%s\n", name,
            properties.optimalTilingFeatures, unpatched ? "true" : "false",
            patched ? "true" : "false");
}

int main(void) {
    VkApplicationInfo application;
    memset(&application, 0, sizeof application);
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;

    VkInstance instance;
    if (vkCreateInstance(&info, NULL, &instance) != VK_SUCCESS) {
        fputs("no Vulkan instance\n", stderr);
        return 1;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    if (count == 0) {
        fputs("no Vulkan device\n", stderr);
        vkDestroyInstance(instance, NULL);
        return 1;
    }

    VkPhysicalDevice devices[8];
    if (count > 8) {
        count = 8;
    }
    vkEnumeratePhysicalDevices(instance, &count, devices);

    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(devices[0], &properties);
    printf("device: %s\n", properties.deviceName);

    /* The two combined formats Vulkan guarantees one of, the stencil-only format, and a color
     * format as a control -- the patch must leave a color format answering from the color bit. */
    report(devices[0], VK_FORMAT_D32_SFLOAT_S8_UINT, "DEPTH32F_STENCIL8");
    report(devices[0], VK_FORMAT_D24_UNORM_S8_UINT, "DEPTH24_STENCIL8");
    report(devices[0], VK_FORMAT_S8_UINT, "STENCIL8");
    report(devices[0], VK_FORMAT_R8G8B8A8_UNORM, "RGBA8 (control, color)");

    vkDestroyInstance(instance, NULL);
    return 0;
}
