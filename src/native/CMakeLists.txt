cmake_minimum_required(VERSION 3.15)
project(fmodex64_proxy CXX ASM_MASM)

# ============================================================================
# 平台化目录结构:
#   core/             公用框架(引擎/游戏无关):WASAPI 触觉输出 haptic_out
#   engines/fmod_ex/  FMOD Ex 引擎适配器(所有 FMOD Ex 游戏共用):hook 逻辑 dllmain
#   games/sekiro/     只狼专属:从只狼那份 fmodex64.dll 生成的 .def/thunks + profile.json
#   tools/            开发工具(不进产品):haptic_test(enum/play34)
#
# 三者编成同一个代理 fmodex64.dll。
# 换游戏只需换 games/<game>/ 下的 .def / thunks(用导出解析脚本重新生成)+ profile。
# ============================================================================

add_library(fmodex64 SHARED
    engines/fmod_ex/dllmain.cpp        # FMOD Ex hook(引擎层)
    core/haptic_out.cpp                # 触觉输出(公用框架)
    games/sekiro/thunks.asm)           # 只狼 dll 的 jmp 转发桩(游戏专属)

set_target_properties(fmodex64 PROPERTIES
    OUTPUT_NAME "fmodex64"
    PREFIX "")

# 让 dllmain.cpp 里的 #include "haptic_out.h" / "thunks_gen.h" 不改路径就能找到。
# (这是本次重构不动 C++ 源码的关键:头文件搜索路径在这里给。)
target_include_directories(fmodex64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/games/sekiro)

# WASAPI 触觉输出需要的库
target_link_libraries(fmodex64 PRIVATE ole32 avrt)

# 导出表:只狼那份 dll 专属的 .def(706 个转发桩别名 + 3 个拦截)
target_link_options(fmodex64 PRIVATE
    "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/games/sekiro/fmodex64.def")

set_property(TARGET fmodex64 PROPERTY CXX_STANDARD 17)

# 构建(在 src/native/ 目录下执行):
#   cmake -B build -A x64
#   cmake --build build --config Release
# 产物:build/Release/fmodex64.dll
