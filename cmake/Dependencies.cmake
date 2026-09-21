# 依赖锁定校验（docs/decisions/DEC-003-dependency-locking.md，工程规范第 9.1 节）。
# third_party/dependencies.lock.json 登记全部 pinned 依赖；configure 时校验每个
# submodule 的 HEAD 与锁定 commit 一致，缺失或漂移即失败。目标级接入分别在
# M1（executor）、M3（heyaki）、M5（EUI-NEO）的里程碑内实施，本文件只做校验。

set(AKI_DEPENDENCIES_LOCK "${PROJECT_SOURCE_DIR}/third_party/dependencies.lock.json")

if(NOT EXISTS "${AKI_DEPENDENCIES_LOCK}")
    message(FATAL_ERROR
        "third_party/dependencies.lock.json is missing; "
        "see docs/decisions/DEC-003-dependency-locking.md")
endif()

find_package(Git REQUIRED)

file(READ "${AKI_DEPENDENCIES_LOCK}" _aki_lock)
string(JSON _aki_count LENGTH "${_aki_lock}" "dependencies")
math(EXPR _aki_last "${_aki_count} - 1")

foreach(_i RANGE ${_aki_last})
    string(JSON _name GET "${_aki_lock}" "dependencies" "${_i}" "name")
    string(JSON _path GET "${_aki_lock}" "dependencies" "${_i}" "path")
    string(JSON _url GET "${_aki_lock}" "dependencies" "${_i}" "url")
    string(JSON _commit GET "${_aki_lock}" "dependencies" "${_i}" "commit")

    set(_abs "${PROJECT_SOURCE_DIR}/${_path}")
    if(NOT EXISTS "${_abs}")
        message(FATAL_ERROR
            "Pinned dependency '${_name}' is missing at '${_path}'. "
            "Run: git submodule update --init ${_path}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_abs}" rev-parse HEAD
        RESULT_VARIABLE _rev_result
        OUTPUT_VARIABLE _head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT _rev_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to read HEAD of pinned dependency '${_name}' (${_path}).")
    endif()

    if(NOT _head STREQUAL _commit)
        message(FATAL_ERROR
            "Pinned dependency '${_name}' is at ${_head}, but ${_commit} is required "
            "by third_party/dependencies.lock.json. "
            "Run: git -C ${_path} checkout ${_commit}")
    endif()

    message(STATUS "Dependency '${_name}' pinned at ${_commit}")
endforeach()
