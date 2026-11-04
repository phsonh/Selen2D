# SheenBidi 3.0.0, Unicode 17.0.0. No experimental editing API or platform service.
get_filename_component(dependency_sheenbidi "${CMAKE_CURRENT_LIST_DIR}/../../sheenbidi" ABSOLUTE)
add_library(DependencySheenBidi STATIC "${dependency_sheenbidi}/Source/SheenBidi.c")
target_include_directories(DependencySheenBidi PUBLIC "${dependency_sheenbidi}/Headers" PRIVATE "${dependency_sheenbidi}/Source")
target_compile_definitions(DependencySheenBidi PRIVATE SB_CONFIG_UNITY)
if(MSVC)
  target_compile_options(DependencySheenBidi PRIVATE /utf-8)
endif()
