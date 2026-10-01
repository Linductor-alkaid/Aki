# Exercise the actual configure-time boundary, including source/patch drift.
set(_fixture "${BINARY_DIR}/eui-ime-patch-test")
file(REMOVE_RECURSE "${_fixture}")
file(MAKE_DIRECTORY "${_fixture}/third_party/EUI-NEO/3rd/glfw/src"
    "${_fixture}/docs/eui_neo_feedback/patches")
set(_source "third_party/EUI-NEO/3rd/glfw/src/x11_window.c")
set(_patch "docs/eui_neo_feedback/patches/EUI-20261001-002-filtered-x11-keys.patch")
configure_file("${SOURCE_DIR}/third_party/dependencies.lock.json"
    "${_fixture}/third_party/dependencies.lock.json" COPYONLY)
configure_file("${SOURCE_DIR}/${_source}" "${_fixture}/${_source}" COPYONLY)
configure_file("${SOURCE_DIR}/${_patch}" "${_fixture}/${_patch}" COPYONLY)
set(_project [=[
cmake_minimum_required(VERSION 3.25)
project(ime_patch_fixture LANGUAGES C)
find_package(Git REQUIRED)
set(GLFW_BUILD_X11 ON)
add_library(glfw STATIC "${PROJECT_SOURCE_DIR}/third_party/EUI-NEO/3rd/glfw/src/x11_window.c")
# Simulate GLFW's relative generated Wayland protocol source too.
file(WRITE "${PROJECT_BINARY_DIR}/wayland-client-protocol.h" "/* generated */\n")
set_source_files_properties(wayland-client-protocol.h PROPERTIES GENERATED TRUE)
target_sources(glfw PRIVATE wayland-client-protocol.h)
include("@SOURCE_DIR@/cmake/EuiX11ImePatch.cmake")
aki_apply_eui_x11_ime_patch()
get_target_property(_sources glfw SOURCES)
if(NOT _sources STREQUAL "${PROJECT_BINARY_DIR}/compat/eui-x11-ime/3rd/glfw/src/x11_window.c;${PROJECT_BINARY_DIR}/wayland-client-protocol.h")
    message(FATAL_ERROR "GLFW does not compile the generated source")
endif()
]=])
string(CONFIGURE "${_project}" _project @ONLY)
file(WRITE "${_fixture}/CMakeLists.txt" "${_project}")
function(configure_fixture expect_success error_text)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_fixture}" -B "${_fixture}/build"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(expect_success)
        if(NOT _result EQUAL 0)
            message(FATAL_ERROR "Patch fixture configure failed: ${_out}${_err}")
        endif()
    elseif(_result EQUAL 0 OR NOT "${_out}${_err}" MATCHES "${error_text}")
        message(FATAL_ERROR "Patch drift was not rejected: ${_out}${_err}")
    endif()
endfunction()
configure_fixture(TRUE "")
# Compile the actual patched dispatch block with a counting key callback.
# This catches consuming the timestamp of a filtered event, then losing an
# unfiltered forwarded Backspace carrying exactly the same timestamp.
file(READ "${_fixture}/build/compat/eui-x11-ime/3rd/glfw/src/x11_window.c" _patched)
string(FIND "${_patched}" "                Time diff =" _start)
string(SUBSTRING "${_patched}" ${_start} -1 _tail)
string(FIND "${_tail}" "\n                if (!filtered)" _end)
if(_start LESS 0 OR _end LESS 0)
    message(FATAL_ERROR "Cannot locate actual GLFW XIM dispatch block")
endif()
string(SUBSTRING "${_tail}" 0 ${_end} _dispatch)
set(_harness [=[
#include <assert.h>
#include <limits.h>
typedef unsigned long Time;
struct Window { struct { Time keyPressTimes[256]; } x11; } state;
struct Event { struct { Time time; } xkey; } input;
static unsigned callbacks;
#define GLFW_PRESS 1
#define _glfwInputKey(window, key, code, action, mods) (++callbacks)
static void press(int filtered, Time timestamp) {
    struct Window* window = &state;
    struct Event* event = &input;
    const int keycode = 22;
    event->xkey.time = timestamp;
@_dispatch@
}
int main(void) {
    press(1, 100);
    assert(callbacks == 0 && state.x11.keyPressTimes[22] == 0);
    press(0, 100); /* IBus forwards an unconsumed press */
    assert(callbacks == 1 && state.x11.keyPressTimes[22] == 100);
    press(0, 100); /* real duplicate must still be deduplicated */
    assert(callbacks == 1);
    press(1, 101); /* composition consumes the next press */
    assert(callbacks == 1 && state.x11.keyPressTimes[22] == 100);
    press(0, 102); /* ordinary key repeat */
    assert(callbacks == 2);
    state.x11.keyPressTimes[22] = ULONG_MAX - 1;
    press(0, 1); /* timestamp wrap */
    assert(callbacks == 3);
    return 0;
}
]=])
string(CONFIGURE "${_harness}" _harness @ONLY)
file(WRITE "${_fixture}/dispatch.c" "${_harness}")
file(APPEND "${_fixture}/CMakeLists.txt" "\nadd_executable(dispatch_test dispatch.c)\n")
configure_fixture(TRUE "")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${_fixture}/build" --target dispatch_test
    RESULT_VARIABLE _built OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _built EQUAL 0)
    message(FATAL_ERROR "Dispatch regression build failed: ${_out}${_err}")
endif()
execute_process(COMMAND "${_fixture}/build/dispatch_test" RESULT_VARIABLE _ran)
if(NOT _ran EQUAL 0)
    message(FATAL_ERROR "Actual GLFW dispatch block lost or duplicated a key event")
endif()
configure_fixture(TRUE "") # idempotent reconfigure, not double-application
file(SHA256 "${SOURCE_DIR}/${_source}" _original)
file(SHA256 "${_fixture}/${_source}" _retained)
if(NOT _original STREQUAL _retained)
    message(FATAL_ERROR "The pinned input source was modified")
endif()
file(APPEND "${_fixture}/${_patch}" "\n# drift\n")
configure_fixture(FALSE "source or patch hash mismatch")
configure_file("${SOURCE_DIR}/${_patch}" "${_fixture}/${_patch}" COPYONLY)
file(APPEND "${_fixture}/${_source}" "\n/* drift */\n")
configure_fixture(FALSE "source or patch hash mismatch")
configure_file("${SOURCE_DIR}/${_source}" "${_fixture}/${_source}" COPYONLY)
configure_fixture(TRUE "")
file(REMOVE_RECURSE "${_fixture}")
