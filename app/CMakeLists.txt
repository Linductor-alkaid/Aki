# app 层（设计第 8/10/14 节）：Application State 单写者边界与事件模型（M1-02）。
# 头文件接口库；executor::comm 是跨上下文交付的指定设施（EXEC-03），
# 经 PUBLIC 依赖向 app 消费方（tests、后续 Manager 与 UI 宿主）传播。
aki_add_skeleton_library(aki_app)
target_link_libraries(aki_app INTERFACE executor::executor)
