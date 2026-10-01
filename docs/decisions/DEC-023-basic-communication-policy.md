# DEC-023：基础通信与设备控制授权分开

> 状态：Accepted
> 日期：2026-09-30
> 负责人：Linductor
> 关联工作项：M5-30、M5-34
> 修订：DEC-019、DEC-021、DEC-022 的基础通信产品目标；当前 pinned 实现限制仍有效

## 背景

用户明确设备信任应服务于后续 shell 等设备控制；消息、图片与普通文件
希望只需已连接即可通信。当前 Heyaki 将所有业务通道放在设备 grant 后，
Aki 去掉界面门控仍不能发送或接收未授权业务帧。

## 决策

目标策略：双方完成密码学身份验证并建立连接后，应用明确启用基础
通信能力，可收发消息和向指定 inbox 传文件；不因此生成持久化 TrustGrant，
不将设备状态改成 Trusted。接收根、路径校验、大小/配额、队列容量与
速率限制继续由服务端强制执行。shell、RPC 控制、gateway、远程目录读取
等高级能力继续独立要求相应 capability、scope 与设备授权。

Heyaki 的默认策略保持兼容；Aki 需通过公开配置选择基础通信策略，双端
通道创建、接纳和业务帧验证都必须落实相同边界。仅有 LAN 在线发现不
满足条件，身份未验证/断连仍禁止发送。不得自动签发包含控制能力的 grant。

按用户要求，Heyaki 独立管理，通过反馈台账与 issue 提出公开能力；本轮
不修改 pinned 依赖。上游提供 API、测试并固定版本后再在 Aki Adapter
接入。此决策批准产品边界，不代表当前免信任通信已实现。

## 验收与风险

上游需验证：无 grant 双端基础通信、断连/取消/关闭、拒绝策略、身份伪造、
目录穿越、配额/背压，以及基础通道无法打开 shell/控制/gateway。Aki
后续验证 Unknown/Pending/Rejected/Revoked 与控制授权分别表现、失败可见
和双设备实际文本/图片/文件传输。公开配置缺口由 HEY-20260930-003 跟踪。

## 2026-10-01 接入依据

Heyaki v1.1.1 提供公开 basic_communication opt-in 配置及独立 policy_scopes。
Aki 组合根启用该配置，低层 NodeSession 的缺省仍关闭；不自动配对或签发
grant。配置既有 inbox 接收根，保持高级服务默认关闭。已配对会话仍按
实际 grant 的有效 scopes 执行，基础策略不扩充设备授权。验收归 M5-34。


## 2026-10-02 首次建链与信任方向修订

M5-41 补齐应用接线：连接对账接纳具有完整身份公钥、目录可见且尚未建链
的所有非本机设备，不依赖 TrustState。Unknown/Rejected/Revoked 不阻塞
基础通信；自动建链不生成 grant，终态信任只由用户显式重新配对回到 Pending。
沿现有 peer 观察 timer 采样有效 issued/received grant 方向，变化经 Manager
收件箱校准；不会把 authenticated 或 policy_scopes 当作设备信任事实。
UI 仍按连接路径开放会话；断连禁止发送，控制服务配置保持关闭。
