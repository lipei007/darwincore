# DarwinCore Network 测试套件

## 测试概述

本测试套件为 DarwinCore Network 模块提供全面的测试覆盖，包括：

1. **kqueue 核心机制测试** - 验证底层事件驱动的正确性
2. **基本通信测试** - IPv4/IPv6/Unix Domain Socket 功能测试
3. **高并发压力测试** - 性能极限、资源泄漏、稳定性测试

---

## 测试架构

```
test/
├── server_client_test.cpp          # 基本通信测试
├── test_kqueue_core.cpp            # kqueue 核心机制测试
├── test_stress_concurrency.cpp     # 高并发压力测试
└── CMakeLists.txt                  # 测试构建配置
```

---

## 快速开始

### 构建测试

```bash
cd test
mkdir build && cd build
cmake ..
make -j4
```

### 运行测试

```bash
# 1. 运行 kqueue 核心机制测试
./test_kqueue_core

# 2. 运行基本通信测试
./server_client_test all

# 3. 运行不同规模的压力测试
./test_stress_concurrency small    # 100 连接
./test_stress_concurrency medium   # 1000 连接
./test_stress_concurrency large    # 10000 连接
./test_stress_concurrency extreme  # 50000 连接
```

---

## 测试详情

### 1. kqueue 核心机制测试 (`test_kqueue_core`)

**测试目标：**
- kqueue 事件注册与触发的正确性
- EV_EOF / EV_ERROR 处理
- EVFILT_READ / WRITE 语义验证
- udata 生命周期安全
- 非阻塞 IO 返回值全覆盖
- 批量事件处理

**运行：**
```bash
make test_kqueue
./test_kqueue_core
```

**覆盖场景：**
| 测试项 | 描述 |
|--------|------|
| 基本事件触发 | EV_ADD 后写入数据是否触发读事件 |
| EV_EOF 行为 | 对端 close() 后是否正确触发 EV_EOF |
| udata 生命周期 | kevent 返回时 udata 是否保持有效 |
| EV_ADD/DELETE/ENABLE/DISABLE | 事件标志位的正确性 |
| 非阻塞 IO 返回值 | recv/send 返回 >0, ==0, -1+EAGAIN, -1+EPIPE |
| EVFILT_WRITE 语义 | macOS 上写事件的即时就绪特性 |
| 批量事件处理 | 一次性获取多个事件 |

---

### 2. 基本通信测试 (`server_client_test`)

**测试目标：**
- IPv4 TCP 通信
- IPv6 TCP 通信
- Unix Domain Socket 通信

**运行：**
```bash
make test_all
# 或
./server_client_test all
```

---

### 3. 高并发压力测试 (`test_stress_concurrency`)

**测试目标：**
- Accept 风暴（高并发连接）
- 持续吞吐量测试
- 资源泄漏检测

**运行：**
```bash
# 不同规模
make test_small     # 100 连接，10 消息/连接
make test_medium    # 1000 连接，100 消息/连接
make test_large     # 10000 连接，1000 消息/连接
make test_extreme   # 50000 连接，100 消息/连接

# 或直接运行
./test_stress_concurrency medium
```

**性能指标：**
- **QPS** (Queries Per Second) - 每秒处理的消息数
- **吞吐量** - MB/s
- **平均延迟** - ms/msg
- **连接成功率** - 成功连接 / 总连接数

**测试场景：**

#### 场景 1: Accept 风暴
- 创建大量并发连接
- 验证所有连接是否被正确 accept
- 测试 Reactor 负载均衡

#### 场景 2: 持续吞吐量
- 每个连接发送多条消息
- 测试长期运行的稳定性
- 测量 QPS 和吞吐量

#### 场景 3: 资源泄漏
- 多轮连接创建和销毁
- 检测内存泄漏
- 检测文件描述符泄漏

---

## 性能基准

### 预期性能指标（macOS, M1/M2 芯片）

