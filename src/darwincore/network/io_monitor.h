//
// DarwinCore Network Module
// IOMonitor - IO 事件监控封装（macOS kqueue）
//
// Description:
//   封装 kqueue 系统调用，提供 IO 事件监控接口。
//   仅支持 macOS/BSD 平台。
//
// Author: DarwinCore Network Team
// Date: 2026

#ifndef DARWINCORE_NETWORK_IO_MONITOR_H
#define DARWINCORE_NETWORK_IO_MONITOR_H

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <sys/event.h>

namespace darwincore
{
  namespace network
  {

    /**
     * @brief IO 监控器封装类（内部使用）
     *
     * 提供 macOS kqueue 的 IO 事件监控接口。
     *
     * 用途：
     *   - 监控文件描述符的可读/可写事件
     *   - 封装底层 kqueue 系统调用细节
     */
    class IOMonitor
    {
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
       * @brief 停止监控文件描述符的读事件
       * @param fd 要停止监控的文件描述符
       * @return true 成功, false 失败
       */
      bool StopReadMonitor(int fd);

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
       * @brief 初始化用户态唤醒事件（EVFILT_USER）
       * @param ident 用户事件标识
       * @return true 成功, false 失败
       */
      bool InitializeWakeupEvent(uintptr_t ident);

      /**
       * @brief 触发唤醒事件
       * @param ident 用户事件标识
       * @return true 成功, false 失败
       */
      bool TriggerWakeupEvent(uintptr_t ident);

      /**
       * @brief 等待事件（阻塞）
       * @param events kevent 事件数组
       * @param max_events 事件数组最大容量
       * @param timeout_ms 超时时间（毫秒），nullptr 表示无限等待
       * @return 返回的事件数量，-1 表示错误
       */
      int WaitEvents(struct kevent *events, int max_events, const int *timeout_ms);

      /**
       * @brief 获取监控器的文件描述符
       * @return kqueue_fd
       */
      int GetFd() const { return kqueue_fd_; }

    private:
      int kqueue_fd_; ///< kqueue 文件描述符
    };

  } // namespace network
} // namespace darwincore

#endif // DARWINCORE_NETWORK_IO_MONITOR_H
