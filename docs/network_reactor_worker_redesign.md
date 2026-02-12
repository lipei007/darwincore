# DarwinCore Network 重构设计（Reactor/Worker 分层）

## 目标

- 可靠：连接生命周期可观测、可恢复、可优雅停机。
- 高性能：Reactor 线程不做业务计算，最小化阻塞。
- 高并发：事件驱动 + 背压，避免单点拥塞拖垮整体。

## 职责边界

### Reactor（仅 I/O 与连接状态机）

- 处理 `accept/read/write/close/error/timeout`。
- 独占 fd 所有权，只有 Reactor 线程能操作 fd。
- 做传输层背压（高/低水位、暂停/恢复读事件）。
- 向 Worker 投递业务事件，不执行业务逻辑。

### Worker（仅业务）

- 协议解码/编码、业务路由、业务计算。
- 不能直接碰 fd，也不能直接关闭连接。
- 通过命令队列向 Reactor 发 `Send/Close/Pause/Resume` 请求。

## 统计设计

### 全局（Server）

- `total_connections`
- `active_connections`
- `total_messages_received/sent`
- `total_bytes_received/sent`
- `total_errors`
- `worker_total_queue_size`
- `worker_submit_failures`

### 分 Reactor

- `reactor_id`
- `total_connections`
- `active_connections`
- `total_bytes_received/sent`
- `total_ops_processed`
- `pending_operation_queue_size`
- `total_dispatch_failures`
- `total_add_requests/failures`
- `total_remove_requests/failures`
- `total_send_requests/failures`
- `GetSendBufferSize` 查询改为通过 Reactor 队列同步查询（避免跨线程访问连接表）

## 运行参数（已支持）

- `Server::Options::reactor_count`：0 = 自动（CPU 核数）
- `Server::Options::worker_count`
- `Server::Options::worker_queue_size`

## 分阶段实施

### Phase 1（已完成）

- 暴露统一统计 API：`Server::GetStatistics()`
- 补齐基础可测性（无网络依赖单元测试）
- 修复基础并发可见性问题（`WorkerPool::is_running_` 原子化）

### Phase 2（进行中）

- [x] Reactor 操作队列唤醒机制（`EVFILT_USER`，替代 100ms 轮询依赖）
- [x] `AddConnection` 改为非阻塞入队
- [x] 停机路径移除跨线程直接访问连接表
- [x] ClientReactor 发送队列改为唤醒驱动（低延迟发包）
- [ ] `Remove/Send` 请求的可追踪回执与错误传播完善

### Phase 3

- 引入可配置背压策略（阻塞/丢弃/断开慢连接）
- 增加慢消费者与队列积压告警
- 生产压测与稳定性回归

## TDD 测试矩阵（后续）

1. 连接生命周期：Connect -> Data -> Disconnect 顺序与幂等关闭。
2. 背压行为：发送高水位时暂停读，降到低水位恢复读。
3. 队列压力：Worker 队列满时策略行为正确且可观测。
4. 超时回收：空闲连接按配置关闭并上报统计。
5. 停机语义：`Stop()` 后无 fd 泄漏、无悬挂线程。
6. 统计一致性：全局统计 = 各 Reactor 汇总（允许短时间观测滞后）。

## 压测入口

- `test_server_high_concurrency [client_count] [messages_per_client] [payload_size] [send_p99_ms_max]`
- 输出指标：`send_p50`、`send_p99`、`throughput(MB/s)`、统计一致性断言。
