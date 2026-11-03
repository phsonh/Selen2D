# DXC uses ATL smart-pointer/value templates. Disable ATL's blanket atls.lib
# autolink; Integrate.cmake builds its base module with the selected CRT.
find_path(ENGINE_DXC_ATL_INCLUDE_DIR atlbase.h
    HINTS "$ENV{VCToolsInstallDir}/atlmfc/include")
if(NOT ENGINE_DXC_ATL_INCLUDE_DIR AND DEFINED ENV{VCINSTALLDIR})
    file(GLOB atl_candidates "$ENV{VCINSTALLDIR}/Tools/MSVC/*/atlmfc/include")
    list(SORT atl_candidates COMPARE NATURAL ORDER DESCENDING)
    find_path(ENGINE_DXC_ATL_INCLUDE_DIR atlbase.h HINTS ${atl_candidates})
endif()
if(NOT ENGINE_DXC_ATL_INCLUDE_DIR)
    message(FATAL_ERROR "DXC requires Visual Studio ATL headers; install the C++ ATL component or set ENGINE_DXC_ATL_INCLUDE_DIR")
endif()
include_directories(SYSTEM "${ENGINE_DXC_ATL_INCLUDE_DIR}")
add_definitions(-D_ATL_NO_DEFAULT_LIBS)
