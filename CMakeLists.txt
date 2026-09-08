cmake_minimum_required(VERSION 3.10)
project(test_m04 C CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加SOEM库
add_subdirectory(soem)

# 创建测试可执行文件
add_executable(test_m04 test_m04.cpp)

# 链接SOEM库
target_link_libraries(test_m04 soem)

# 添加包含目录
target_include_directories(test_m04 PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/soem/include
    ${CMAKE_CURRENT_SOURCE_DIR}/soem/osal/win32
    ${CMAKE_CURRENT_SOURCE_DIR}/soem/oshw/win32
)

# Windows特定配置
if(WIN32)
    # SOEM库已自动链接wpcap和Packet，无需重复链接
    # 与ethercat_win工程保持一致：不要求管理员权限
endif()

# 编译选项
target_compile_options(test_m04 PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

# 输出目录
set_target_properties(test_m04 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    VS_DEBUGGER_COMMAND_ARGUMENTS ""
)

message(STATUS "M04测试程序配置完成")
message(STATUS "请确保已安装Npcap: https://npcap.com/")
message(STATUS "运行时需要管理员权限")