# V2 与 V3 对比

这个仓库把公开学习路径分成两层：先用 `v2` 建立基础理解，再用 `v3` 理解更低延迟、更接近产品化的方向。

| 路径 | 交互形状 | 第一目标 | 适合人群 | 当前公开状态 |
| --- | --- | --- | --- | --- |
| `v2` | 更简单的 request/response 流程 | 从零理解一次端云交互 | 第一次接触项目的开发者 | local 浏览器 demo、hardware baseline、最小 FastAPI base |
| `v3` | 更进阶的低延迟方向 | 理解下一阶段 public-preview contract | 有硬件或产品化经验的开发者 | `/api/v3/healthz`、`/api/v3/contract`、session create/read/close/events 的最小 in-process 示例 |

## 什么时候先选 V2

- 你第一次看这个仓库。
- 你还没有准备硬件。
- 你想先理解基础交互流程。
- 你需要从官方 `ESP-VoCat v1.2` baseline 开始。

## 什么时候看 V3

- 你已经理解 v2。
- 你关心更低延迟的交互方向。
- 你想看 public-preview API contract 如何划边界。
- 你在评估硬件产品化路径，但还不需要完整生产部署。

## 当前 V3 边界

当前 v3 已经不只是空 route skeleton。它包含：

- `/api/v3/contract`：公开 contract 说明。
- `POST /api/v3/session`：创建 in-process、in-memory、ephemeral 的 accepted session record。
- `GET /api/v3/session/{session_id}`：读取当前进程内 session，并在受控条件下做一次最小 progression。
- `DELETE /api/v3/session/{session_id}`：应用最小 close 语义，保留 closed terminal state。
- `GET /api/v3/session/{session_id}/events`：从当前 record 做固定 snapshot projection。

但它仍然不是：

- 实时音频实现
- event stream / replay
- provider orchestration
- 持久化 session runtime
- 生产部署指南
- 多板卡公开 SDK
