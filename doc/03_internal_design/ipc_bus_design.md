# IPC 总线内部设计方案 (草案)

## 1. 设计目标
*   **低延迟：** 满足网络协议状态同步的实时性要求。
*   **高可靠：** 支持 Broker 异常重启后的客户端自动重连。
*   **解耦：** 模块间无需知道彼此的进程 ID，仅通过 Topic 通信。

## 2. 通信模式
1.  **Publish/Subscribe (Pub/Sub):** 用于状态广播。例如端口 Up/Down 状态变更。
2.  **Request/Response (RPC):** 用于同步操作。例如 CLI 获取某个模块的统计信息。

## 3. 消息报文格式 (Header)

| 字段 | 长度 (Bytes) | 说明 |
| :--- | :--- | :--- |
| Magic Num | 2 | 协议识别码 (0x4950, "IP") |
| Version | 1 | 协议版本号 |
| Msg Type | 1 | 0: Pub, 1: Sub, 2: Req, 3: Ack |
| Topic ID | 4 | 预定义的业务主题 ID |
| Payload Len | 4 | 后续数据长度 |
| Sequence | 4 | 消息序列号 |

## 4. 关键流程
*   **注册：** 客户端连接 UDS 地址（如 `/tmp/nos_ipc.sock`），发送订阅请求。
*   **分发：** Broker 匹配 Topic，将 Pub 消息拷贝给所有 Sub 订阅者。
*   **清理：** 客户端断开时，Broker 自动清除其订阅信息。
