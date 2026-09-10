# CMakeList.txt: CMakeProject1 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.12)
# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 如果支持，请为 MSVC 编译器启用热重载。
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("GitServer")

# 设置编译选项
if (MSVC)
# /W4 = 警告等级 4（MSVC 最严格的常规警告级别）
# /utf-8 → 强制使用 UTF-8 编码
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 设置编译输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR})

set(TARGET ${PROJECT_NAME}-${CMAKE_SYSTEM_NAME})

find_package(nlohmann_json REQUIRED)

# Boost：通过 b2 生成的官方 CMake 配置文件（config 模式）查找。
# Boost_ROOT（环境变量或 CMake 缓存变量）需指向安装前缀:
#   D:/Library/boost_1_91_0/stage
# 需要 filesystem 组件（ConfigMgr 用它获取当前工作目录）：
# 用 COMPONENTS 声明所需组件，并链接对应的 Boost::<组件名> target
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem)
find_package(spdlog REQUIRED)

# gRPC 的 CMake 配置依赖 protobuf 提供的目标（protobuf::libprotobuf / protobuf::libprotoc）
# 必须先加载 protobuf，再加载 gRPC，顺序不能反
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
# 将源代码添加到此项目的可执行文件。
add_executable (${TARGET}   "src/const.h" "src/CServer.h" "src/HttpConnection.h" "src/LogicSystem.h" "src/Singleton.h" "src/CServer.cpp" "src/GitServer.cpp" "src/HttpConnection.cpp" 
"src/LogicSystem.cpp"
"message.pb.cc" "message.grpc.pb.cc"
 "src/VarifyGrpcClient.h" "src/ConfigMgr.h" "src/ConfigMgr.cpp")

#include <boost/beast/http.hpp>
#include <boost/beast.hpp>
#include <boost/asio.hpp>
target_link_libraries(${TARGET} 
                        nlohmann_json::nlohmann_json
                        Boost::boost
                        Boost::filesystem
                        spdlog::spdlog
                        gRPC::grpc++
                        gRPC::grpc
                        protobuf::libprotobuf
                        )


# TODO: 如有需要，请添加测试并安装目标。
