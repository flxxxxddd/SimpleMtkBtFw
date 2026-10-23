# SimpleMtkBtFw

[English](README.md)

SimpleMtkBtFw 是一个用于 macOS 的 MediaTek USB 蓝牙驱动

它不是 Wi-Fi 驱动，也不是独立蓝牙协议栈。固件初始化完成后，蓝牙功能由 macOS 接管。

## 支持硬件

准确 USB 匹配列表见 [SimpleMtkBtFw/Info.plist](SimpleMtkBtFw/Info.plist)。

当前 personalities 覆盖 MT7921、MT7922、MT7922A，以及 MediaTek 通用蓝牙 USB class 匹配。运行时固件流程只接受 MediaTek 内部设备 ID `0x7961` 和 `0x7922`。

内置固件：

- `BT_RAM_CODE_MT7961_1_2_hdr.bin`
- `BT_RAM_CODE_MT7961_1a_2_hdr.bin`
- `BT_RAM_CODE_MT7922_1_1_hdr.bin`

## 构建

在项目根目录执行：

```sh
xcodebuild -project SimpleMtkBtFw.xcodeproj -scheme SimpleMtkBtFw -configuration Release build
xcodebuild -project SimpleMtkBtFw.xcodeproj -scheme SimpleMtkBtPatcher -configuration Release build
```

工程包含两个 KEXT：

- `SimpleMtkBtFw.kext`：加载 MediaTek 蓝牙固件。
- `SimpleMtkBtPatcher.kext`：基于 Lilu 的兼容性补丁。

## 安装

仅推荐通过 OpenCore 注入。

加载顺序：

1. `Lilu.kext`
2. `SimpleMtkBtFw.kext`
3. `SimpleMtkBtPatcher.kext`

不要同时注入另一个会绑定到同一 USB 蓝牙设备的蓝牙固件加载器。

### macOS Tahoe 26：MT7922 / RZ616 BLE HID 配对修复

在 macOS Tahoe 26 上，MT7922 / AMD RZ616 可能能够正常发现并建立 BLE 连接，
但在配对 BLE HID 设备时一直转圈，且不会出现验证码输入提示。

本分支包含一个实验性兼容补丁，通过 OpenCore boot-args 启用：

```text
-smbtlefix
```

已确认的故障链路为：

1. Tahoe 在 Remote Features 之后等待 LE PHY Update Complete；
2. MT7922 未按 Tahoe 预期提供该 readiness event；
3. 补丁注入 PHY Update Complete 后，`bluetoothd` 会继续推进，但随后卡在
   `LE_READY_WAIT_FOR_LENGTH`；
4. 补丁向当前连接发送真实的 `HCI_LE_Set_Data_Length (0x2022)`；
5. MT7922 返回成功的 Command Complete，但没有继续产生 Tahoe 等待的
   LE Data Length Change；
6. 补丁把这条私有 `0x2022` 的成功完成事件转换为缺失的
   LE Data Length Change，使 `bluetoothd` 继续进入 SMP 配对流程。

目前实测成功环境：

- AMD RZ616 / MediaTek MT7922
- USB ID `0489:E0D8`
- macOS Tahoe 26
- Logitech K251 BLE 键盘

启用补丁后，K251 可以正常弹出验证码并完成配对。

> `-smbtlefix` 目前仍是实验性、按需启用的补丁，不建议在没有上述症状时默认开启。

技术细节见 [Issue #2](https://github.com/flxxxxddd/SimpleMtkBtFw/issues/2) 和
[PR #3](https://github.com/flxxxxddd/SimpleMtkBtFw/pull/3)。

## 排错

- 检查内核日志中的 `SimpleMtkBtFw` 和 `SimpleMtkBtPatcher` 输出。
- 确认 USB 设备被 `SimpleMtkBtFw/Info.plist` 匹配。
- 确认日志中的内部设备 ID 是 `0x7961` 或 `0x7922`。
- 在 IORegistry 中检查 `FirmwareLoaded` 和 `fw_name`。
- Tahoe 26 下如果 BLE HID 设备可见但配对一直转圈，可尝试启用 `-smbtlefix`。
- 可用以下命令查看 Tahoe BLE 修补计数器：

  ```sh
  ioreg -l -w0 | grep -E 'SimpleMtkBtLEPhyFakeCount|SimpleMtkBtLESetDataLengthSentCount|SimpleMtkBtLEDataLengthFakeCount'
  ```

  成功触发一次配对修补时，三个计数器通常都会增加。

## 致谢

- `laobamac` - SimpleMtkBtFw 作者，负责 MediaTek 蓝牙移植、集成和测试。
- `zxystd` 以及 IntelBluetoothFirmware 贡献者 - 原始框架、保留的上游代码和固件生成辅助工具。
- MediaTek Inc. - MediaTek 蓝牙固件格式和 Linux 驱动参考资料。
- Linux kernel Bluetooth 贡献者 - `btmtk` / `btusb` 逻辑和设备 ID 参考。
- `vit9696`、acidanthera 和 Lilu 贡献者 - Lilu patching framework。
- `dawalishi821` 提供网卡硬件

## 版权和许可

以各源文件头部和仓库内 license 文件为准。从上游改写或保留的文件继续保留原始版权和 SPDX/license 头；SimpleMtkBtFw 新写文件使用 `laobamac` 版权头。

发布包含固件 blob 的二进制包前，请确认固件再分发条款。

## 免责声明

这是面向非官方支持硬件的实验性驱动。使用风险由使用者自行承担。
