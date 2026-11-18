# Network play (src/net), included by runtime.cmake: libdatachannel for the room's WebSocket and each peer's WebRTC data
# channels, over Mbed TLS on every platform, both fetched when the build is configured. Defines saturn_net.
include(FetchContent)
set(_net ${_src}/net)

FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
    URL_HASH SHA256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG 6b1e2e620f1e37f0eafeee702eaea0043cb305fd   # v0.24.6
    GIT_SUBMODULES deps/libjuice deps/usrsctp deps/plog deps/json)

# A function, so the libraries' options stay out of the game's own scope.
function(saturn_fetch_net)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)       # option() takes these variables as set
  # Clang with MSVC's libraries takes GCC's options, but all three libraries give it MSVC's when they see MSVC
  set(_gnu_on_msvc OFF)
  if(CMAKE_C_SIMULATE_ID STREQUAL "MSVC" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    set(_gnu_on_msvc ON)
    set(CMAKE_C_SIMULATE_ID "")
    set(CMAKE_CXX_SIMULATE_ID "")
    set(MSVC OFF)
  endif()
  set(ENABLE_PROGRAMS OFF)
  set(ENABLE_TESTING OFF)
  set(MBEDTLS_FATAL_WARNINGS OFF)
  set(MBEDTLS_USER_CONFIG_FILE ${_net}/mbedtls/saturn_mbedtls_config.h CACHE FILEPATH "" FORCE)
  FetchContent_MakeAvailable(mbedtls)
  target_include_directories(mbedcrypto PUBLIC ${_net}/mbedtls)    # threading_alt.h
  foreach(t mbedtls mbedx509 mbedcrypto)
    target_compile_options(${t} PRIVATE -w)
  endforeach()
  add_library(saturn_mbedtls INTERFACE)
  target_link_libraries(saturn_mbedtls INTERFACE mbedtls mbedx509 mbedcrypto)
  add_library(MbedTLS::MbedTLS ALIAS saturn_mbedtls)                # what libdatachannel links, if it exists

  set(USE_MBEDTLS ON)
  set(NO_MEDIA ON)
  set(NO_EXAMPLES ON)
  set(NO_TESTS ON)
  set(BUILD_SHARED_LIBS ON)                   # its default, which its install rules need; saturn_net links the static one
  FetchContent_MakeAvailable(libdatachannel)
  set_target_properties(datachannel PROPERTIES EXCLUDE_FROM_ALL ON)
  if(_gnu_on_msvc)
    foreach(t datachannel-static juice-static usrsctp)
      target_compile_definitions(${t} PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)   # as their MSVC branches set
    endforeach()
  endif()
  set(SATURN_JSON_INCLUDE ${libdatachannel_SOURCE_DIR}/deps/json/single_include PARENT_SCOPE)
endfunction()
saturn_fetch_net()

# The CA certificates the room's TLS is checked against, compiled in
set(_cacert ${CMAKE_CURRENT_BINARY_DIR}/saturn-cacert/cacert.h)
add_custom_command(OUTPUT ${_cacert}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_rt}/third_party/cacert/cacert.pem -DOUTPUT=${_cacert} -DNAME=cacert_pem -P ${_rt}/embed.cmake
    DEPENDS ${_rt}/third_party/cacert/cacert.pem ${_rt}/embed.cmake VERBATIM)
add_library(saturn_net STATIC ${_net}/session.cpp ${_net}/mbedtls_threads.cpp ${_net}/ca_bundle.cpp ${_net}/http_get.cpp
    ${_net}/netband.cpp ${_net}/netband_sockets.cpp ${_net}/netband_wire.cpp ${_cacert})
target_include_directories(saturn_net PUBLIC ${_net} ${SATURN_JSON_INCLUDE} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/saturn-cacert)
target_link_libraries(saturn_net PUBLIC LibDataChannel::LibDataChannelStatic PRIVATE saturn_mbedtls)
