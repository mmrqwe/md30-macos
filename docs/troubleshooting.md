# 开发踩坑与排查记录

## USB 能枚举，macOS 却没有音频设备

MD30 的 USB ID 为 `04b4:5030`。配置 1 提供厂商自定义接口，流 0 使用 Bulk OUT 端点 `0x02`。依据原厂 Windows 驱动的行为实现时钟、路由和流控制，再用 libASPL 向 Core Audio 发布双声道输出设备。

## 采样率锁定，耳机仍然无声

设备显示采样率只验证时钟已经切换。驱动还要在开始播放时配置 USB 到模拟混音的路由，并初始化各级数字增益。分别发送左、右声道测试音，读取 USB 输入及内部模拟混音电平，可以把 USB 传输与设备内部路由分开检查。模拟混音表有电平时，仍需试听物理输出。

## 短测试正常，高采样率听歌却有毛刺

短音调跑通只验证基本播放。192 kHz 普通播放曾在长测中出现缓冲溢出，增加缓冲容量、提高发送线程调度优先级后，同样的 90 秒测试未复现丢帧。音乐对照仍可听到高采样率毛刺，绕过系统混音后也能复现。

最终改为异步 Bulk 写入，提前排队约 24 ms 的 USB 数据。192 kHz 用 18 个 256 帧区块，44.1 kHz 用 5 个区块。加深队列后，同一段 192 kHz 音乐的实机试听未再听到原先的毛刺，44.1 kHz 也进行了试听。Core Audio 时钟根据缓冲占用校准，驱动不靠插值或跳过样本消化时钟差。

## 把独占权限和逐位输出分开验证

Core Audio 独占权限限制哪个播放器能使用设备。逐位输出还要求播放器选择整数 PCM 格式并关闭会更改样本的处理。驱动把 16 或 24 位整数左对齐到 32 位 USB 字，低位补零，32 位样本原样提交。系统混音路径使用浮点 PCM。

在实机上验证了 16、24、32 位和六档采样率的 18 种直接 USB 组合，提交给 USB 的 PCM 与独立生成的预期数据逐位吻合。通过正式 Core Audio IOProc 另外验证了 48 kHz 的三种整数独占格式。模拟耳机输出的听测用于确认播放。

## 安装后 Chrome 播放卡住，控制中心音量条消失

更新驱动时重新加载 Core Audio，原有客户端有时继续使用旧的音频状态。Chrome 在本机 WAV 测试页也会卡在启动阶段。控制中心曾将 MD30 判为音量不可调，当时系统设置中的同一音量仍可调。

安装包现在等 Core Audio 重新加载驱动并确认输出音量可写，然后刷新 Chrome 音频子进程和控制中心。实测升级前打开的 Chrome 测试页无需刷新就能完整播放，控制中心重新识别了原有音量。这个安装后流程只对当前图形登录用户执行。

## 面板随 USB 自动启动

登录后，LaunchAgent 使用 IOKit 匹配通知监听 MD30。注册时先处理已连接的设备以启用后续通知。面板未运行时在后台启动，已运行时复用原实例。面板收起后也会检查 USB 状态，连续两次找不到设备才退出。实机验证覆盖了设备已连接时启动面板以及复用已打开的实例。

## 断电重启后是否要载入原厂启动代码

实机断电重开后，USB 重新枚举，设备可返回 `B5=01` 和 `B6=05`，并在 Mac 未下发启动代码的情况下通过 48 kHz 播放和电平检查。平时可直接使用。

可选的诊断工具从用户自行获取的原厂 2.0.1.0 x64 `MlCyMD_win7.sys` 提取并校验设备启动代码，临时写入 USB 控制器 RAM。关机断电后清除这次载入。原厂二进制和提取数据保留在本地。[乐之邦驱动下载](https://www-old.musiland.cn/index.php/Download/show/id/73) · [Infineon EZ-USB 启动说明](https://community.infineon.com/t5/Knowledge-Base-Articles/Examples-showing-how-to-download-firmware-to-a-EZ-USB-AN21xx-FX-FX1-FX2-FX2LP/ta-p/253105)

## 复测命令

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target all md30-mixer-smoke md30-hal-smoke
build/debug/md30-usb --audio-ready
```

直接 USB 检查需要独占接口。先暂停使用 MD30 的应用，接口仍被占用时暂时将系统输出切到别的设备。

```sh
build/debug/md30-usb --probe-runtime
build/debug/md30-mixer-smoke
build/debug/md30-hal-smoke build/debug/MD30.driver/Contents/MacOS/MD30
python3 tests/pcm_smoke.py build/debug/md30-usb
```

安装系统驱动后，可以用公开的 Core Audio 接口检查整数独占。

```sh
xcrun clang -I src tests/integer_playback_smoke.c -framework CoreAudio \
  -framework CoreFoundation -o build/debug/md30-integer-smoke
build/debug/md30-integer-smoke
```
