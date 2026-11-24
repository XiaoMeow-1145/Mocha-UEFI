# 小米平板 1 的 ARM32 UEFI 原型

小米平板 1（A0101，代号 mocha）使用 Tegra K1 T124 和 Cortex-A15，能够运行板级定制的 AArch32 UEFI 环境。第一版使用 U-Boot v2025.10 的 Mocha 板级支持和 EFI Loader，从原厂引导器接力。Surface 2 的 Tegra 4 T114 使用独立的板级配置。两者可参考同类子系统的实现，驱动能否复用还取决于硬件控制器、板级连线以及操作系统接口。[Mocha 板级文档](https://docs.u-boot.org/en/v2025.10/board/xiaomi/mocha.html#chainload-configuration) · [Surface 2 板级文档](https://docs.u-boot.org/en/v2026.10/board/microsoft/surface-2.html) · [U-Boot 的 UEFI 支持](https://docs.u-boot.org/en/v2025.10/develop/uefi/uefi.html)

## 构建与验证

在本目录执行以下命令。依赖是 Ubuntu 24.04 软件包名。构建会下载并校验固定 SHA-256 的 U-Boot 源码，再应用 [PRAM 修复补丁](<patches/0001-reserve-chainload-pram.patch>)。交叉工具链可通过 CROSS_COMPILE 指定。

~~~sh
sudo apt install build-essential bc bison flex swig curl bzip2 patch \
    gcc-arm-linux-gnueabihf device-tree-compiler python3-dev \
    python3-setuptools python3-pyelftools libssl-dev qemu-system-arm
make -j"$(nproc)" mocha sd-files
make -j"$(nproc)" test
~~~

| 构建产物 | 用途 |
| --- | --- |
| [u-boot-dtb.bin](<build/mocha/u-boot-dtb.bin>) | Mocha 原厂引导接力载荷。ARM32 入口为 0x80a00000，包含板级 DTB 和内置 EFI 自检。 |
| [BOOTARM.EFI](<build/sd/EFI/BOOT/BOOTARM.EFI>) | 可放入 microSD 的 PE32 ARM Thumb EFI 应用。 |
| [u-boot.bin](<build/qemu-arm/u-boot.bin>) | QEMU virt 的 Cortex-A15 测试固件。 |

## microSD 启动

[Mocha 板级配置](<mocha-chainload.config>)使用 RAM 环境和非持久 EFI 变量。倒计时结束后，U-Boot 从 microSD 第一分区（mmc 1:1）的 FAT 文件系统读取 EFI/BOOT/BOOTARM.EFI，将板级 FDT 交给应用。把该文件放到已准备好的 FAT 分区的对应路径即可。应用返回成功后回到 U-Boot 控制台。缺卡、文件读取失败或应用启动失败时，固件运行内置自检。

在 U-Boot 控制台可用下方命令单独重跑内置自检。

~~~text
bootefi hello ${fdtcontroladdr}
~~~

[EFI 自检](<mocha_probe.c>)检查 System Table、Boot Services 的 GetMemoryMap 和 EFI 配置表里的 FDT，打印 EFI 内存表的保留区域并查询 GOP。加载自 microSD 或其他受支持的 FAT 介质时，它会通过 Loaded Image 的设备句柄查找 Simple File System 协议，并以只读方式打开源卷的根目录。

QEMU 8.2.2 的 Cortex-A15 virt 使用只读 VirtIO FAT 盘。以下三条路径均通过 [自动测试](<qemu_smoke.py>)。

| 测试场景 | 结果 |
| --- | --- |
| 无介质 | 运行内置自检，输出 UEFI probe PASS。 |
| 有效 BOOTARM.EFI | 运行外部应用，输出 EFI boot volume open OK 和 UEFI probe PASS，返回 U-Boot 控制台。 |
| 文件存在但内容不是 EFI 镜像 | U-Boot 拒绝加载该文件并运行内置自检。 |

另一次 QEMU 测试为 virt 接入 Bochs 显示设备，自检通过 GOP 读取到 1280×1024 模式。Mocha 载荷的静态检查确认了 ELF32 ARM 入口地址、microSD 的 mmc1 别名和设备树 compatible 值 xiaomi,mocha、nvidia,tegra124。A0101 真机已通过内置 EFI 自检。

## 真机交接与 Windows

U-Boot v2025.10 为 Mocha 原厂 fastboot 接力指定了 u-boot-dtb.bin，链接地址为 0x80A00000。具体临时启动命令和装载行为需要在对应的原厂引导器上核实。[Mocha 上游接力配置](https://docs.u-boot.org/en/v2025.10/board/xiaomi/mocha.html#chainload-configuration)

AOSP Android 14 的 fastboot boot 会检测 ANDROID! 镜像头。传入原始二进制时，主机工具会先生成 Android boot image，然后请求临时启动。分析原厂接力时应同时核对主机生成的镜像和设备侧行为。[AOSP fastboot 源码](https://android.googlesource.com/platform/system/core/+/refs/tags/android-14.0.0_r1/fastboot/fastboot.cpp#643)

从 MIUI 中国稳定版 V9.2.4.0.KXFCNEK 工厂包读取了 images/boot.img 的头部。识别到 ANDROID!，页大小为 2048 字节，kernel_addr 为 0x10008000，ramdisk_addr 为 0x12000000，tags_addr 为 0x10000100。头部偏移 0x28 的值为 84,648 字节，与包内独立设备树文件的大小匹配，也符合页对齐后的镜像尾部长度。这个值可作为老格式 DT 大小的线索。该头部描述的是工厂镜像，设备侧临时启动的放置地址和跳转规则仍需实测。[小米官方工厂包](https://cdnorg.d.miui.com/V9.2.4.0.KXFCNEK/mocha_images_V9.2.4.0.KXFCNEK_20180406.0000.00_4.4_cn_0d819c67da.tgz)

一台运行 Android 9 的 A0101 已从原厂引导器接力进入 U-Boot。通过网络 ADB 执行 `reboot bootloader` 后，同一台平板以 USB fastboot 枚举。`getvar product` 返回 `Mocha`，`getvar version-bootloader` 返回 `tegra-19r15.1-android-4.4-98-gbf89846`，主机随后执行了以下命令。

~~~sh
fastboot --header-version 0 --page-size 2048 --base 0x10000000 \
  --kernel-offset 0x00008000 --ramdisk-offset 0x02000000 \
  --tags-offset 0x00000100 boot build/mocha/u-boot-dtb.bin
~~~

首版镜像临时启动后，平板屏幕显示 `Tegra124 (Mocha) #`。照片记录到 microSD 电压协商超时 `-110`，随后内置自检路径报 `out of memory` 和 `Cannot initialize UEFI sub-system, r = 9`。

Mocha 接力配置在 DRAM 顶部预留 225 MiB PRAM。首版的 LMB 仍会从这段地址分配页面，EFI 则把它视为引导服务代码占用区，拒绝分配。修复版将 PRAM 纳入 LMB 的保护范围，在 EFI 内存表中将其标为 `EFI_RESERVED_MEMORY_TYPE`。QEMU 配置同样预留 225 MiB，并检查保留区的起始地址与页数。对照构建仅关闭 `CONFIG_LMB_RESERVE_PRAM`，复现了 `out of memory` 和 `r = 9`。修复版还通过了 3 GiB virt 测试，覆盖 ARM32 内存上界恰好到达 4 GiB 的情况。

修复版经 USB `fastboot boot` 临时启动。主机发送了 784,384 字节的引导镜像，平板 LCD 上显示了以下结果。

~~~text
Mocha UEFI probe (AArch32)
UEFI revision=0x0002006e
GetMemoryMap OK, bytes=0x000002f8, descriptor size=0x00000028
EFI reserved memory start=0x00000000f1f00000, pages=0x000000000000e100
FDT table and header OK
EFI boot volume unavailable
GOP active, width=0x00000600, height=0x00000800
UEFI probe PASS
~~~

真机的 EFI 内存表将 `[0xf1f00000, 0x100000000)` 标为保留区。GOP 报告的分辨率为 1536 × 2048。屏幕也记录了 microSD 卡协商超时 `-110`，固件随后运行内置应用。真机从 microSD 读取 EFI 应用和访问源卷的路径仍待验证。

### Windows PE 1809 ARM32 真机测试

测试介质取自微软 Windows 10 1809 ADK 的 Windows PE 加载项。将原版微软启动文件、BCD、SDI 和 WIM 放入 256 MiB GPT/FAT32 RAM 盘。U-Boot 将它注册为 EFI 块设备，从同一卷加载 ARM Thumb EFI 应用，使启动管理器能定位启动数据。[微软 ADK 下载页](https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install) · [介质打包工具](<make_winprobe_disk.py>) · [EFI 加载代码](<mocha_winprobe.c#L136>)

EFI 加载器原先遇到 ARM Thumb 的 MOV32T 重定位后拒绝加载微软启动程序。补齐这种重定位后，Cortex-A15 QEMU 运行了微软启动程序，并显示 `Loading files...`。[重定位补丁](<patches/0003-arm-thumb-mov32-relocation.patch>) · [QEMU 日志](<build/winprobe-fixed/qemu/winpe-fixed.log#L29>)

真机的第一版 WinPE 固件报告 `WINPROBE RAM disk decompression or checksum failed`。旧版早期栈顶 `0x85cfff10` 落在内嵌 gzip 的地址范围内，启动代码清零全局数据时覆盖了压缩数据。修复版把早期栈移到单独的可写区域。原厂 Fastboot 接受了 195,264,512 字节的修复版临时启动镜像。[栈补丁](<patches/0004-winprobe-mocha-early-stack.patch>) · [修复版 Fastboot 日志](<build/winprobe-fixed/mocha/fastboot-attempt.log#L5>)

实测固件的 SHA-256 为 `09a4de98c1088eba29aba40cfa10bd9db96ac2438140b7cea58fbe924205db70`。压缩 RAM 盘的 SHA-256 为 `825638cebbbbf57567135be569343fa9d2ed29f36137274f4136f372bb2e309c`。[实测固件](<build/winprobe-fixed/mocha/u-boot-dtb.bin>)

修复版在 A0101 的 LCD 上先显示 `WINPROBE decompressing RAM disk`。操作者在解压完成后看到了 Windows PE 加载进度条。之后拍摄的 LCD 照片记录了以下状态。该日志由固件在结束 EFI 引导服务后打印，`0` 对应 `EFI_SUCCESS`。[退出引导服务日志代码](<patches/0002-trace-winprobe.patch#L33>)

~~~text
WINPROBE ExitBootServices = 0x0000000000000000
~~~

真机已确认运行 ARM32 Windows 启动程序并到达 UEFI 交接点。内核是否继续运行以及 WinPE 命令行是否出现仍待验证。主机随后再次识别到序列号 `4A6CF56E` 的平板处于 Fastboot 模式（USB `18d1:d00d`）。返回 Fastboot 的过程没有日志。这一版固件只发布板级 FDT。Windows 的 ARM 板级支持通过 ACPI 获取中断控制器、定时器等平台信息。[Mocha 构建配置](<build/winprobe-fixed/mocha/.config#L2132>) · [微软 ACPI 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-system-description-tables)

从本目录使用已准备好的压缩 RAM 盘构建并临时启动。平板端的启动介质和 EFI 变量都位于 RAM，实验固件关闭了 MMC 控制器、USB 存储和 Fastboot 刷写。[RAM 盘](<build/winpe-1809-complete.img.gz>) · [实验配置](<winprobe.config>)

~~~sh
make -j"$(nproc)" WINPROBE_OUT="$PWD/build/winprobe-fixed" \
  WINPROBE_DISK="$PWD/build/winpe-1809-complete.img.gz" winprobe-mocha
fastboot --header-version 0 --page-size 2048 --base 0x10000000 \
  --kernel-offset 0x00008000 --ramdisk-offset 0x02000000 \
  --tags-offset 0x00000100 boot build/winprobe-fixed/mocha/u-boot-dtb.bin
~~~

### ACPI 和 WinPE 命令行测试

第二次真机测试发布单核 ACPI，同时保留 FDT 和 ARM 缓存设置。录像里，解压开始于录像前，固件报告耗时 159,054 ms。约 10 秒时正在校验 RAM 盘，25–35 秒显示 Windows 的 `Loading files...` 进度条，60 秒到约 64.3 秒持续显示 `ExitBootServices = 0`。录像中的 GICD/GICC 为 `50041000`/`50042000`，计数器频率为 12 MHz。录像结束时仍在交接画面。 [Fastboot 日志](<build/winprobe-acpi-winpe/mocha/fastboot-attempt.log>) · [35 秒加载进度](<build/boot-video-rtrtr/frame-08.png>) · [录像末帧](<build/boot-video-rtrtr/final-64_3s.png>)

后续测试版将 LCD 改为 1536 × 2048 的 32 位 BGRX，GOP 行距为 6144 字节，EFI 内存表保留 12 MiB 活动帧缓冲。WinPE 的启动脚本在进入命令行后显示 `[WINPE-1809-ARM32-WITNESS] CMD_ENTERED`，执行 `ver` 和 `wpeinit`，并保留命令提示符。WinPE 的 `SanPolicy=4` 在启动时将内置磁盘置为离线，打包后的 EFI 启动介质已回读验证。原厂 Fastboot 于 03:22:46 接受了 197,361,664 字节的临时启动包，等待真机画面确认 `ver` 输出。 [显示补丁](<patches/0006-winprobe-mocha-bgra.patch>) · [配置](<winprobe-acpi-fb32.config>) · [Fastboot 日志](<build/winprobe-witness-fb32/mocha/fastboot-offline-attempt.log>) · [固件](<build/winprobe-witness-fb32/mocha/u-boot-dtb.bin>)
