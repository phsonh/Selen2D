# Build-time bytecode embedding. No runtime filesystem path or shader compiler dependency.
file(READ "${VERTEX}" vertex HEX)
file(READ "${FRAGMENT}" fragment HEX)
string(REGEX REPLACE "(..)" "0x\\1," vertex "${vertex}")
string(REGEX REPLACE "(..)" "0x\\1," fragment "${fragment}")
if(NOT DEFINED NAMESPACE)
    set(NAMESPACE image_shader_bytecode)
endif()
file(WRITE "${OUTPUT}" "#pragma once\nnamespace ${NAMESPACE} {\ninline constexpr unsigned char vertex[]={${vertex}};\ninline constexpr unsigned char fragment[]={${fragment}};\ninline constexpr auto format=${FORMAT};\n}\n")
