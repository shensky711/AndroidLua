# CMake 最低版本号要求
cmake_minimum_required(VERSION 3.4.1)

# 项目信息
project(luaJava)

# 查找所有源文件并将名称保存到 DIR_SRCS 变量
aux_source_directory(src/main/cpp/luajava/ DIR_SRCS)

# 生成动态链接库luajava
add_library(luajava SHARED ${DIR_SRCS})

# 把src/main/cpp/lua目录下的CMake文件include进来
add_subdirectory(src/main/cpp/lua)

# 添加链接库lua
target_link_libraries(luajava lua)
