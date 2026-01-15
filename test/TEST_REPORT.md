 # DarwinCore Network 调试报告

## 问题总结

### 原始症状
- ✅ OnNetworkConnected 被调用（kConnected 事件正常）
- ❌ OnNetworkMessage 未被调用（kData 事件丢失）

### 根本原因：**死锁 (Deadlock)**

在 `JsonRpcServer::OnNetworkConnected` (jsonrpc_server.cc:158) 中存在死锁：

**执行流程：**
1. Worker 1 线程处理 kConnected 事件
2. 调用 `JsonRpcServer::OnNetworkConnected`
3. 获取 `connections_mutex_` 锁 (第 163 行)
4. **在持有锁的情况下**调用用户回调 `on_connected_` (第 168 行)
5. 用户回调调用 `server.GetConnectionCount()` (simple_server.cc:94)
6. `GetConnectionCount()` 尝试获取 `connections_mutex_` 锁 (jsonrpc_server.cc:128)
7. **死锁！** 同一线程无法重复获取同一把锁

**代码位置：**
```cpp
// src/jsonrpc_server.cc:158-170 (错误版本)
void JsonRpcServer::OnNetworkConnected(const network::ConnectionInformation& info) {
    ConnectionState state;
    state.parser = std::make_unique<FrameParser>();

    std::lock_guard<std::mutex> lock(connections_mutex_);  // 第一次获取锁
    connections_[info.connection_id] = std::move(state);

    if (on_connected_) {
        on_connected_(info.connection_id, info);  // 调用用户回调
    }
    // 锁在这里才释放
}

// 用户回调 (simple_server.cc:90-94)
server.SetOnClientConnected([&server](uint64_t conn_id, ...) {
    std::cout << "Total connections: " << server.GetConnectionCount() << std::endl;
    //                                  ^^^^^^^^^^^^^^^^^^^^
    //                                  尝试再次获取锁 -> 死锁！
});
```

## 解决方案

将用户回调的调用移到锁作用域之外：

```cpp
// src/jsonrpc_server.cc:158-173 (修复版本)
void JsonRpcServer::OnNetworkConnected(const network::ConnectionInformation& info) {
    ConnectionState state;
    state.parser = std::make_unique<FrameParser>();

    // 先添加连接
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_[info.connection_id] = std::move(state);
    }  // 锁在这里释放

    // 在锁外调用用户回调，避免死锁
    if (on_connected_) {
        on_connected_(info.connection_id, info);
    }
}
```

## 验证结果

修复后的完整调用链：
```
客户端发送数据
  → Reactor0::HandleReadEvent 收到 18 字节 ✅
  → WorkerPool::SubmitEvent (type=1) ✅
  → Worker 1 处理事件 ✅
  → Server::Impl::OnNetworkEvent ✅
  → Server 调用 on_message_ ✅
  → [###] SetOnMessage callback triggered ✅
  → [***] OnNetworkMessage called ✅
```

## 经验教训

1. **永远不要在持有锁的情况下调用用户回调**
   - 用户回调可能做任何事情，包括调用需要同一个锁的方法
   - 这会导致死锁或不可预测的行为

2. **调试多线程问题时，添加详细的调试日志至关重要**
   - 在关键位置添加 `[!!!]` 标记的日志
   - 追踪完整的调用链路

3. **死锁的典型症状**
   - 某个事件被处理，但后续事件不再被处理
   - 日志输出突然停止
   - 线程看起来"卡住"了

## 调试过程

通过在以下位置添加调试日志发现了问题：
1. `Reactor::HandleReadEvent` - 确认数据被读取
2. `WorkerPool::SubmitEvent` - 确认事件被提交
3. `WorkerPool::WorkerLoop` - 确认事件被处理
4. `Server::Impl::OnNetworkEvent` - 确认回调链路

关键发现：
- kConnected 事件被完全处理 ✅
- kData 事件被提交到队列，但 Worker 没有处理它 ❌
- 说明 Worker 线程在处理 kConnected 事件时卡住了

---
