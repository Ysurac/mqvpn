cmake_minimum_required(VERSION 3.22)
project(mqvpn_jni C)

# Prebuilt libmqvpn.a, xquic, BoringSSL per ABI
set(PREBUILT_DIR ${CMAKE_SOURCE_DIR}/../../../prebuilt/${ANDROID_ABI})

# libmqvpn (static)
add_library(mqvpn STATIC IMPORTED)
set_target_properties(mqvpn PROPERTIES
    IMPORTED_LOCATION ${PREBUILT_DIR}/libmqvpn.a)

# xquic (static)
add_library(xquic-static STATIC IMPORTED)
set_target_properties(xquic-static PROPERTIES
    IMPORTED_LOCATION ${PREBUILT_DIR}/libxquic.a)

# BoringSSL
add_library(ssl STATIC IMPORTED)
set_target_properties(ssl PROPERTIES
    IMPORTED_LOCATION ${PREBUILT_DIR}/libssl.a)
add_library(crypto STATIC IMPORTED)
set_target_properties(crypto PROPERTIES
    IMPORTED_LOCATION ${PREBUILT_DIR}/libcrypto.a)

# Include paths
set(MQVPN_ROOT ${CMAKE_SOURCE_DIR}/../../../../../../)
include_directories(
    ${MQVPN_ROOT}/include
    ${MQVPN_ROOT}/src
    ${MQVPN_ROOT}/third_party/xquic/include
)

# JNI shared library
add_library(mqvpn_jni SHARED mqvpn_jni.c)

target_link_libraries(mqvpn_jni
    mqvpn
    xquic-static
    ssl
    crypto
    log)  # Android __android_log_print
