// 细腻触觉/音频输出：WASAPI 渲染。两种目标：
//   ① DualSense（>=4 声道）：独占模式、设备真实格式(16-bit)、写 ch3/4 触觉音圈。
//   ② 虚拟声卡 CABLE（2 声道）：共享模式、float、写两声道 —— 喂给 DSX 的 Audio-to-Haptics。
// 目标由桌面 haptic_target.txt 决定：内容是端点 ID("{...}.{...}")→按 ID 取；否则当名字子串匹配。
// 渲染线程通过 haptic_pull_audio() 拉左右两路触觉波形。
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
static const float HAP_GAIN  = 1.6f;     // 输出端只做温和放大，主要手感由 DLL 内的触觉整形决定
static const float HAP_LP_HZ = 1200.0f;  // 保留更多瞬态纹理，减少低频拖尾感；0=不低通

static FILE* g_hlog = nullptr;
static void hlog(const char* fmt, ...) {
    if (!g_hlog) return; va_list a; va_start(a, fmt); vfprintf(g_hlog, fmt, a); va_end(a);
    fputc('\n', g_hlog); fflush(g_hlog);
}

// 读输出目标配置：默认 "DualSense"
static void get_target(char* out, int cap) {
    strncpy_s(out, cap, "DualSense", _TRUNCATE);
    char path[MAX_PATH]; char* up = nullptr; size_t n = 0;
    if (_dupenv_s(&up, &n, "USERPROFILE") == 0 && up) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\Desktop\\haptic_target.txt", up); free(up);
        FILE* f = nullptr; fopen_s(&f, path, "r");
        if (f) { char b[256] = ""; if (fgets(b, sizeof(b), f)) {
            int s = 0; while (b[s]==' '||b[s]=='\t') s++;
            int e = (int)strlen(b); while (e>s && (b[e-1]=='\n'||b[e-1]=='\r'||b[e-1]==' '||b[e-1]=='\t')) e--;
            b[e]=0; if (e>s) strncpy_s(out, cap, b+s, _TRUNCATE); } fclose(f); }
    }
}

// 定位目标设备：ID（以 '{' 开头）→GetDevice；否则名字子串→枚举匹配
static IMMDevice* find_target(IMMDeviceEnumerator* en, bool* outIsCable) {
    char target[256]; get_target(target, sizeof(target));
    hlog("target = \"%s\"", target);
    *outIsCable = (strstr(target, "CABLE") != nullptr) || (strstr(target, "Cable") != nullptr);
    if (target[0] == '{') {
        wchar_t id[300]; MultiByteToWideChar(CP_UTF8, 0, target, -1, id, 300);
        IMMDevice* d = nullptr;
        if (SUCCEEDED(en->GetDevice(id, &d)) && d) { hlog("GetDevice by ID ok"); return d; }
        hlog("GetDevice by ID 失败"); return nullptr;
    }
    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT n = 0; col->GetCount(&n); IMMDevice* found = nullptr;
    for (UINT i = 0; i < n && !found; ++i) {
        IMMDevice* d = nullptr; col->Item(i, &d);
        IPropertyStore* ps = nullptr; d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT nm; PropVariantInit(&nm); ps->GetValue(PKEY_Device_FriendlyName, &nm);
        char nb[512]=""; if(nm.vt==VT_LPWSTR) WideCharToMultiByte(CP_UTF8,0,nm.pwszVal,-1,nb,sizeof(nb),0,0);
        if (strstr(nb, target)) { found = d; d->AddRef(); hlog("found: %s", nb); }
        PropVariantClear(&nm); ps->Release(); d->Release();
    }
    col->Release(); return found;
}

