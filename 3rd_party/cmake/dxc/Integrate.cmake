if(NOT MSVC OR NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "The runtime DXC integration requires Windows MSVC x64")
endif()
get_target_property(dxc_kind dxcompiler TYPE)
if(NOT dxc_kind STREQUAL "SHARED_LIBRARY")
  message(FATAL_ERROR "The pinned compiler must produce a runtime DLL")
endif()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS engine_dxc_compile=4 engine_dxc_link=1)
set(CMAKE_JOB_POOL_COMPILE engine_dxc_compile)
set(CMAKE_JOB_POOL_LINK engine_dxc_link)

# Compile the installed ATL base module with this compiler and CRT. Never
# import an ATL DLL or differently built atls.lib.
set(atl_base "${ENGINE_DXC_ATL_INCLUDE_DIR}/../src/atl/atls/atlbase.cpp")
if(NOT EXISTS "${atl_base}")
  message(FATAL_ERROR "The Visual Studio ATL source component is required: ${atl_base}")
endif()
target_sources(LLVMMSSupport PRIVATE "${atl_base}")
set_source_files_properties("${atl_base}" TARGET_DIRECTORY LLVMMSSupport
  PROPERTIES COMPILE_DEFINITIONS "_ATL_NO_COMMODULE")
