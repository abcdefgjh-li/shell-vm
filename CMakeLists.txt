cmake_minimum_required(VERSION 3.10)
project(ShellVM VERSION 1.0.0 LANGUAGES CXX)

# C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项
if(MSVC)
    add_compile_options(/W4 /WX- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# 头文件目录
include_directories(include)

# 源文件
set(SOURCES
    src/vm_core.cpp
    src/compiler.cpp
    src/runtime.cpp
)

# 主库
add_library(shellvm STATIC ${SOURCES})

# 示例程序
add_executable(basic_demo examples/basic_demo.cpp)
target_link_libraries(basic_demo shellvm)

# 宿主端字节码嵌入工具
add_executable(embed_bytecode tools/embed_bytecode.cpp)
target_link_libraries(embed_bytecode shellvm)

# 安装配置
install(TARGETS shellvm DESTINATION lib)
install(FILES include/shell_vm.h include/vm_core.h include/compiler.h include/runtime.h DESTINATION include)

# 打包信息
set(CPACK_PACKAGE_NAME "ShellVM")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Android Shell Virtual Machine")
set(CPACK_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_PACKAGE_CONTACT "ShellVM Project")
