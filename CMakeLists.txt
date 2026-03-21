cmake_minimum_required(VERSION 3.15)
project(VoicebotGateway VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 1. 의존성 패키지 찾기
find_package(PkgConfig REQUIRED)

# PJSIP 찾기
pkg_check_modules(PJPROJECT REQUIRED libpjproject)

# Boost, gRPC, Protobuf, OpenSSL 찾기
find_package(Boost REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_library(ONNXRUNTIME_LIB onnxruntime PATHS /opt/homebrew/lib REQUIRED)
find_package(spdlog REQUIRED)

find_program(PROTOC_EXECUTABLE protoc)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protos)
set(PROTO_SRC ${PROTO_DIR}/voicebot.proto)

set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_HDRS ${PROTO_GEN_DIR}/voicebot.pb.h ${PROTO_GEN_DIR}/voicebot.grpc.pb.h)
set(PROTO_SRCS ${PROTO_GEN_DIR}/voicebot.pb.cc ${PROTO_GEN_DIR}/voicebot.grpc.pb.cc)

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
    COMMAND ${PROTOC_EXECUTABLE}
    ARGS --grpc_out=${PROTO_GEN_DIR}
         --cpp_out=${PROTO_GEN_DIR}
         -I ${PROTO_DIR}
         --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
         ${PROTO_SRC}
    DEPENDS ${PROTO_SRC}
)

# 2. 실행 파일 선언
add_executable(vbgw 
    src/main.cpp
    src/engine/VoicebotEndpoint.cpp
    src/engine/VoicebotAccount.cpp
    src/engine/VoicebotCall.cpp
    src/engine/VoicebotMediaPort.cpp
    src/ai/VoicebotAiClient.cpp
    src/ai/SileroVad.cpp
    ${PROTO_SRCS}
)

# 3. 헤더 디렉토리 포함
target_include_directories(vbgw PRIVATE 
    ${PJPROJECT_INCLUDE_DIRS}
    src/
    ${PROTO_GEN_DIR}
    /opt/homebrew/include/onnxruntime
)

# 4. 컴파일 옵션 및 라이브러리 링크
target_compile_options(vbgw PRIVATE ${PJPROJECT_CFLAGS_OTHER})
target_link_directories(vbgw PRIVATE ${PJPROJECT_LIBRARY_DIRS} /opt/homebrew/opt/openssl@3/lib /opt/homebrew/lib)

execute_process(COMMAND pkg-config --static --libs libpjproject
                OUTPUT_VARIABLE PJ_RAW_LDFLAGS
                OUTPUT_STRIP_TRAILING_WHITESPACE)
set_target_properties(vbgw PROPERTIES LINK_FLAGS "${PJ_RAW_LDFLAGS}")

target_link_libraries(vbgw PRIVATE 
    Boost::headers
    gRPC::grpc++
    protobuf::libprotobuf
    OpenSSL::SSL
    OpenSSL::Crypto
    ${ONNXRUNTIME_LIB}
    spdlog::spdlog
)
