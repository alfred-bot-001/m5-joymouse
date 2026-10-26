# M5 JoyMouse（M5StickC + Mini JoyC HAT 蓝牙鼠标）

把初代 M5StickC（ESP32-PICO-D4 v1.1，4MB Flash，FTDI 串口）和 M5Stack **Mini JoyC HAT**（STM32F030F4，I2C 地址 `0x54`，自带 200mAh 电池与 RGB LED）做成一只标准蓝牙 HID 鼠标：摇杆移光标，摇杆按键左键，A 键右键，按住 B 键推摇杆是滚轮。

| 项 | 位置 |
| --- | --- |
| 固件源码 | [firmware/stickc_joy_mouse/stickc_joy_mouse.ino](firmware/stickc_joy_mouse/stickc_joy_mouse.ino)（Arduino，esp32 核心 3.3.x + M5Unified 0.2.x，无第三方 BLE 库） |
| 编译 / 烧录 / 串口 | [firmware/build.sh](firmware/build.sh)（`compile` / `flash [port]` / `monitor [port]`） |
| 构建产物与出厂固件备份 | `~/workspace-agent/agent-data/m5-joymouse/`（`build/`、`backup/`，不进仓库） |

依赖：`arduino-cli`、esp32 核心 3.3.x（`arduino-cli core install esp32:esp32`）、M5Unified（`arduino-cli lib install M5Unified`）。

## 接线

Mini JoyC HAT 直接插 StickC 顶部 8 针插座（GND / 5V / SCL / SDA / BAT / 3V3），StickC 侧 HAT 总线是 **SDA=G0、SCL=G26**，固件用 ESP32 的 I2C 0 号口驱动（M5Unified 内部 AXP192 总线占 1 号口）。HAT 电池经 BAT 引脚与 StickC 电池并联，`cfg.output_power = true` 让 StickC 给 HAT 5V 供电。

## 操作

| 动作 | 效果 |
| --- | --- |
| 推摇杆 | 移动光标。死区 12%，1.8 次方加速曲线，满偏约 1400 px/s |
| 按下摇杆 | 鼠标左键 |
| StickC 正面 A 键（M5 字样） | 鼠标右键 |
| 按住 StickC 侧面 B 键再推摇杆上下 | 滚轮 |
| 长按电源键 6 秒 | 关机（AXP192 硬件行为） |

屏幕（竖屏，HAT 一端朝上）显示蓝牙状态（`advert.` 广播中 / `pairing` 已连接未加密 / `linked` 已配对）、摇杆是否在线、归一化的 X/Y、本次上报的 dx/dy、左右键状态、滚轮模式和电量。HAT 的 RGB LED：广播中蓝色慢闪，已连接绿色，按下左键白色。

上电时固件取 24 次采样作为摇杆中心和按键空闲电平，**上电时不要碰摇杆**；若光标漂移，重启一次即可。光标方向反了改源码里的 `kInvertX` / `kInvertY`。

## 配对

烧录后设备以 `StickC JoyMouse` 名称广播，HID 外观为鼠标，Just Works 配对并绑定（无 PIN）。macOS：系统设置 › 蓝牙，在「附近设备」里点连接；Windows / Linux 同理。配对一次后断电重连会自动回连；在电脑端「忘记设备」后需要重新配对，StickC 侧不用清除。

## 编译与烧录

```sh
# 编译（产物到 agent-data）
devices/stickc_joy_mouse/firmware/build.sh compile
# 编译并烧录，默认串口 /dev/cu.usbserial-89526F4C61，可用第二个参数覆盖
devices/stickc_joy_mouse/firmware/build.sh flash
# 看串口日志
devices/stickc_joy_mouse/firmware/build.sh monitor
```

FQBN `esp32:esp32:m5stack_stickc:PartitionScheme=huge_app,UploadSpeed=750000`。固件约 1.3MB，默认 `default` 分区（1.2MB 应用区）放不下，必须用 `huge_app`。初代 StickC 的 FTDI 串口在 921600 以上读写不稳定，烧录用 750000，读 Flash 备份用 115200。

串口日志（115200）关键行：`[joy] found at 0x54 ...` 表示摇杆读到了，`[ble] advertising as HID mouse` 表示开始广播，`[ble] pairing ok` 表示电脑已完成配对。

## 回滚

2026-10-25 烧录前已把整片 4MB 出厂固件备份到 `~/workspace-agent/agent-data/m5-joymouse/backup/stickc_c88541430590_factory_20261025.bin`（sha256 见同目录记录）。恢复出厂：

```sh
esptool --port /dev/cu.usbserial-89526F4C61 --baud 460800 write-flash 0x0 ~/workspace-agent/agent-data/m5-joymouse/backup/stickc_c88541430590_factory_20261025.bin
```

## 寄存器（Mini JoyC HAT，I2C 0x54）

| 寄存器 | 内容 |
| --- | --- |
| `0x00` | X、Y 12 位 ADC，各 2 字节小端 |
| `0x10` | X、Y 8 位（0–255），各 1 字节，固件用这个 |
| `0x20` | 按键，固件按上电空闲电平判断按下 |
| `0x30` | RGB LED，写 R、G、B 三字节 |
| `0xFE` | STM32 固件版本 |
| `0xFF` | I2C 地址（可改，固件不改） |

## 排查记录（追加）

- 2026-10-25 固件 0.1.0～0.1.5 实机：BLE 广播正常，HAT 插上后摇杆始终 `JOY: none`。屏幕诊断实测 SDA（G0）3135 mV、SCL（G26）128 mV，拔掉 HAT 后 SCL 回到高电平；对调 SDA/SCL 探测不到；插座 5V 断电 0.3 秒重上电多次无效。StickC 侧固件、引脚、驱动、5V 输出均已排除，判断 HAT 上的 STM32 未运行（未供电或卡死，HAT 自带电池使其拔插不复位）。待试：插着充电一小时；拆 HAT 底部两颗螺丝拔电池插头硬复位。
- 在 macOS 上用 pyserial / 串口监视器打开 FTDI 端口会触发 DTR/RTS 复位，且常把 ESP32 带进下载模式（G0 为低）；烧录后 esptool 的 RTS 复位也不可靠。**烧录后用电源键重启（长按 6 秒关、短按开），不要靠串口看日志，诊断信息看屏幕。** LCD 在 ESP32 复位后会保留上一帧画面，看到旧版本号即说明没有真正启动。
- 用 `esptool --before no-reset --after no-reset chip-id` 能连上即表示此刻在下载模式；用 `--after hard-reset` 复位后不要再打开串口。
- 摇杆未找到时屏幕的诊断行：`D? C? 5V?` 为 SDA/SCL 空闲电平与插座 5V 状态；下一行为两线毫伏（无上拉）、`p`/`SWAP!`（对调探测）与重试次数，每 5 次重试断电重上电一次。
- 2026-10-26 HAT 充电一小时与拆壳断电池硬复位后仍 `JOY: none`，判定该 HAT 损坏。应用户要求把 2026-10-25 的出厂整片备份（sha256 `b67abf0b…9069f`）用 `esptool --baud 750000 write-flash 0x0` 刷回，4MB 写入校验通过；StickC 当前为出厂固件，JoyMouse 固件只保留在仓库，待换新摇杆后再烧。
