// DualSense speaker path probe.
// Writes a short beep pattern to ch1/ch2 only and keeps ch3/ch4 silent.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>
#include <propvarutil.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

static const PROPERTYKEY PK_DevFmt = { {0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}}, 0 };
static const double PI = 3.14159265358979323846;

static bool contains_ci(const char* text, const char* needle) {
    if (!text || !needle || !*needle) return true;
    size_t nl = strlen(needle);
    for (const char* p = text; *p; ++p) {
        size_t i = 0;
        for (; i < nl; ++i) {
            char a = p[i];
            char b = needle[i];
            if (!a) return false;
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (i == nl) return true;
    }
    return false;
}

static void wstr_to_utf8(const wchar_t* w, char* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, nullptr, nullptr);
}

static IMMDevice* find_target(IMMDeviceEnumerator* en, const char* target) {
    if (target && target[0] == '{') {
        wchar_t id[512];
        MultiByteToWideChar(CP_UTF8, 0, target, -1, id, 512);
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDevice(id, &dev)) && dev) return dev;
        return nullptr;
    }

    IMMDeviceCollection* col = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    UINT count = 0;
    col->GetCount(&count);
    printf(">> active render endpoints:\n");

    IMMDevice* found = nullptr;
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr;
        col->Item(i, &dev);
        IPropertyStore* ps = nullptr;
        dev->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT name;
        PropVariantInit(&name);
        ps->GetValue(PKEY_Device_FriendlyName, &name);
        char friendly[512] = "";
        if (name.vt == VT_LPWSTR) wstr_to_utf8(name.pwszVal, friendly, sizeof(friendly));
        printf("   [%u] %s\n", i, friendly[0] ? friendly : "(?)");
        if (!found && contains_ci(friendly, target ? target : "DualSense")) {
            found = dev;
            found->AddRef();
        }
        PropVariantClear(&name);
        ps->Release();
        dev->Release();
    }
    col->Release();
    return found;
}

