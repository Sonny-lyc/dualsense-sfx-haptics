// 弹刀合成脉冲独立试听工具：不需要开只狼，直接连 DualSense 的 ch3/4 触觉音圈，
// 依次播放几组不同参数的合成脉冲（跟 dllmain.cpp 的 shape_haptic_sample 里那段算法完全一致：
// 白噪声 -> 115Hz 高通 -> 300Hz 低通(带通) -> 指数包络 -> 放大系数），每组之间停顿方便区分。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

static const PROPERTYKEY PK_DevFmt = { {0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}}, 0 };

static void wstr_to_utf8(const wchar_t* w, char* out, int cap) {
    out[0] = 0;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr);
}

static IMMDevice* find_dualsense(IMMDeviceEnumerator* en) {
    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT count = 0;
    col->GetCount(&count);
    IMMDevice* found = nullptr;
    for (UINT i = 0; i < count && !found; ++i) {
        IMMDevice* dev = nullptr;
        col->Item(i, &dev);
        IPropertyStore* ps = nullptr;
        dev->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT name; PropVariantInit(&name);
        ps->GetValue(PKEY_Device_FriendlyName, &name);
        char friendly[512] = "";
        if (name.vt == VT_LPWSTR) wstr_to_utf8(name.pwszVal, friendly, sizeof(friendly));
        if (strstr(friendly, "DualSense") || strstr(friendly, "Dualsense")) { found = dev; found->AddRef(); printf(">> found: %s\n", friendly); }
        PropVariantClear(&name); ps->Release(); dev->Release();
    }
    col->Release();
    return found;
}

// ---- 跟 dllmain.cpp 完全一致的脉冲算法，参数可调 ----
struct PulseParams { const char* label; float makeup; float decayTauFrames; };

static void render_pulse(float* buf, unsigned frames, const PulseParams& p, uint32_t& seed) {
    const float HP_A = 0.985f;      // 115Hz 高通系数，跟 dllmain.cpp 一致
    const float BP_LP_A = 0.0385f;  // 300Hz 低通系数，跟 dllmain.cpp 一致
    float hpPrevIn = 0.0f, hpPrevOut = 0.0f, bp = 0.0f;
    for (unsigned i = 0; i < frames; ++i) {
        seed = seed * 1664525u + 1013904223u;
        float noise = (float)(int32_t)seed * (1.0f / 2147483648.0f);
        float hpRaw = HP_A * (hpPrevOut + noise - hpPrevIn);
        hpPrevIn = noise; hpPrevOut = hpRaw;
        bp += (hpRaw - bp) * BP_LP_A;
        float envelope = expf(-(float)i / p.decayTauFrames);
        float s = bp * envelope * p.makeup;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        buf[i] = s;
    }
}

static void write_sample(BYTE* dst, UINT32 frame, int channel, int channels, bool isFloat, int bits, float sample) {
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    int index = (int)frame * channels + channel;
    if (isFloat && bits == 32) { ((float*)dst)[index] = sample; return; }
    if (bits == 16) { ((int16_t*)dst)[index] = (int16_t)(sample * 32767.0f); return; }
}

