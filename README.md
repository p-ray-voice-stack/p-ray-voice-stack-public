# P-Ray Voice Stack

一个面向学习和原型验证的 AI 语音设备入门栈。

这个仓库的目标不是直接交付一整套量产系统，而是让开发者从零开始，沿着清晰路径理解“本地语音交互 -> 单板硬件 baseline -> v3 进阶链路”的演进方式。团队如果需要生产部署、定制板卡适配或长期交付支持，可以从 `commercial/` 进入商业协作路径。

## 学习路径

1. 从 `examples/local-v2/` 开始：不用硬件，先理解一次语音交互的基本形状。
2. 进入 `examples/hardware-v2/`：使用官方公开 baseline `ESP-VoCat v1.2`。
3. 阅读 `examples/hardware-v3/` 和 `docs/v3-runtime-boundary.md`：理解 v3 public-preview 的边界。
4. 如果你需要产品化交付、私有部署或板卡适配，查看 `commercial/README.md`。

## 当前公开状态

- `local-v2`：静态浏览器 demo，用于第一步教学，不包含真实麦克风、STT、TTS 或流式音频。
- `hardware-v2`：公开的 `ESP-VoCat v1.2` 单板 baseline，配置值保持空占位，不提交 Wi-Fi、设备 ID 或服务器地址。
- `hardware-v3` / `v3`：公开进阶路径，包含 public-preview API contract 和一层克制的 in-process runtime 示例。
- `commercial/`：面向 To-B 协作的公开入口，说明开源学习路径与商业交付边界。

## 仓库结构

- `server/`：最小 FastAPI 服务。
- `examples/`：local-v2、hardware-v2、hardware-v3 学习路径。
- `hardware/`：官方公开硬件 baseline。
- `docs/`：快速开始、v2/v3 对比、v3 runtime 边界、开源与商业边界。
- `commercial/`：商业协作入口和需求收集问题。
- `tests/`：公开仓库结构和核心 contract 的基础测试。

## 不包含什么

这个公开仓库当前不包含：

- 生产部署方案
- 真实实时音频 transport
- 真实 event stream / replay
- provider 调用
- 持久化 session state
- 多板卡公开支持
- 客户项目私有配置或交付材料

这些内容属于后续商业协作或更高阶公开阶段，不在当前 public-preview 范围内。
