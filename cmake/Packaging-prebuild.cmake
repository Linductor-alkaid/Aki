# CPack pre-build 裁剪脚本（M6-01，DEC-017）：从临时安装树移除 pinned
# heyaki 无条件 install() 规则带入的开发产物（lib/ 静态库、include/ 头文件、
# share/heyaki 的 proto/coturn/supply-chain 合规审计面）。share/licenses/
# 保留（第三方许可文本随二进制分发的合规要求）。
#
# 安装树根随生成器不同：DEB 含 CPACK_PACKAGING_INSTALL_PREFIX（/opt/aki），
# NSIS 直接映射安装根；对两者同时探测，EXISTS 守卫保证幂等。
# 本脚本由 CPACK_PRE_BUILD_SCRIPTS 调用，可用变量以 CPack 文档为准
#（CPACK_TEMPORARY_INSTALL_DIRECTORY 在此阶段已就位）。

set(_aki_prune_dirs lib include share/heyaki)
foreach(_aki_root
        "${CPACK_TEMPORARY_INSTALL_DIRECTORY}"
        "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/opt/aki")
    foreach(_aki_dir IN LISTS _aki_prune_dirs)
        if(EXISTS "${_aki_root}/${_aki_dir}")
            file(REMOVE_RECURSE "${_aki_root}/${_aki_dir}")
        endif()
    endforeach()
endforeach()
