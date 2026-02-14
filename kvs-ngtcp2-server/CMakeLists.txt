cmake_minimum_required(VERSION 3.10)
project(kvs-ngtcp2-server C)

set(CMAKE_C_STANDARD 99)

# Relay server: TCP+TLS multiplexed streams (same protocol as QUIC; QUIC can be swapped in later)
find_package(OpenSSL REQUIRED)

add_executable(kvs-ngtcp2-server
  src/main.c
  src/server.c
  src/session.c
  src/protocol.c
)

target_include_directories(kvs-ngtcp2-server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(kvs-ngtcp2-server PRIVATE
  OpenSSL::SSL
  OpenSSL::Crypto
)

if(UNIX AND NOT APPLE)
  target_link_libraries(kvs-ngtcp2-server PRIVATE pthread)
endif()

install(TARGETS kvs-ngtcp2-server RUNTIME DESTINATION bin)
