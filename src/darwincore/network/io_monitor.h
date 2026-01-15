//
// DarwinCore Network Module
// IOMonitor - IO 事件监控封装
//
// Description:
//   封装 kqueue/epoll 系统调用，提供跨平台的 IO 事件监控接口
//   macOS 使用 kqueue，Linux 使用 epoll
//
// Author: DarwinCore Network Team
// Date: 2026

#ifndef DARWINCORE_NETWORK_IO_MONITOR_H
#define DARWINCORE_NETWORK_IO_MONITOR_H

#include <cerrno>
#include <cstdint>
#include <cstring>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define USE_KQUEUE 1
#else
#include <sys/epoll.h>
#define USE_EPOLL 1
#endif

namespace darwincore {
namespace network {

/**
 * @brief IO 监控器封装类（内部使用）
 *
 * 提供跨平台的 IO 事件监控接口：
 * - macOS/FreeBSD: 使用 kqueue
 * - Linux: 使用 epoll
 *
 * 用途：
 *   - 监控文件描述符的可读/可写事件
 *   - 封装底层系统调用细节
 */
class IOMonitor {
public:
  IOMonitor();
  ~IOMonitor();

  // 禁止拷贝和移动
  IOMonitor(const IOMonitor &) = delete;
  IOMonitor &operator=(const IOMonitor &) = delete;

  /**
   * @brief 初始化监控器
   * @return true 成功, false 失败
   */
  bool Initialize();

  /**
   * @brief 关闭监控器，释放资源
   */
  void Close();

  /**
   * @brief 开始监控文件描述符的读事件
   * @param fd 要监控的文件描述符
   * @return true 成功, false 失败
   */
  bool StartReadMonitor(int fd);

  /**
   * @brief 开始监控文件描述符的写事件
   * @param fd 要监控的文件描述符
   * @return true 成功, false 失败
   */
  bool StartWriteMonitor(int fd);

  /**
   * @brief 停止监控文件描述符的写事件
   * @param fd 要停止监控的文件描述符
   * @return true 成功, false 失败
   */
  bool StopWriteMonitor(int fd);

  /**
   * @brief 停止监控文件描述符
   * @param fd 要停止监控的文件描述符
   * @return true 成功, false 失败
   */
  bool StopMonitor(int fd);

  /**
   * @brief 等待事件（阻塞）
   * @param events 事件数组
   * @param max_events 事件数组最大容量
   * @param timeout_ms 超时时间（毫秒），nullptr 表示无限等待
   * @return 返回的事件数量，-1 表示错误
   */
  int WaitEvents(void *events, int max_events, const int *timeout_ms);

  /**
   * @brief 获取监控器的文件描述符
   * @return kqueue_fd 或 epoll_fd
   */
  int GetFd() const { return monitor_fd_; }

private:
  int monitor_fd_; ///< kqueue_fd 或 epoll_fd
  int timeout_ms_; ///< 默认超时时间（毫秒）
};

} // namespace network
} // namespace darwincore

#endif // DARWINCORE_NETWORK_IO_MONITOR_H
