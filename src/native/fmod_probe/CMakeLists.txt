cmake_minimum_required(VERSION 3.15)
project(fmod_probe CXX ASM_MASM)

# 必须 64 位（只狼是 x64，fmodex64.dll 也是 x64）
# thunks.asm: 360 个 jmp 跳转桩（自动生成），导出修饰名直接 jmp 到原函数
# haptic_out.cpp: WASAPI 写 DualSense ch3/4 触觉音圈
add_library(fmodex64 SHARED dllmain.cpp thunks.asm haptic_out.cpp)
add_executable(ds_speaker_beep ../haptic_test/ds_speaker_beep.cpp)

# 输出文件名固定为 fmodex64.dll
set_target_properties(fmodex64 PROPERTIES
    OUTPUT_NAME "fmodex64"
    PREFIX "")

# WASAPI 触觉输出需要的库
target_link_libraries(fmodex64 PRIVATE ole32 avrt)
target_link_libraries(ds_speaker_beep PRIVATE ole32 avrt)

# 用 .def 控制导出：706 个转发 + 3 个拦截
target_link_options(fmodex64 PRIVATE
    "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/fmodex64.def")

set_property(TARGET fmodex64 PROPERTY CXX_STANDARD 17)
set_property(TARGET ds_speaker_beep PROPERTY CXX_STANDARD 17)

if (MSVC)
    target_compile_options(fmodex64 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
    target_compile_options(ds_speaker_beep PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

# 构建示例：
#   cmake -B build -A x64
#   cmake --build build --config Release
# 产物：build/Release/fmodex64.dll
