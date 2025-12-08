# UnrealAgentLink 实现记录（当前进度）

文档日期：2025-12-08  
适用版本：UE 5.0-5.7（单代码适配）

## 已完成功能
- 网络层 `FUAL_NetworkManager`
  - WebSocket 客户端，默认连接 `ws://127.0.0.1:17860`。
  - 线程安全发送、自动重连（FTSTicker），Socket 线程回调通过委托转交。
- 指令层 `FUAL_CommandHandler`
  - 在 GameThread 解析 JSON 请求。
  - 支持 `cmd.run_python`（依赖 PythonScriptPlugin）与 `cmd.exec_console`。
  - 统一构造响应 `{"type":"res","code":200/500,...}`。
- 日志层 `FUAL_LogInterceptor`
  - 拦截 UE 日志，生成事件 `{"type":"evt","method":"log.entry",...}` 推送到 Agent。
  - 兼容 UE5.0 的 VerbosityToString 与 5.1+ 的 FLogVerbosity::ToString。
- 模块装配
  - 启动时初始化网络/指令/日志组件并注册菜单按钮。
  - 关闭时移除委托与输出设备，安全清理。
- 构建配置
  - Build.cs 添加 Json/JsonUtilities/WebSockets/PythonScriptPlugin 依赖。
  - uplugin 声明 PythonScriptPlugin 依赖并启用。

## 关键文件
- `Source/UnrealAgentLink/Public/UAL_NetworkManager.h` & `.cpp`
- `Source/UnrealAgentLink/Public/UAL_CommandHandler.h` & `.cpp`
- `Source/UnrealAgentLink/Public/UAL_LogInterceptor.h` & `.cpp`
- `Source/UnrealAgentLink/Private/UnrealAgentLink.cpp`
- `Source/UnrealAgentLink/UnrealAgentLink.Build.cs`
- `UnrealAgentLink.uplugin`

## 线程与调用约束
- WebSocket 线程严禁直接调用 UE API；收到消息后用 `AsyncTask(ENamedThreads::GameThread, ...)` 切回主线程。
- 日志上报在 GameThread 序列化并发送，避免跨线程资源访问。

## 使用与验证建议
1. 启动外部 Agent WebSocket 服务（确保端口与 DefaultUrl 一致，默认 17860）。
2. 打开 UE 编辑器，加载插件；菜单按钮将弹出连接状态。
3. 从 Agent 发送指令示例：
   - `cmd.exec_console`：`{"type":"req","id":"uuid","method":"cmd.exec_console","payload":{"command":"stat fps"}}`
   - `cmd.run_python`：`{"type":"req","id":"uuid","method":"cmd.run_python","payload":{"script":"print('Hello')"}}`
4. 期望返回 `type=res`，`code=200`；日志将以 `type=evt`、`method=log.entry` 推送。

## 后续改进清单
- DefaultUrl 改为可配置（Editor 设置或控制台命令）。
- Python 输出捕获与返回（当前仅布尔状态）。
- 心跳 Ping/Pong 报文格式与重连间隔可配置。
- 增加自动化测试或示例关卡验证流程。

