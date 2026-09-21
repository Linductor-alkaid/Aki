add_executable(aki_skeleton_test test_skeleton.cpp)
target_compile_features(aki_skeleton_test PRIVATE cxx_std_20)
target_link_libraries(aki_skeleton_test PRIVATE
    aki_app
    aki_device
    aki_conversation
    aki_transfer
    aki_persistence
    aki_heyaki
    aki_ui)

add_test(NAME skeleton.includes COMMAND aki_skeleton_test)
set_tests_properties(skeleton.includes PROPERTIES LABELS "unit")

add_test(NAME skeleton.app_runs COMMAND aki)
set_tests_properties(skeleton.app_runs PROPERTIES
    LABELS "smoke"
    PASS_REGULAR_EXPRESSION "aki 0\\.1\\.0")