static DWORD WINAPI render_thread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) return 1;

    IMMDevice* dev = nullptr; bool isCable = false;
    for (int t = 0; t < 600 && !dev; ++t) { dev = find_target(en, &isCable); if (!dev) Sleep(100); }
    if (!dev) { hlog("目标设备未找到"); en->Release(); return 1; }

    IAudioClient* ac = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) { hlog("Activate fail"); return 1; }

    // 读真实格式，判断声道数 → 决定模式
    IPropertyStore* ps = nullptr; dev->OpenPropertyStore(STGM_READ, &ps);
    PROPVARIANT df; PropVariantInit(&df); ps->GetValue(PK_DevFmt, &df);
    int devCh = 2;
    if (df.vt==VT_BLOB && df.blob.cbSize>=sizeof(WAVEFORMATEX)) devCh = ((WAVEFORMATEX*)df.blob.pBlobData)->nChannels;

    bool exclusive = (devCh >= 4) && !isCable;   // DualSense 走独占；CABLE 走共享
    WAVEFORMATEX* pfmt = nullptr;
    bool isFloat = false;
    HRESULT hr;
    REFERENCE_TIME defP=0, minP=0; ac->GetDevicePeriod(&defP,&minP);

    if (exclusive) {
        pfmt = (WAVEFORMATEX*)malloc(df.blob.cbSize); memcpy(pfmt, df.blob.pBlobData, df.blob.cbSize);
        REFERENCE_TIME dur = defP;
        hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, pfmt, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 fr=0; ac->GetBufferSize(&fr); dur=(REFERENCE_TIME)(10000.0*1000/pfmt->nSamplesPerSec*fr+0.5);
            ac->Release(); dev->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,(void**)&ac);
            hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, pfmt, nullptr);
        }
    } else {
        ac->GetMixFormat(&pfmt);                 // 共享：用混音格式（通常 float 2ch）
        isFloat = (pfmt->wBitsPerSample == 32);
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, pfmt, nullptr);
    }
    PropVariantClear(&df); ps->Release();
    if (FAILED(hr)) { hlog("Initialize fail 0x%08lx (exclusive=%d)", (unsigned long)hr, (int)exclusive); return 1; }

    const int CH = pfmt->nChannels, BITS = pfmt->wBitsPerSample;
    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ac->SetEventHandle(evt);
    UINT32 bufFrames=0; ac->GetBufferSize(&bufFrames);
    IAudioRenderClient* rc=nullptr;
    if (FAILED(ac->GetService(__uuidof(IAudioRenderClient),(void**)&rc))) { hlog("GetService fail"); return 1; }
    DWORD taskIdx=0; HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio",&taskIdx);
    BYTE* p=nullptr; if (SUCCEEDED(rc->GetBuffer(bufFrames,&p))) rc->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    ac->Start();
    hlog("started: exclusive=%d CH=%d bits=%d float=%d bufFrames=%u", (int)exclusive, CH, BITS, (int)isFloat, bufFrames);

    float lpK = (HAP_LP_HZ>0) ? (1.0f - expf(-2.0f*(float)PI*HAP_LP_HZ/pfmt->nSamplesPerSec)) : 1.0f;
    float lpL = 0, lpR = 0; static float hapL[8192], hapR[8192];

    while (g_started) {
        if (WaitForSingleObject(evt, 1000) != WAIT_OBJECT_0) continue;
        UINT32 pad=0; ac->GetCurrentPadding(&pad);
        UINT32 avail = bufFrames - pad; if (!avail) continue;
        if (FAILED(rc->GetBuffer(avail,&p))) break;
        int got = haptic_pull_audio(hapL, hapR, (int)avail);
        for (UINT32 i=0;i<avail;i++) {
            float xL = got ? hapL[i] : 0.0f;
            float xR = got ? hapR[i] : 0.0f;
            lpL += (xL - lpL)*lpK;                       // 低通
            lpR += (xR - lpR)*lpK;
            float yL = lpL * HAP_GAIN; if (yL>1) yL=1; if (yL<-1) yL=-1;
            float yR = lpR * HAP_GAIN; if (yR>1) yR=1; if (yR<-1) yR=-1;
            for (int c=0;c<CH;c++) {
                bool hapCh = (CH>=4) ? (c==2||c==3) : true;   // 4声道→ch3/4；否则两声道都写
                float s = 0.0f;
                if (hapCh) s = (CH>=4) ? (c==2 ? yL : yR) : (c==0 ? yL : yR);
                if (isFloat) ((float*)p)[i*CH+c] = s;
                else         ((short*)p)[i*CH+c] = (short)(s*32767.0f);
            }
        }
        rc->ReleaseBuffer(avail, 0);
    }
    ac->Stop(); if (mm) AvRevertMmThreadCharacteristics(mm);
    rc->Release(); CloseHandle(evt); ac->Release(); dev->Release(); en->Release(); CoUninitialize();
    return 0;
}

extern "C" void haptic_out_start() {
    if (g_started) return; g_started = true;
    char path[MAX_PATH]="haptic_out_log.txt"; char* up=nullptr; size_t n=0;   // 调试日志放 %TEMP%，不脏桌面
    if (_dupenv_s(&up,&n,"TEMP")==0 && up) { _snprintf_s(path,sizeof(path),_TRUNCATE,"%s\\haptic_out_log.txt",up); free(up); }
    fopen_s(&g_hlog, path, "w"); hlog("=== haptic_out start ===");
    CreateThread(nullptr, 0, render_thread, nullptr, 0, nullptr);
}
