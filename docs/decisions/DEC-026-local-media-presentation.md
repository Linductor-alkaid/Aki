# DEC-026：本地归档文件与图片呈现

> 状态：Accepted；日期：2026-10-01；负责人：Linductor；工作项 M5-38/39。

当前 UI 只记本次会话的出站源路径，未读取接收归档；文件位置不展示。
改为 Persistence 完成归档后，沿既有 DatabaseWorker 与 AppStateOwner
MpscChannel 发布 LocalTransferArtifact（TransferId、相对路径、哈希、大小），
容量随传输预算。失败与关闭通过既有作业 future/通信统计可见。Core 与
wire 不携带本地路径；UI 只消费 Application State，无 compose 内文件 IO。

启动恢复从 TRANSFER 的 stored_* 回写位恢复本地记录，并在恢复期核对
文件存在性。只有已就位归档才宣告可用；传输完成但归档未回写显示准备中。
同一次出站可用用户选取源即时显示；历史/入站走持久归档。图片气泡默认
显示 contain 缩略图，点击放大；文件名、传输状态与本地路径可见，文件
所在目录通过 Platform Adapter 交给系统文件管理器，错误明示，不执行传入文件。路径校验在边界拒绝
绝对路径、穿越及 TransferId 与归档路径不一致的输入。

正常 shutdown 先停止业务生产者；已有 DB 完成回写若遇到 AppState
关闭，拒绝统计仍可见，不重新打开状态；归档可在下次恢复收敛。
验证需覆盖归档完成/失败/缺源、乱序、重启、容量、关闭中的投递，以及
Linux/Windows 双向真实图片。未执行平台验证不得标完成。

归档失败通过 SetLocalTransferArtifactFailure 发布（错误 ≤512 字节），
作业 future 同时失败，迟到错误不得覆盖成功文件。启动时 Completed 行没有
stored_* 记录显示保存失败；已存记录但磁盘文件缺失显示不存在。恢复记录
超过默认 256 预算时明确启动失败，避免引入无界本地事实集合。
目录打开是 UI 点击中的原生桌面交接：Windows ShellExecuteW，Linux GIO，
不创建 Aki worker，不经 shell、不管理系统文件管理器生命周期。
