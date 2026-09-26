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
# 编译出来的可执行文件（exe），直接放到项目根目录。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR})

# set(变量名 值)
# ${变量名}是cmake取变量值的语法 
# 定义一个叫 TARGET 的变量，后面可以 ${TARGET} 引用它
# 取项目名
# ${CMAKE_SYSTEM_NAME}CMake 内置变量，代表当前编译目标的操作系统名称
# 在 Windows 编译，项目名 Demo → TARGET = Demo-Windows
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

# ============ Redis：hiredis(动态) + redis-plus-plus(静态) ============
# 两个库都装了官方 CMake 配置文件，用 config 模式直接找
# 不用配置环境变量了
# 往 CMake 的查找路径列表 CMAKE_PREFIX_PATH 追加两个目录
list(APPEND CMAKE_PREFIX_PATH
    "D:/Library/redis-hiredis/hiredis-debug"
    "D:/Library/redis-plus-plus/redis-plus-plus-debug"
)
# 顺序不能反：redis++ 的配置文件内部引用了 hiredis::hiredis 这个 target
find_package(hiredis CONFIG REQUIRED)
find_package(redis++ CONFIG REQUIRED)

# 将源代码添加到此项目的可执行文件。
add_executable (${TARGET}   "src/const.h" "src/CServer.h" "src/HttpConnection.h" "src/LogicSystem.h" "src/Singleton.h" "src/CServer.cpp" "src/GitServer.cpp" "src/HttpConnection.cpp" 
"src/LogicSystem.cpp"
"message.pb.cc" "message.grpc.pb.cc"
 "src/VarifyGrpcClient.h" "src/ConfigMgr.h" "src/ConfigMgr.cpp" "src/AsioIOServicePool.h" "src/AsioIOServicePool.cpp" "src/VarifyGrpcClient.cpp" "src/RedisTest.h" 
 "src/RedisMgr.h" 
 "src/RedisMgr.cpp" 
 "src/RedisConPool.h" 
 "src/RedisConPool.cpp" 
 "src/TestRedisMgr.h"
 "src/RedisHolder.h"
 "src/RedisHolder.cpp"
 "src/TestRedisConPool.h"
)

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
                        hiredis::hiredis          # ← 新增：hiredis 动态库
                        redis++::redis++_static   # ← 新增：redis-plus-plus 静态库
                        )


# hiredis 是动态库（redis++ 是静态库，不需要拷）：
# 链接只需要 hiredisd.lib（导入库），但【运行时】Windows 加载器必须在
# exe 所在目录（或 PATH 里）找到 hiredisd.dll，否则报"找不到 hiredisd.dll"。
# POST_BUILD = 每次编译成功后自动执行；
# $<TARGET_FILE_DIR:${TARGET}> 会展开成 exe 实际输出目录
#（第 27 行已把 Debug 输出目录设为项目根目录，所以 DLL 会被拷到这里和 exe 作伴）
# copy_if_different = 内容没变就不重复拷贝，开销几乎为零
# 在你的目标 ${TARGET} 编译成功之后，自动把 hiredisd.dll 复制到程序 exe 所在目录
add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND}  -E 
    copy_if_different#只有源文件和目标文件不一样时才复制；
        "D:/Library/redis-hiredis/hiredis-debug/bin/hiredisd.dll"
        $<TARGET_FILE_DIR:${TARGET}>)# 目标完整路径 + 文件名（exe 全路径）


# TODO: 如有需要，请添加测试并安装目标。
