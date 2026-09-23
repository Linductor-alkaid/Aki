# 依赖锁定校验（docs/decisions/DEC-003-dependency-locking.md，工程规范第 9.1 节）。
# third_party/dependencies.lock.json 登记全部依赖；configure 时按 class 校验：
#   - pinned：校验每个 submodule 的 HEAD 与锁定 commit 一致，缺失或漂移即失败；
#   - vendored（DEC-004，M2-02 起支持）：非 git 依赖，逐文件 file(SHA256) 比对
#     锁定哈希，缺失或漂移即 FATAL_ERROR 并附修复提示。
# 目标级接入分别在 M1（executor）、M2（sqlite）、M3（heyaki）、M5（EUI-NEO）的
# 里程碑内实施，本文件只做校验。

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
    string(JSON _class GET "${_aki_lock}" "dependencies" "${_i}" "class")

    set(_abs "${PROJECT_SOURCE_DIR}/${_path}")
    if(NOT EXISTS "${_abs}")
        message(FATAL_ERROR
            "Dependency '${_name}' is missing at '${_path}'. "
            "See third_party/dependencies.lock.json "
            "(${_class} entry, url: ${_url}).")
    endif()

    if(_class STREQUAL "pinned")
        string(JSON _commit GET "${_aki_lock}" "dependencies" "${_i}" "commit")

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

        # DEC-006（M3-01，对 DEC-003 的修订条款）：executor 自 M3-01 起经 heyaki
        # 单图间接进入构建（heyaki third_party/executor 提供构建图内的 executor
        # target，双侧同 pin）。configure 校验扩展为三方一致：Aki lock /
        # heyaki lock / heyaki checkout——任一侧升级即破坏单图单副本前提。
        if(_name STREQUAL "executor")
            set(_heyaki_lock_path
                "${PROJECT_SOURCE_DIR}/third_party/heyaki/third_party/dependencies.lock")
            if(NOT EXISTS "${_heyaki_lock_path}")
                message(FATAL_ERROR
                    "Heyaki dependency lock is missing at "
                    "third_party/heyaki/third_party/dependencies.lock "
                    "(submodule not initialized). Run: git submodule update --init")
            endif()
            file(READ "${_heyaki_lock_path}" _heyaki_lock_text)
            # heyaki lock 行格式：name|url|ref|commit|recursive|group（[|] 为
            # 字符类形式的字面管道符——CMake regex 中 \| 会被当作择一运算符）。
            string(REGEX MATCH "executor[|][^\r\n]*" _heyaki_executor_line
                "${_heyaki_lock_text}")
            if(_heyaki_executor_line STREQUAL "")
                message(FATAL_ERROR
                    "Heyaki lock has no executor entry; cannot verify the "
                    "single-graph executor pin (DEC-006).")
            endif()
            string(REPLACE "|" ";" _heyaki_executor_fields "${_heyaki_executor_line}")
            list(GET _heyaki_executor_fields 3 _heyaki_executor_commit)
            if(NOT _heyaki_executor_commit STREQUAL _commit)
                message(FATAL_ERROR
                    "Executor pin mismatch across the single build graph (DEC-006): "
                    "Aki lock requires ${_commit}, heyaki lock requires "
                    "${_heyaki_executor_commit}. Align both lock files before "
                    "configuring.")
            endif()

            set(_heyaki_executor_dir
                "${PROJECT_SOURCE_DIR}/third_party/heyaki/third_party/executor")
            if(NOT EXISTS "${_heyaki_executor_dir}/.git")
                message(FATAL_ERROR
                    "Heyaki's executor checkout is missing at "
                    "third_party/heyaki/third_party/executor. "
                    "Run: bash third_party/heyaki/scripts/fetch_third_party.sh "
                    "(ref+commit verified, DEC-006).")
            endif()
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" -C "${_heyaki_executor_dir}" rev-parse HEAD
                RESULT_VARIABLE _heyaki_rev_result
                OUTPUT_VARIABLE _heyaki_executor_head
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(NOT _heyaki_rev_result EQUAL 0 OR
               NOT _heyaki_executor_head STREQUAL _commit)
                message(FATAL_ERROR
                    "Heyaki's executor checkout is at "
                    "'${_heyaki_executor_head}', but ${_commit} is required by both "
                    "lock files (single-graph single-copy premise, DEC-006). "
                    "Run: bash third_party/heyaki/scripts/fetch_third_party.sh")
            endif()

            message(STATUS "Executor pin verified three-way consistent "
                "(Aki lock / heyaki lock / checkout @ ${_commit})")
        endif()

        message(STATUS "Dependency '${_name}' pinned at ${_commit}")
    elseif(_class STREQUAL "vendored")
        string(JSON _version GET "${_aki_lock}" "dependencies" "${_i}" "version")
        string(JSON _file_count LENGTH "${_aki_lock}" "dependencies" "${_i}" "files")
        math(EXPR _members_last "${_file_count} - 1")
        foreach(_j RANGE ${_members_last})
            # MEMBER 按索引取 files 对象的键名。
            string(JSON _member MEMBER "${_aki_lock}" "dependencies" "${_i}" "files" "${_j}")
            set(_file "${_abs}/${_member}")
            if(NOT EXISTS "${_file}")
                message(FATAL_ERROR
                    "Vendored dependency '${_name}' (${_version}) is missing file "
                    "'${_path}/${_member}'. Restore it from the official artifact "
                    "recorded in third_party/dependencies.lock.json (source: ${_url}). "
                    "See docs/decisions/DEC-004-local-persistence-sqlite.md.")
            endif()
            file(SHA256 "${_file}" _actual_sha256)
            string(JSON _expected_sha256 GET "${_aki_lock}" "dependencies" "${_i}" "files" "${_member}")
            if(NOT _actual_sha256 STREQUAL _expected_sha256)
                message(FATAL_ERROR
                    "Vendored dependency '${_name}' (${_version}): file "
                    "'${_path}/${_member}' has SHA-256 ${_actual_sha256}, but "
                    "${_expected_sha256} is required by "
                    "third_party/dependencies.lock.json. Restore the official file "
                    "(source: ${_url}); see docs/decisions/DEC-004-local-persistence-sqlite.md.")
            endif()
        endforeach()

        # 暴露 vendored 版本供 sourceid 断言等消费（当前仅 sqlite 一个 vendored 条目）。
        if(_name STREQUAL "sqlite")
            set(AKI_SQLITE_LOCK_VERSION "${_version}")
        endif()

        message(STATUS
            "Dependency '${_name}' vendored at ${_path} "
            "(version ${_version}, ${_file_count} files verified)")
    else()
        message(FATAL_ERROR
            "Dependency '${_name}' in third_party/dependencies.lock.json has unknown "
            "class '${_class}' (expected 'pinned' or 'vendored').")
    endif()
endforeach()
