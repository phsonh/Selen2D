cmake_minimum_required(VERSION 3.25)

# Reject non-Release calls before inspecting paths, taking locks, or writing
# anything. This also protects direct cmake -P invocations outside the target.
if(NOT DEFINED CONFIGURATION OR NOT CONFIGURATION STREQUAL "Release")
    message(FATAL_ERROR "Formal release staging requires CONFIGURATION=Release; Debug builds cannot publish")
endif()

foreach(argument IN ITEMS SOURCE_ROOT ENGINE_EXECUTABLE DXC_RUNTIME_FILE LEGACY_PACK_FILENAME RELEASE_NAME DESTINATION CONFIGURATION DUMPBIN)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "Release staging requires ${argument}")
    endif()
endforeach()
if(NOT RELEASE_NAME MATCHES "^[a-z0-9][a-z0-9_.-]*-windows-x64$" OR
        NOT CONFIGURATION MATCHES "^[A-Za-z0-9_-]+$" OR
        NOT LEGACY_PACK_FILENAME MATCHES "^[A-Za-z0-9_-]+\\.exe$")
    message(FATAL_ERROR "Unsafe release or legacy packer name")
endif()
file(REAL_PATH "${SOURCE_ROOT}" SOURCE_ROOT)
get_filename_component(runtime_bin "${ENGINE_EXECUTABLE}" DIRECTORY)
get_filename_component(runtime_filename "${ENGINE_EXECUTABLE}" NAME)
if(NOT runtime_filename MATCHES "^[A-Za-z0-9_-]+\\.exe$")
    message(FATAL_ERROR "Unsafe runtime executable name")
endif()

# Check resolved absolute paths before creating output or moving legacy files.
# In particular, reject junctions/symlinks that redirect releases or build.
include("${CMAKE_CURRENT_LIST_DIR}/CopyNotices.cmake")
function(require_exact_path candidate expected)
    engine_require_generated_path("${candidate}" "${expected}")
endfunction()
set(releases_root "${SOURCE_ROOT}/releases")
set(work_root "${SOURCE_ROOT}/build/release-work")
require_exact_path("${releases_root}" "${releases_root}")
require_exact_path("${SOURCE_ROOT}/build" "${SOURCE_ROOT}/build")
require_exact_path("${work_root}" "${work_root}")
require_exact_path("${DESTINATION}" "${releases_root}/${RELEASE_NAME}")
require_exact_path("${releases_root}/${RELEASE_NAME}.zip" "${releases_root}/${RELEASE_NAME}.zip")

set(shell_files main.lua example/main.lua example/effect.hlsl example/palette.hlsl
    example/assets/NotoSans-Regular.ttf example/assets/OFL.txt example/assets/SOURCE.md
    example/assets/pixel.png example/assets/sprite.png example/assets/bell.wav)
set(guide_files README.md getting-started.md shader.md runtime.md input-window.md file-launch.md debug-console.md tools-gui.md
    resource.md audio.md fractional-images.md text.md example.md)
set(api_modules Audio Debug File Graphic Input Math Object Resource Text Time Window)
set(api_files runtime/Runtime.lua)
set(legacy_api_files doc/api/Runtime.d.lua)
foreach(module IN LISTS api_modules)
    list(APPEND api_files "modules/${module}.lua")
    list(APPEND legacy_api_files "doc/api/${module}.d.lua")
endforeach()
foreach(required IN ITEMS "${ENGINE_EXECUTABLE}" "${DXC_RUNTIME_FILE}" "${DUMPBIN}" "${SOURCE_ROOT}/CHANGELOG.md")
    if(NOT EXISTS "${required}" OR IS_DIRECTORY "${required}")
        message(FATAL_ERROR "Required release input is missing: ${required}")
    endif()
endforeach()
foreach(relative IN LISTS shell_files)
    if(NOT EXISTS "${SOURCE_ROOT}/src/res/development/${relative}")
        message(FATAL_ERROR "Missing development-shell source: ${relative}")
    endif()
endforeach()
foreach(relative IN LISTS guide_files)
    if(NOT EXISTS "${SOURCE_ROOT}/doc/guide/${relative}")
        message(FATAL_ERROR "Missing guide source: ${relative}")
    endif()
endforeach()
foreach(relative IN LISTS api_files)
    if(NOT EXISTS "${SOURCE_ROOT}/doc/api/${relative}")
        message(FATAL_ERROR "Missing API declaration: ${relative}")
    endif()
endforeach()

