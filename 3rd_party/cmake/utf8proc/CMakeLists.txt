# utf8proc v2.12.0, Unicode 18.0.0. Fixed vendored build; no downloads.
set(dependency_utf8proc_root "${CMAKE_CURRENT_LIST_DIR}/../../utf8proc")
add_library(DependencyUtf8proc STATIC "${dependency_utf8proc_root}/utf8proc.c")
target_include_directories(DependencyUtf8proc PUBLIC "${dependency_utf8proc_root}")
target_compile_definitions(DependencyUtf8proc PUBLIC UTF8PROC_STATIC)