int main(int argc, char** argv) {
    // 依次试听几组：跟当前实机装着的那版一致(makeup=6.0/decay~40ms)，外加几个对照组，方便比大小。
    // 固定种子后重新验证：makeup=250 连续放几次，波形现在应该是完全一样的，手感应该稳定了。
    PulseParams variants[] = {
        { "makeup=250 固定波形 (第1次)", 250.0f, 1900.0f },
        { "makeup=250 固定波形 (第2次)", 250.0f, 1900.0f },
        { "makeup=250 固定波形 (第3次)", 250.0f, 1900.0f },
        { "makeup=250 固定波形 (第4次)", 250.0f, 1900.0f },
        { "makeup=250 固定波形 (第5次)", 250.0f, 1900.0f },
    };
    int variantCount = (int)(sizeof(variants) / sizeof(variants[0]));

    printf(">> 弹刀合成脉冲试听工具，依次播放 %d 组，每组间隔 1.2 秒\n", variantCount);
    printf(">> 手柄用 USB 连好，确认没有其它程序占用 DualSense 音频设备（先关掉只狼/DSX）\n\n");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("FAIL CoInitializeEx\n"); return 1; }

    IMMDeviceEnumerator* en = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr) || !en) { printf("FAIL MMDeviceEnumerator\n"); return 1; }

    IMMDevice* dev = find_dualsense(en);
    if (!dev) { printf("FAIL 没找到 DualSense 设备，确认 USB 连接、且没被别的程序占用\n"); return 1; }

    IPropertyStore* ps = nullptr;
    dev->OpenPropertyStore(STGM_READ, &ps);
    PROPVARIANT df; PropVariantInit(&df);
    ps->GetValue(PK_DevFmt, &df);
    WAVEFORMATEX* fmt = nullptr;
    if (df.vt == VT_BLOB && df.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        fmt = (WAVEFORMATEX*)CoTaskMemAlloc(df.blob.cbSize);
        memcpy(fmt, df.blob.pBlobData, df.blob.cbSize);
    }
    PropVariantClear(&df); ps->Release();
    if (!fmt || fmt->nChannels < 4) { printf("FAIL 设备不是 >=4 声道，没法走 ch3/4 独占触觉\n"); return 1; }

    IAudioClient* ac = nullptr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (FAILED(hr) || !ac) { printf("FAIL Activate hr=0x%08lx\n", (unsigned long)hr); return 1; }

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    ac->GetDevicePeriod(&defPeriod, &minPeriod);
    REFERENCE_TIME dur = defPeriod;
    hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, fmt, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 frames = 0; ac->GetBufferSize(&frames);
        dur = (REFERENCE_TIME)(10000.0 * 1000.0 / fmt->nSamplesPerSec * frames + 0.5);
        ac->Release();
        dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, fmt, nullptr);
    }
    if (FAILED(hr)) { printf("FAIL Initialize hr=0x%08lx（设备可能被只狼/DSX占用，先关掉）\n", (unsigned long)hr); return 1; }

    bool isFloat = (fmt->wBitsPerSample == 32) && (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE || fmt->wFormatTag == 3);
    int bits = fmt->wBitsPerSample;
    int channels = fmt->nChannels;
    int rate = (int)fmt->nSamplesPerSec;
    printf(">> ch=%d rate=%d bits=%d float=%d\n\n", channels, rate, bits, isFloat ? 1 : 0);

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ac->SetEventHandle(evt);
    UINT32 bufferFrames = 0;
    ac->GetBufferSize(&bufferFrames);
    IAudioRenderClient* render = nullptr;
    hr = ac->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    if (FAILED(hr) || !render) { printf("FAIL GetService\n"); return 1; }

    BYTE* data = nullptr;
    if (SUCCEEDED(render->GetBuffer(bufferFrames, &data))) render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    hr = ac->Start();
    if (FAILED(hr)) { printf("FAIL Start\n"); return 1; }

    for (int v = 0; v < variantCount; ++v) {
        printf(">> [%d/%d] %s\n", v + 1, variantCount, variants[v].label);

        // 固定种子：每组都从同一个起点生成，波形固定可复现，跟 dllmain.cpp 的固定脉冲表一致
        // （早期这里用的是跨组一直递增的种子，同一档参数每次放的噪声实际不一样，波形本身的
        // 随机起伏会让人感觉"有时候一下有时候两下"——这个坑已经在正式代码里改掉了）。
        uint32_t seed = 0xABCD1234u;
        const int pulseFrames = rate / 20; // 50ms
        float* pulseBuf = new float[pulseFrames];
        render_pulse(pulseBuf, pulseFrames, variants[v], seed);

        const int silenceFrames = rate * 12 / 10; // 1.2s 停顿
        int totalFrames = pulseFrames + silenceFrames;
        int written = 0;
        while (written < totalFrames) {
            if (WaitForSingleObject(evt, 2000) != WAIT_OBJECT_0) { printf("FAIL render timeout\n"); break; }
            UINT32 padding = 0; ac->GetCurrentPadding(&padding);
            UINT32 available = bufferFrames - padding;
            if (!available) continue;
            hr = render->GetBuffer(available, &data);
            if (FAILED(hr)) break;
            for (UINT32 i = 0; i < available; ++i) {
                float sample = (written < pulseFrames) ? pulseBuf[written] : 0.0f;
                for (int ch = 0; ch < channels; ++ch)
                    write_sample(data, i, ch, channels, isFloat, bits, (ch == 2 || ch == 3) ? sample : 0.0f);
                ++written;
            }
            render->ReleaseBuffer(available, 0);
        }
        delete[] pulseBuf;
    }

    ac->Stop();
    printf("\n>> 全部播完\n");

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    render->Release();
    CloseHandle(evt);
    ac->Release();
    dev->Release();
    en->Release();
    CoTaskMemFree(fmt);
    CoUninitialize();
    return 0;
}