# Inspect the actual linked PE, not a configuration flag or the DLLs lying in
# bin. System DLL imports remain normal Windows dependencies; private compiler
# import entries and dynamic MSVC runtimes are forbidden in the executable.
# The explicitly loaded source-built DLL is inspected separately below.
execute_process(COMMAND "${DUMPBIN}" /NOLOGO /DEPENDENTS "${ENGINE_EXECUTABLE}"
    RESULT_VARIABLE dependency_result OUTPUT_VARIABLE dependency_output ERROR_VARIABLE dependency_error)
if(NOT dependency_result EQUAL 0)
    message(FATAL_ERROR "Cannot inspect executable dependencies: ${dependency_error}")
endif()
string(REGEX MATCHALL "[A-Za-z0-9_.-]+\\.[dD][lL][lL]" imported_dlls "${dependency_output}")
list(REMOVE_DUPLICATES imported_dlls)
if(NOT imported_dlls)
    message(FATAL_ERROR "No Windows PE imports were reported; dependency inspection did not succeed")
endif()
foreach(dependency IN LISTS imported_dlls)
    string(TOLOWER "${dependency}" lower_dependency)
    if(lower_dependency MATCHES "^(dxcompiler|dxil|msvcp.*|vcruntime.*|concrt.*|ucrtbase.*)\\.dll$" OR
            lower_dependency MATCHES "^api-ms-win-crt-")
        message(FATAL_ERROR "Release executable still imports a private runtime: ${dependency}")
    endif()
    if(NOT lower_dependency MATCHES "^(api-ms-|ext-ms-)" AND
            NOT EXISTS "$ENV{SystemRoot}/System32/${dependency}")
        message(FATAL_ERROR "Release executable has a non-system DLL dependency: ${dependency}")
    endif()
endforeach()

file(MAKE_DIRECTORY "${work_root}" "${releases_root}")
# Serialize explicit Release publishers; keep the lock outside the package.
file(LOCK "${work_root}/${RELEASE_NAME}.lock" GUARD PROCESS TIMEOUT 120)
string(TIMESTAMP staged_at "%Y%m%dT%H%M%SZ" UTC)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(workspace "${work_root}/${RELEASE_NAME}-${CONFIGURATION}-${staged_at}-${nonce}")
require_exact_path("${workspace}" "${workspace}")
if(EXISTS "${workspace}")
    message(FATAL_ERROR "Fresh staging directory already exists: ${workspace}")
endif()
set(staging "${workspace}/package")
file(MAKE_DIRECTORY "${staging}")

include("${CMAKE_CURRENT_LIST_DIR}/VerifyDxcRuntime.cmake")
engine_verify_dxc_runtime("${DXC_RUNTIME_FILE}" "${DUMPBIN}" "${workspace}/verification" "${CONFIGURATION}")
file(WRITE "${workspace}/verification/executable-dependents.txt" "${dependency_output}")

engine_copy_notices("${SOURCE_ROOT}" "${runtime_bin}")