| 测试规模 | 连接数 | QPS | 吞吐量 | 平均延迟 |
|----------|--------|-----|--------|----------|
| small    | 100    | > 50K | > 50 MB/s | < 2ms |
| medium   | 1000   | > 100K | > 200 MB/s | < 5ms |
| large    | 10000  | > 200K | > 500 MB/s | < 10ms |
| extreme  | 50000  | > 300K | > 1 GB/s | < 20ms |

*注：实际性能取决于硬件配置和系统负载*

---

## 测试最佳实践

### 1. 测试前准备

```bash
# 增加文件描述符限制（macOS/Linux）
ulimit -n 65536

# 检查当前限制
ulimit -a
```

### 2. 调试模式

如果测试失败，启用详细日志：

```cpp
// 在测试代码中设置
config.verbose = true;
```

### 3. 使用 Instrument (macOS)

检测内存泄漏：

```bash
# 运行泄漏检测
leaks --atExit -- ./test_stress_concurrency medium

# 使用 Instruments GUI
instruments -t "Leaks" -D /tmp/trace.trace ./test_stress_concurrency medium
```

### 4. 系统监控

在运行大型测试时监控系统资源：

```bash
# 监控文件描述符
watch -n 1 'lsof -p <pid> | wc -l'

# 监控内存
watch -n 1 'ps -o pid,rss,vsz -p <pid>'

# 监控 CPU
top -pid <pid> -s 1
```

---

## 常见问题

### Q: 测试失败，提示 "Too many open files"

**A:** 增加文件描述符限制：
```bash
ulimit -n 65536
```

### Q: extreme 测试连接失败

**A:** 检查系统限制，可能需要调整：
```bash
# macOS
sudo sysctl -w kern.maxfiles=65536
sudo sysctl -w kern.maxfilesperproc=65536

# Linux
sudo sysctl -w fs.file-max=65536
```

### Q: 测试过程中 CPU 使用率低，性能不佳

**A:** 检查 Reactor 线程数配置，建议设置为 CPU 核心数。

---

## 持续集成

### CI 脚本示例

```bash
#!/bin/bash
set -e

ulimit -n 65536

cd test
mkdir -p build && cd build
cmake ..
make -j4

# 运行所有测试
./test_kqueue_core
./server_client_test all
./test_stress_concurrency small

echo "All tests passed!"
```

---

## 扩展测试

### 添加新测试

1. 创建新的测试文件 `test_your_test.cpp`
2. 在 `CMakeLists.txt` 中添加构建目标
3. 编写测试用例
4. 运行验证

### 测试模板

```cpp
#include <iostream>
#include <cassert>

bool TestYourFeature() {
  std::cout << "\n========== 测试: Your Feature ==========" << std::endl;

  // 测试逻辑
  bool result = true;

  // 验证
  assert(result == true);

  std::cout << "========== 测试 "
            << (result ? "通过" : "失败")
            << " ==========\n" << std::endl;

  return result;
}

int main() {
  bool pass = TestYourFeature();
  return pass ? 0 : 1;
}
```

---

## 测试覆盖报告

### 核心机制覆盖

- [x] kqueue 事件注册与触发
- [x] EV_EOF / EV_ERROR 处理
- [x] EVFILT_READ / WRITE 语义
- [x] udata 生命周期管理
- [x] 非阻塞 IO 返回值路径

### 连接管理覆盖

- [x] Accept 风暴
- [x] 并发连接创建
- [x] 连接关闭与清理
- [x] 半关闭（shutdown）处理

### 性能测试覆盖

- [x] QPS 测试
- [x] 吞吐量测试
- [x] 延迟测试
- [x] 资源泄漏检测

---

## 相关文档

- [DarwinCore 架构文档](../CLAUDE.md)
- [Network 模块设计](../src/darwincore/network/README.md)

---

## 贡献

如需添加新的测试用例或改进现有测试，请：

1. Fork 项目
2. 创建特性分支
3. 添加测试
4. 提交 Pull Request

---

## 许可证

Copyright © 2026 DarwinCore Network Team
