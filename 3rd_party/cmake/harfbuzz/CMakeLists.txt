# HarfBuzz 12.3.0; upstream's unity translation unit, without optional platform backends.
get_filename_component(dependency_harfbuzz "${CMAKE_CURRENT_LIST_DIR}/../../harfbuzz" ABSOLUTE)
add_library(DependencyHarfBuzz STATIC "${dependency_harfbuzz}/src/harfbuzz.cc")
target_include_directories(DependencyHarfBuzz PUBLIC "${dependency_harfbuzz}/src")
target_compile_features(DependencyHarfBuzz PRIVATE cxx_std_17)
target_compile_definitions(DependencyHarfBuzz PUBLIC HB_STATIC PRIVATE HB_NO_SUBSET
  HB_BUFFER_MAX_LEN_FACTOR=0 HB_BUFFER_MAX_LEN_MIN=262144 HB_BUFFER_MAX_LEN_DEFAULT=262144
  HB_BUFFER_MAX_OPS_FACTOR=0 HB_BUFFER_MAX_OPS_MIN=8388608 HB_BUFFER_MAX_OPS_DEFAULT=8388608)
if(MSVC)
  target_compile_options(DependencyHarfBuzz PRIVATE /utf-8 /bigobj)
endif()
