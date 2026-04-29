# Quickstart

这份 quickstart 只回答一个问题：第一次看这个仓库，应该按什么顺序学习。

## 1. 先看 Local V2

从 `examples/local-v2/` 开始。

这一层不需要硬件，也不要求你配置云服务。它只是用一个静态浏览器页面，让你先理解一次语音交互大概会经过哪些角色：

- 用户输入
- 助手回复
- 音频输出占位

当前 `local-v2` 还不是完整语音链路，不包含真实麦克风、STT、TTS 或 streaming。

## 2. 再看 Hardware V2

当你理解 local-v2 后，再进入 `examples/hardware-v2/`。

公开硬件 baseline 只支持一个官方路径：

- `ESP-VoCat v1.2`
- firmware 路径：`hardware/esp-vocat-v1.2/firmware/`
- 默认公开 transport：`v2 async`

真实烧录前需要你在本地配置 Wi-Fi、服务器地址和设备 ID。仓库不会提交这些值。

## 3. 最后看 Hardware V3

当 v2 路径已经清楚后，再进入 `examples/hardware-v3/` 和 `docs/v3-runtime-boundary.md`。

v3 当前是 public-preview：

- 有 `/api/v3/` 路由命名空间
- 有 public contract
- 有最小 in-process session 示例
- 没有真实实时音频 transport
- 没有生产部署或持久化 runtime

## 4. 商业协作入口

如果你的目标不是学习，而是把语音设备做成可交付产品，请看：

- `commercial/README.md`
- `commercial/services.md`
- `commercial/intake-form.md`

那里说明了公开学习路径和 To-B 交付支持之间的边界。
