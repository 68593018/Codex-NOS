# 调度器深度设计 (Refined)

## 1. 动态服务注册表 (Service Registry)
调度器内部维护一个全局映射表：`Service ID -> {Thread ID, Component ID}`。
*   当组件调用 `nos_service_register_provider` 时，向表里填入信息。
*   当调用 `nos_service_msg_send(msg)` 时，根据 `msg->dst_service` 查表，确定目标线程和组件。

## 2. 线程私有消息队列 (Thread-Local Queue)
每个 `nos_thread_t` 拥有独立的：
*   **Message Queue**：存放发给该线程下属组件的消息。
*   **Notify FD**：一个 `eventfd` 或 `pipe`，用于唤醒 `epoll_wait`。
*   **Epoll FD**：管理该线程关心的所有事件源。

## 3. 核心调度算法 (Run Loop v2)
```c
while (running) {
    // 1. 阻塞等待事件发生 (消息通知、定时器、Socket)
    int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    
    for (int n = 0; n < nfds; ++n) {
        if (events[n].data.fd == notify_fd) {
            // 2. 消息到达事件：清空 notify_fd，处理消息队列
            consume_notification();
            process_msg_queue();
        } else {
            // 3. 其他 I/O 事件：分发给对应的组件 on_event 回调
            dispatch_io_event(events[n]);
        }
    }
}
```

## 4. 线程间通信 (Cross-Thread Posting)
当线程 A 的组件向线程 B 的服务发消息时：
1.  获取线程 B 的锁，将消息挂入 B 的私有队列。
2.  向线程 B 的 `notify_fd` 写入一个字节（唤醒）。
3.  线程 B 从 `epoll_wait` 醒来并处理。