function(copy_release_file source destination)
    if(NOT EXISTS "${source}" OR IS_DIRECTORY "${source}" OR IS_SYMLINK "${source}")
        message(FATAL_ERROR "Release input must be an ordinary file: ${source}")
    endif()
    get_filename_component(parent "${destination}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    file(COPY_FILE "${source}" "${destination}" ONLY_IF_DIFFERENT)
endfunction()

copy_release_file("${ENGINE_EXECUTABLE}" "${staging}/${runtime_filename}")
copy_release_file("${SOURCE_ROOT}/CHANGELOG.md" "${staging}/doc/guide/CHANGELOG.md")
# Keep one maintained changelog at the repository root. Relocate only its
# documented relative links when publishing it beside the offline guides.
file(READ "${staging}/doc/guide/CHANGELOG.md" changelog_text)
string(REPLACE "](doc/guide/" "](" changelog_text "${changelog_text}")
file(WRITE "${staging}/doc/guide/CHANGELOG.md" "${changelog_text}")
copy_release_file("${DXC_RUNTIME_FILE}" "${staging}/dxcompiler.dll")
copy_release_file("${DXC_RUNTIME_FILE}" "${runtime_bin}/dxcompiler.dll")
file(SHA256 "${staging}/dxcompiler.dll" staged_dxc_hash)
file(SHA256 "${runtime_bin}/dxcompiler.dll" deployed_dxc_hash)
if(NOT staged_dxc_hash STREQUAL ENGINE_DXC_RUNTIME_SHA256 OR
        NOT deployed_dxc_hash STREQUAL ENGINE_DXC_RUNTIME_SHA256)
    message(FATAL_ERROR "DXC runtime changed between inspection and staging")
endif()
foreach(relative IN LISTS shell_files)
    copy_release_file("${SOURCE_ROOT}/src/res/development/${relative}" "${staging}/${relative}")
    copy_release_file("${SOURCE_ROOT}/src/res/development/${relative}" "${runtime_bin}/${relative}")
endforeach()
foreach(relative IN LISTS guide_files)
    copy_release_file("${SOURCE_ROOT}/doc/guide/${relative}" "${staging}/doc/guide/${relative}")
    file(READ "${staging}/doc/guide/${relative}" guide_text)
    string(REPLACE "](../../CHANGELOG.md)" "](CHANGELOG.md)" guide_text "${guide_text}")
    file(WRITE "${staging}/doc/guide/${relative}" "${guide_text}")
endforeach()
foreach(relative IN LISTS api_files)
    copy_release_file("${SOURCE_ROOT}/doc/api/${relative}" "${staging}/doc/api/${relative}")
endforeach()
copy_release_file("${runtime_bin}/licenses/project/LICENSE" "${staging}/doc/guide/LICENSE")
copy_release_file("${runtime_bin}/licenses/project/THIRD_PARTY_NOTICES.txt" "${staging}/doc/guide/THIRD_PARTY_NOTICES.txt")
foreach(relative IN LISTS ENGINE_NOTICE_FILES)
    copy_release_file("${runtime_bin}/licenses/${relative}" "${staging}/doc/guide/licenses/${relative}")
endforeach()
file(GLOB_RECURSE package_files LIST_DIRECTORIES FALSE RELATIVE "${staging}" "${staging}/*")
set(package_directories)
foreach(relative IN LISTS package_files)
    string(TOLOWER "${relative}" lower_relative)
    if((lower_relative MATCHES "\\.dll$" AND NOT relative STREQUAL "dxcompiler.dll") OR
            lower_relative MATCHES "\\.(pdb|lib|exp|obj|log|lock|key|pem)$" OR
            lower_relative MATCHES "(^|/)\\.env($|\\.)" OR
            (lower_relative MATCHES "\\.exe$" AND NOT relative STREQUAL runtime_filename))
        message(FATAL_ERROR "Unexpected generated/private material in release allowlist: ${relative}")
    endif()
    get_filename_component(parent "${relative}" DIRECTORY)
    while(NOT parent STREQUAL "")
        list(APPEND package_directories "${parent}")
        get_filename_component(parent "${parent}" DIRECTORY)
    endwhile()
endforeach()
list(REMOVE_DUPLICATES package_directories)
list(SORT package_files)
set(package_manifest "{}")
foreach(relative IN LISTS package_files)
    file(SHA256 "${staging}/${relative}" file_hash)
    file(SIZE "${staging}/${relative}" file_size)
    set(entry "{}")
    string(JSON entry SET "${entry}" bytes "${file_size}")
    engine_json_quote("${file_hash}" hash_json)
    string(JSON entry SET "${entry}" sha256 "${hash_json}")
    string(JSON package_manifest SET "${package_manifest}" "${relative}" "${entry}")
endforeach()
file(WRITE "${workspace}/package-files.json" "${package_manifest}\n")

# Back up only artifacts produced by the old deployment layout. Validate a
# legacy license tree's inventory before moving the directory; unknown content
# is retained and reported, never deleted or swept into the ZIP.
set(legacy_names "${LEGACY_PACK_FILENAME}" dxil.dll product-info.json
    build-configuration.txt README.md LICENSE THIRD_PARTY_NOTICES.txt
    .release-stage.lock .dxc-deploy.lock licenses ${legacy_api_files})
set(legacy_license_files ${ENGINE_NOTICE_FILES} .notices.lock
    dxc/NOTICE.txt dxc/LICENSE-LLVM.txt dxc/LICENSE-Windows-SDK.rtf
    dxc/ThirdPartyNotices.txt dxc/runtime-source.txt)
if(IS_DIRECTORY "${DESTINATION}/licenses")
    file(GLOB_RECURSE old_license_files LIST_DIRECTORIES FALSE RELATIVE "${DESTINATION}/licenses" "${DESTINATION}/licenses/*")
    foreach(relative IN LISTS old_license_files)
        if(NOT relative IN_LIST legacy_license_files)
            message(FATAL_ERROR "Unknown legacy license-tree file was preserved; move it before staging: licenses/${relative}")
        endif()
    endforeach()
endif()
if(IS_DIRECTORY "${DESTINATION}")
    file(GLOB_RECURSE existing_paths LIST_DIRECTORIES TRUE RELATIVE "${DESTINATION}" "${DESTINATION}/*")
    foreach(relative IN LISTS existing_paths)
        if(relative IN_LIST package_files OR relative IN_LIST package_directories OR
                relative IN_LIST legacy_names OR relative MATCHES "^licenses/")
            continue()
        endif()
        message(FATAL_ERROR "Unknown release path was preserved and excluded from packaging; move it before staging: ${relative}")
    endforeach()
endif()
foreach(relative IN LISTS legacy_names)
    set(legacy_path "${DESTINATION}/${relative}")
    if(EXISTS "${legacy_path}" OR IS_SYMLINK "${legacy_path}")
        require_exact_path("${legacy_path}" "${releases_root}/${RELEASE_NAME}/${relative}")
        set(backup_root "${workspace}/legacy")
        require_exact_path("${backup_root}" "${backup_root}")
        get_filename_component(backup_parent "${backup_root}/${relative}" DIRECTORY)
        file(MAKE_DIRECTORY "${backup_parent}")
        file(RENAME "${legacy_path}" "${backup_root}/${relative}" NO_REPLACE RESULT move_result)
        if(NOT move_result STREQUAL "0")
            message(FATAL_ERROR "Cannot preserve legacy generated file ${legacy_path}: ${move_result}")
        endif()
    endif()
endforeach()

# Archive only this fresh allowlist tree; never archive the existing release
# directory. No lock, build metadata, tool executable or runtime log is present.
set(archive "${workspace}/${RELEASE_NAME}.zip")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive}" --format=zip --
        "${runtime_filename}" dxcompiler.dll main.lua example doc
    WORKING_DIRECTORY "${staging}"
    RESULT_VARIABLE archive_result ERROR_VARIABLE archive_error)