static bool is_float_format(const WAVEFORMATEX* fmt) {
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE* ext = (const WAVEFORMATEXTENSIBLE*)fmt;
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

static void write_sample(BYTE* dst, UINT32 frame, int channel, int channels, const WAVEFORMATEX* fmt, float sample) {
    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    int index = (int)frame * channels + channel;
    if (is_float_format(fmt) && fmt->wBitsPerSample == 32) {
        ((float*)dst)[index] = sample;
        return;
    }
    if (fmt->wBitsPerSample == 16) {
        ((int16_t*)dst)[index] = (int16_t)(sample * 32767.0f);
        return;
    }
    if (fmt->wBitsPerSample == 24) {
        int32_t v = (int32_t)(sample * 8388607.0f);
        BYTE* p = dst + (size_t)index * 3;
        p[0] = (BYTE)(v & 0xff);
        p[1] = (BYTE)((v >> 8) & 0xff);
        p[2] = (BYTE)((v >> 16) & 0xff);
        return;
    }
    if (fmt->wBitsPerSample == 32) {
        ((int32_t*)dst)[index] = (int32_t)(sample * 2147483647.0f);
    }
}

static float beep_sample(int frame, int rate, float gain) {
    const int cycle = rate / 2;
    const int beep = rate * 140 / 1000;
    int cyclePos = frame % cycle;
    int beepIndex = frame / cycle;
    if (beepIndex >= 4 || cyclePos >= beep) return 0.0f;
    double t = (double)frame / (double)rate;
    double freq = (beepIndex & 1) ? 1600.0 : 1050.0;
    double fade = 1.0;
    int fadeFrames = rate * 8 / 1000;
    if (cyclePos < fadeFrames) fade = (double)cyclePos / (double)fadeFrames;
    if (cyclePos > beep - fadeFrames) fade = (double)(beep - cyclePos) / (double)fadeFrames;
    if (fade < 0.0) fade = 0.0;
    return (float)(sin(2.0 * PI * freq * t) * fade * gain);
}

static const char* audio_error_hint(HRESULT hr) {
    switch ((unsigned long)hr) {
    case 0x8889000a: return "device in use: close Sekiro/DSX/this app's active haptic output, then retry";
    case 0x88890008: return "unsupported format";
    case 0x88890019: return "exclusive mode is disabled for this endpoint";
    default: return "see WASAPI HRESULT";
    }
}

int main(int argc, char** argv) {
    const char* target = argc > 1 ? argv[1] : "DualSense";
    float gain = argc > 2 ? (float)atof(argv[2]) : 0.12f;
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 0.5f) gain = 0.5f;

    printf(">> DualSense speaker probe: target=\"%s\" gain=%.2f\n", target, gain);
    printf(">> This writes only ch1/ch2. ch3/ch4 stay silent. Listen for four short beeps.\n");

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { printf("FAIL CoInitializeEx hr=0x%08lx\n", (unsigned long)hr); return 1; }

    IMMDeviceEnumerator* en = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr) || !en) { printf("FAIL MMDeviceEnumerator hr=0x%08lx\n", (unsigned long)hr); return 1; }

    IMMDevice* dev = find_target(en, target);
    if (!dev) { printf("FAIL target render endpoint not found\n"); en->Release(); CoUninitialize(); return 1; }

    IPropertyStore* ps = nullptr;
    dev->OpenPropertyStore(STGM_READ, &ps);
    PROPVARIANT name;
    PropVariantInit(&name);
    ps->GetValue(PKEY_Device_FriendlyName, &name);
    char friendly[512] = "";
    if (name.vt == VT_LPWSTR) wstr_to_utf8(name.pwszVal, friendly, sizeof(friendly));
    printf(">> selected: %s\n", friendly[0] ? friendly : "(?)");
    PropVariantClear(&name);

    IAudioClient* ac = nullptr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (FAILED(hr) || !ac) { printf("FAIL Activate hr=0x%08lx\n", (unsigned long)hr); return 1; }

    PROPVARIANT df;
    PropVariantInit(&df);
    ps->GetValue(PK_DevFmt, &df);
    WAVEFORMATEX* fmt = nullptr;
    bool exclusive = false;
    if (df.vt == VT_BLOB && df.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        fmt = (WAVEFORMATEX*)CoTaskMemAlloc(df.blob.cbSize);
        memcpy(fmt, df.blob.pBlobData, df.blob.cbSize);
        exclusive = fmt->nChannels >= 4;
    }
    PropVariantClear(&df);
    ps->Release();
    if (!fmt) { printf("FAIL cannot read device format\n"); return 1; }

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    ac->GetDevicePeriod(&defPeriod, &minPeriod);
    if (exclusive) {
        REFERENCE_TIME dur = defPeriod;
        hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, fmt, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 frames = 0;
            ac->GetBufferSize(&frames);
            dur = (REFERENCE_TIME)(10000.0 * 1000.0 / fmt->nSamplesPerSec * frames + 0.5);
            ac->Release();
            dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
            hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, fmt, nullptr);
        }
    } else {
        hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    if (FAILED(hr)) {
        printf(">> exclusive init failed or not suitable: 0x%08lx (%s), trying shared mix format\n",
               (unsigned long)hr, audio_error_hint(hr));
        if (ac) ac->Release();
        dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        CoTaskMemFree(fmt);
        fmt = nullptr;
        hr = ac->GetMixFormat(&fmt);
        if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, fmt, nullptr);
        exclusive = false;
    }
    if (FAILED(hr)) { printf("FAIL Initialize hr=0x%08lx (%s)\n", (unsigned long)hr, audio_error_hint(hr)); return 1; }
    if (fmt->nChannels < 2) { printf("FAIL endpoint has less than 2 channels\n"); return 1; }
    if (!(is_float_format(fmt) && fmt->wBitsPerSample == 32) && fmt->wBitsPerSample != 16 && fmt->wBitsPerSample != 24 && fmt->wBitsPerSample != 32) {
        printf("FAIL unsupported format bits=%u tag=0x%04x\n", fmt->wBitsPerSample, fmt->wFormatTag);
        return 1;
    }

    printf(">> mode=%s ch=%u rate=%lu bits=%u float=%d\n", exclusive ? "exclusive" : "shared", fmt->nChannels,
           fmt->nSamplesPerSec, fmt->wBitsPerSample, is_float_format(fmt) ? 1 : 0);

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ac->SetEventHandle(evt);
    UINT32 bufferFrames = 0;
    ac->GetBufferSize(&bufferFrames);
    IAudioRenderClient* render = nullptr;
    hr = ac->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    if (FAILED(hr) || !render) { printf("FAIL GetService hr=0x%08lx\n", (unsigned long)hr); return 1; }

    BYTE* data = nullptr;
    if (SUCCEEDED(render->GetBuffer(bufferFrames, &data))) render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    hr = ac->Start();
    if (FAILED(hr)) { printf("FAIL Start hr=0x%08lx\n", (unsigned long)hr); return 1; }

    int rate = (int)fmt->nSamplesPerSec;
    int totalFrames = rate * 2400 / 1000;
    int written = 0;
    while (written < totalFrames) {
        if (WaitForSingleObject(evt, 2000) != WAIT_OBJECT_0) { printf("FAIL render timeout\n"); break; }
        UINT32 padding = 0;
        ac->GetCurrentPadding(&padding);
        UINT32 available = bufferFrames - padding;
        if (!available) continue;
        hr = render->GetBuffer(available, &data);
        if (FAILED(hr)) { printf("FAIL GetBuffer hr=0x%08lx\n", (unsigned long)hr); break; }
        for (UINT32 i = 0; i < available; ++i) {
            float sample = written < totalFrames ? beep_sample(written, rate, gain) : 0.0f;
            for (int ch = 0; ch < fmt->nChannels; ++ch) {
                write_sample(data, i, ch, fmt->nChannels, fmt, (ch == 0 || ch == 1) ? sample : 0.0f);
            }
            ++written;
        }
        render->ReleaseBuffer(available, 0);
    }

    ac->Stop();
    printf(">> done. If you heard four short beeps from the controller body, ch1/ch2 can drive speaker audio.\n");
    printf(">> If you heard nothing, or only felt vibration, this endpoint does not expose the controller speaker as normal WASAPI ch1/ch2.\n");

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