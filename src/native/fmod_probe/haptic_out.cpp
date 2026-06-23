// 细腻触觉输出：WASAPI 独占 → DualSense 音频设备 ch3/4 渲染被电平调制的 ~100Hz 载波。
// 电平 = 音效响度（dllmain 采样 audibility 后调 haptic_out_set_level）。
// 输出端已用独立工具 play34 验证可驱动 DualSense 触觉音圈。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "haptic_out.h"

static const PROPERTYKEY PK_DevFmt = { {0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}}, 0 };
static const double PI = 3.14159265358979;

static volatile bool g_started = false;

// 可调参数
static const float HAP_GAIN   = 3.0f;     // 音效波形 → 触觉 增益
static const float HAP_LP_HZ  = 700.0f;   // 低通截止（聚焦可感受频段、去高频噪声）；0=不低通

static FILE* g_hlog = nullptr;
static void hlog(const char* fmt, ...) {
    if (!g_hlog) return;
    va_list a; va_start(a, fmt); vfprintf(g_hlog, fmt, a); va_end(a);
    fputc('\n', g_hlog); fflush(g_hlog);
}

// 找激活的、名字含 "DualSense" 的渲染端点
static IMMDevice* find_dualsense(IMMDeviceEnumerator* en) {
    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT n = 0; col->GetCount(&n);
    IMMDevice* found = nullptr;
    for (UINT i = 0; i < n && !found; ++i) {
        IMMDevice* d = nullptr; col->Item(i, &d);
        IPropertyStore* ps = nullptr; d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT nm; PropVariantInit(&nm); ps->GetValue(PKEY_Device_FriendlyName, &nm);
        char nb[512] = ""; if (nm.vt == VT_LPWSTR) WideCharToMultiByte(CP_UTF8, 0, nm.pwszVal, -1, nb, sizeof(nb), 0, 0);
        if (strstr(nb, "DualSense")) { found = d; d->AddRef(); hlog("found endpoint: %s", nb); }
        PropVariantClear(&nm); ps->Release(); d->Release();
    }
    col->Release();
    return found;
}

static DWORD WINAPI render_thread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en))) { hlog("no enumerator"); return 1; }

    // 设备可能稍后才插上：重试找 DualSense
    IMMDevice* dev = nullptr;
    for (int tries = 0; tries < 600 && !dev; ++tries) {   // 最多等 ~60s
        dev = find_dualsense(en);
        if (!dev) Sleep(100);
    }
    if (!dev) { hlog("DualSense 端点未找到（USB 没插？）"); en->Release(); return 1; }

    IAudioClient* ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) { hlog("Activate fail"); return 1; }
    IPropertyStore* ps = nullptr; dev->OpenPropertyStore(STGM_READ, &ps);
    PROPVARIANT df; PropVariantInit(&df); ps->GetValue(PK_DevFmt, &df);
    if (!(df.vt == VT_BLOB && df.blob.cbSize >= sizeof(WAVEFORMATEX))) { hlog("no DeviceFormat"); return 1; }
    WAVEFORMATEX* pfmt = (WAVEFORMATEX*)malloc(df.blob.cbSize);
    memcpy(pfmt, df.blob.pBlobData, df.blob.cbSize);
    PropVariantClear(&df); ps->Release();
    const int CH = pfmt->nChannels, RATE = pfmt->nSamplesPerSec;
    const int hapL = (CH >= 4) ? 2 : 0, hapR = (CH >= 4) ? 3 : (CH - 1);
    hlog("fmt ch=%d rate=%d bits=%d", CH, RATE, pfmt->wBitsPerSample);

    REFERENCE_TIME defP = 0, minP = 0; ac->GetDevicePeriod(&defP, &minP);
    REFERENCE_TIME dur = defP;
    HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, pfmt, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 fr = 0; ac->GetBufferSize(&fr);
        dur = (REFERENCE_TIME)(10000.0 * 1000 / RATE * fr + 0.5);
        ac->Release(); dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, pfmt, nullptr);
    }
    if (FAILED(hr)) { hlog("Initialize fail 0x%08lx", (unsigned long)hr); return 1; }

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ac->SetEventHandle(evt);
    UINT32 bufFrames = 0; ac->GetBufferSize(&bufFrames);
    IAudioRenderClient* rc = nullptr;
    if (FAILED(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc))) { hlog("GetService fail"); return 1; }

    DWORD taskIdx = 0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    BYTE* p = nullptr;
    if (SUCCEEDED(rc->GetBuffer(bufFrames, &p))) rc->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    ac->Start();
    hlog("started, bufFrames=%u, hapticCh=%d/%d", bufFrames, hapL + 1, hapR + 1);

    // 1 极点低通系数
    float lpK = 1.0f;
    if (HAP_LP_HZ > 0) lpK = 1.0f - expf(-2.0f * (float)PI * HAP_LP_HZ / RATE);
    float lp = 0;
    static float mono[8192];

    while (g_started) {
        if (WaitForSingleObject(evt, 1000) != WAIT_OBJECT_0) continue;
        UINT32 pad = 0; ac->GetCurrentPadding(&pad);
        UINT32 avail = bufFrames - pad;
        if (!avail) continue;
        if (FAILED(rc->GetBuffer(avail, &p))) break;
        short* out = (short*)p;
        int got = haptic_pull_audio(mono, (int)avail);   // 拉音效真实波形
        for (UINT32 i = 0; i < avail; ++i) {
            float x = got ? mono[i] : 0.0f;
            lp += (x - lp) * lpK;                          // 低通
            float y = lp * HAP_GAIN;
            if (y > 1) y = 1; if (y < -1) y = -1;
            short v = (short)(y * 32767.0f);
            for (int c = 0; c < CH; ++c) out[i * CH + c] = (c == hapL || c == hapR) ? v : 0;
        }
        rc->ReleaseBuffer(avail, 0);
    }
    ac->Stop();
    if (mm) AvRevertMmThreadCharacteristics(mm);
    rc->Release(); CloseHandle(evt); ac->Release(); dev->Release(); en->Release();
    CoUninitialize();
    return 0;
}

extern "C" void haptic_out_start() {
    if (g_started) return;
    g_started = true;
    char path[MAX_PATH] = "haptic_out_log.txt";
    char* up = nullptr; size_t n = 0;
    if (_dupenv_s(&up, &n, "USERPROFILE") == 0 && up) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\Desktop\\haptic_out_log.txt", up); free(up);
    }
    fopen_s(&g_hlog, path, "w");
    hlog("=== haptic_out start ===");
    CreateThread(nullptr, 0, render_thread, nullptr, 0, nullptr);
}