if(NOT archive_result EQUAL 0)
    message(FATAL_ERROR "Cannot create release ZIP: ${archive_error}")
endif()
foreach(relative IN LISTS package_files)
    require_exact_path("${DESTINATION}/${relative}" "${releases_root}/${RELEASE_NAME}/${relative}")
    copy_release_file("${staging}/${relative}" "${DESTINATION}/${relative}")
endforeach()
copy_release_file("${archive}" "${releases_root}/${RELEASE_NAME}.zip")
file(SHA256 "${staging}/${runtime_filename}" executable_hash)
file(SHA256 "${archive}" archive_hash)
file(SHA256 "${workspace}/package-files.json" package_manifest_hash)
file(READ "${workspace}/verification/dxcompiler.json" dxc_metadata)
set(release_metadata "{}")
foreach(pair IN ITEMS "configuration;${CONFIGURATION}" "release;${RELEASE_NAME}"
        "stagedAt;${staged_at}" "executableSha256;${executable_hash}" "zipSha256;${archive_hash}"
        "packageManifestSha256;${package_manifest_hash}")
    list(GET pair 0 key)
    list(GET pair 1 value)
    engine_json_quote("${value}" value_json)
    string(JSON release_metadata SET "${release_metadata}" "${key}" "${value_json}")
endforeach()
string(JSON release_metadata SET "${release_metadata}" dxcompiler "${dxc_metadata}")
file(WRITE "${workspace}/release.json" "${release_metadata}\n")
file(WRITE "${work_root}/${RELEASE_NAME}-last.txt"
    "Configuration: ${CONFIGURATION}\nStaged at: ${staged_at}\n"
    "Executable SHA256: ${executable_hash}\nZIP SHA256: ${archive_hash}\n"
    "DXC DLL SHA256: ${ENGINE_DXC_RUNTIME_SHA256}\nDXC file version: ${ENGINE_DXC_RUNTIME_FILE_VERSION}\n"
    "DXC direct imports: ${ENGINE_DXC_RUNTIME_DEPENDENCIES}\n"
    "Package file manifest SHA256: ${package_manifest_hash}\n"
    "Workspace and any legacy backup: ${workspace}\n"
    "This build overwrites the managed Lua example files in bin and releases.\n"
    "Edit a ZIP extracted to a separate project directory for persistent user changes.\n"
    "PE import inspection:\n${dependency_output}\n")
message(STATUS "Staged ${CONFIGURATION} self-contained runtime directory: ${DESTINATION}")
message(STATUS "Created release ZIP: ${releases_root}/${RELEASE_NAME}.zip")
