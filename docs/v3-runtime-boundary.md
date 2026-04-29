# V3 Runtime Boundary

这份文档是公开版 v3 runtime 的整理说明。它合并了内部阶段评审中的结论，只保留 GitHub 访客需要知道的边界。

## 当前定位

`v3` 是这个仓库的进阶学习路径。它用于说明更低延迟、更接近产品化的方向，但当前仍是 `public-preview`，不是完整生产 runtime。

当前 v3 的价值是：

- 给 advanced path 一个清楚的 `/api/v3/` namespace。
- 用 contract 说明公开能力和不公开能力。
- 用最小 in-process session 行为展示 session 生命周期形状。
- 避免过早引入 provider、transport、worker、persistence 等生产复杂度。

## 当前已公开的行为

### Contract

`GET /api/v3/contract`

返回 v3 public-preview 的公开边界，包括：

- baseline board
- public routes
- supported capabilities
- out-of-scope items
- next step description

### Session Create

`POST /api/v3/session`

创建一个当前进程内的临时 session record：

- in-process
- in-memory
- ephemeral
- process restart 后丢失

它表示“公开 preview session 已被接受”，不表示真实音频或 provider 已连接。

### Session Read

`GET /api/v3/session/{session_id}`

当 session 来自当前进程内 create 时，它可以读回 record，并在受控条件下做一次最小 progression。

这个行为用于展示 runtime-shaped readback，不代表完整状态机。

### Session Close

`DELETE /api/v3/session/{session_id}`

当 session 存在于当前进程内 store 时，它会进入 retained closed terminal state，并使用中性的 `final_reason`，例如 `client-closed`。

close 后仍可通过 read route 读回 closed state。

### Events Snapshot

`GET /api/v3/session/{session_id}/events`

当前 events route 只做 snapshot projection：

- active record 映射为 `session.active`
- closed record 映射为单个 terminal event
- event 来自当前 record 的固定最小映射

它不是 history、不是 replay、不是实时 event stream。

## 明确不包含

当前公开 v3 不包含：

- 真实 audio transport
- 真实 event stream
- provider 调用
- 持久化 session state
- replay / history
- 后台 worker
- 生产部署编排
- 多板卡 runtime 支持

## 为什么停在这里

当前四条 session route 已经形成一个克制的最小闭环：

- create 有真实临时对象
- read 有真实读回
- close 有 terminal 语义
- events 有 snapshot projection

继续加深 in-process 行为，收益会下降，风险会上升：read 会更像副作用写路径，events 会更像伪 history，close 会逼近真实 teardown。

所以当前公开版的合理边界是：

`把 in-process runtime 固定为一个完成的 public-preview 阶段。`
