#pragma once
// 细腻触觉输出：WASAPI 独占往 DualSense 音频设备 ch3/4(触觉音圈) 渲染。
// 渲染线程通过 haptic_pull_audio() 拉取"音效真实波形"，直接送进触觉马达。
extern "C" void haptic_out_start();                 // 启动渲染线程（幂等）
extern "C" int  haptic_pull_audio(float* out, int n); // 由 dllmain 实现：拉 n 个单声道样本，返回 1/0
