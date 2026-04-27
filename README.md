# low_latency_layer

A C++23 implicit Vulkan layer that reduces click-to-photon latency by implementing both AMD and NVIDIA's latency reduction technologies.

By providing hardware-agnostic implementations of the `VK_NV_low_latency2` and `VK_AMD_anti_lag` device extensions, this layer brings Reflex and Anti-Lag capabilities to AMD and Intel GPUs. When paired with [dxvk-nvapi](https://github.com/jp7677/dxvk-nvapi/) to forward the relevant calls, it bypasses the need for official driver-level support.

The layer also eliminates a hardware support disparity as considerably more applications support NVIDIA's Reflex than AMD's Anti-Lag.

Benchmarks suggest the layer performs as well as or better than the proprietary Windows implementations on equivalent hardware. [More details and benchmarks are available here.](#testing-and-benchmarks)

# Dependencies

- [CMake](https://cmake.org): A cross-platform, open-source build system generator.
- [Vulkan Headers](https://github.com/KhronosGroup/Vulkan-Headers): Vulkan header files and API registry.
- [Vulkan Utility Libraries](https://github.com/KhronosGroup/Vulkan-Utility-Libraries): Library to share code across various Vulkan repositories.

# Building from Source and Installation

Clone this repo.

```
    $ git clone https://github.com/Korthos-Software/low_latency_layer.git
    $ cd low_latency_layer
```

Create an out-of-tree build directory (creatively we'll use 'build') and install.

> ⚠️ **WARNING:** You are likely going to have to install your distro's `vulkan-headers`, `vulkan-utility-libraries`, and possibly even `cmake` packages before proceeding. If you see an error here their absence is almost certainly the reason.

```
    $ cmake -B build ./
    $ cd ./build
    $ sudo make install
```

# Usage and Configuration

By default, the layer exposes the `VK_AMD_anti_lag` device extension. For Linux native applications like *Counter-Strike 2* this works out-of-the-box, allowing you to toggle AMD's Anti-Lag in its menus. You can further customize the layer's behavior using the environment variables listed below.

| Variable | Description |
| :--- | :--- |
| `LOW_LATENCY_EXPOSE_REFLEX` | Set to `1` to expose `VK_NV_low_latency2` instead of `VK_AMD_anti_lag`. |
| `LOW_LATENCY_SPOOF_NVIDIA` | Set to `1` to report the device as an NVIDIA GPU to the application, regardless of actual hardware. This is necessary for many applications to expose Reflex as an option. It _might_ be beneficial to keep this off when the application allows it. |
| `DISABLE_LOW_LATENCY` | Set to `1` to disable the layer. |


For Proton-based applications, you must enable NVAPI support alongside the layer's configuration. Use the `PROTON_FORCE_NVAPI=1` environment variable to force this support regardless of your hardware.

**Steam launch options example:**
```
PROTON_FORCE_NVAPI=1 LOW_LATENCY_EXPOSE_REFLEX=1 LOW_LATENCY_SPOOF_NVIDIA=1 %command%
```

The 'Boost' mode of Reflex is supported but is functionally identical to 'On' - the layer treats both modes identically.

# Testing and Benchmarks

Benchmarks were conducted under worst-case conditions using high-end AMD hardware. For configurations that create higher GPU load, these latency reductions will be more pronounced. We preferred testing on low resolution and high refresh-rate monitors as they provide less variance and are more likely to reveal correctness issues against proprietary reference implementations.

## Setup and Methodology
*   **GPU:** ASUS TUF Radeon RX 7900 XTX (flashed 550W Aqua Extreme BIOS) 1250MHz VRAM watercooled
*   **CPU:** AMD Ryzen 7 9800X3D 102.0MHz eCLK -15 CO 2133MHz FCLK delid watercooled
*   **Memory:** 64GB 2x32GB Hynix A-Die 6000MT/s CL28-36-36-30 GDM:off Nitro:1-2-0 (tuned)

We used Gentoo running KDE Plasma 6.6. Direct scanout was enabled throughout the testing process, verified as KWin’s 'Compositing' watermark disappeared when in fullscreen. Latency was measured using the NVIDIA Reflex Analyzer integrated into the ASUS PG248QP.

## THE FINALS
![tf](https://raw.githubusercontent.com/nJ3ahxac/files/main/low_latency_layer/the_finals.png)
**Results**

- We included comparisons against AMD's proprietary DX12 implementation of Anti-Lag 2 on Windows. The results suggest latency matches or beats native Windows numbers.
- We can directly compare our implementation of Reflex and Anti-Lag technologies - they appear to perform identically as both are in line with AMD's proprietary reference implementation of Anti-Lag 2.
- Mesa's anti-lag Vulkan layer was also included in testing. It appears to be a no-op in this case as it provides no latency benefit. The data suggests it may even increase latency slightly.

## Counter-Strike 2
![cs2](https://raw.githubusercontent.com/nJ3ahxac/files/main/low_latency_layer/cs2.png)
**Results**

- Unlike THE FINALS, where results were comparable, both technology implementations clearly beat the native Windows numbers in absolute terms.
- Reflex and Anti-Lag 2 again perform identically, consistent with our previous findings.
- CS2's `-vulkan` backend was also tested on Windows. It regresses baseline latency relative to the default backend, and AMD's Anti-Lag 2 does not recover this — it remains slower than Anti-Lag 2 on the default backend.
- Mesa's Anti-Lag Vulkan layer again appears to be a no-op, matching our findings from THE FINALS.

# License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
