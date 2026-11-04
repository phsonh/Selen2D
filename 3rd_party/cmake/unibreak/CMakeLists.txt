# libunibreak 8.0, Unicode 17.0.0; generated upstream data is committed.
get_filename_component(dependency_unibreak "${CMAKE_CURRENT_LIST_DIR}/../../unibreak/src" ABSOLUTE)
add_library(DependencyUnibreak STATIC
  "${dependency_unibreak}/unibreakbase.c" "${dependency_unibreak}/unibreakdef.c"
  "${dependency_unibreak}/linebreak.c" "${dependency_unibreak}/linebreakdata.c"
  "${dependency_unibreak}/linebreakdef.c" "${dependency_unibreak}/eastasianwidthdef.c"
  "${dependency_unibreak}/emojidef.c" "${dependency_unibreak}/graphemebreak.c")
target_include_directories(DependencyUnibreak PUBLIC "${dependency_unibreak}")
if(MSVC)
  target_compile_options(DependencyUnibreak PRIVATE /utf-8)
endif()
