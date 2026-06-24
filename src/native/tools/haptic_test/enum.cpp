// 枚举所有状态的 WASAPI 渲染端点 → UTF-8 日志。
// 打印：状态、真实硬件格式(PKEY_AudioEngine_DeviceFormat)、并测独占 4ch。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <stdio.h>
#include <propkey.h>

// PKEY_AudioEngine_DeviceFormat
const PROPERTYKEY PK_DevFmt = { {0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}}, 0 };

static FILE* g_f = nullptr;
static void L(const char* fmt, ...) { va_list a; va_start(a, fmt); vfprintf(g_f, fmt, a); va_end(a); }
static void Lw(const wchar_t* w) {
    char buf[512] = ""; if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    fputs(buf, g_f);
}
static HRESULT testExt(IAudioClient* ac, int ch, int rate, int bits, DWORD mask) {
    WAVEFORMATEXTENSIBLE w{}; w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; w.Format.nChannels=(WORD)ch;
    w.Format.nSamplesPerSec=rate; w.Format.wBitsPerSample=(WORD)bits; w.Format.nBlockAlign=(WORD)(ch*bits/8);
    w.Format.nAvgBytesPerSec=rate*w.Format.nBlockAlign; w.Format.cbSize=22; w.Samples.wValidBitsPerSample=(WORD)bits;
    w.dwChannelMask=mask; w.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;
    return ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&w, nullptr);
}
static const char* stateStr(DWORD s){ switch(s){case 1:return "ACTIVE";case 2:return "DISABLED";case 4:return "NOTPRESENT";case 8:return "UNPLUGGED";default:return "?";} }

int main() {
    g_f = fopen("enum_out.txt", "wb");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    IMMDeviceCollection* col = nullptr;
    en->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &col);
    UINT n = 0; col->GetCount(&n);
    L("=== %u render endpoints (all states) ===\n", n);
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* dev = nullptr; col->Item(i, &dev);
        DWORD st = 0; dev->GetState(&st);
        IPropertyStore* ps = nullptr; dev->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT nm; PropVariantInit(&nm); ps->GetValue(PKEY_Device_FriendlyName, &nm);
        L("[%u] (%s) ", i, stateStr(st)); Lw(nm.vt==VT_LPWSTR?nm.pwszVal:L"(?)"); L("\n");
        // 真实硬件格式
        PROPVARIANT df; PropVariantInit(&df);
        if (SUCCEEDED(ps->GetValue(PK_DevFmt, &df)) && df.vt==VT_BLOB && df.blob.cbSize>=sizeof(WAVEFORMATEX)) {
            WAVEFORMATEX* f = (WAVEFORMATEX*)df.blob.pBlobData;
            L("    DeviceFormat: ch=%u rate=%lu bits=%u\n", f->nChannels, f->nSamplesPerSec, f->wBitsPerSample);
        }
        PropVariantClear(&df);
        // 仅对 DualSense / USBAudio 端点测 4ch（其它跳过省噪声）
        char nbuf[512]=""; if(nm.vt==VT_LPWSTR) WideCharToMultiByte(CP_UTF8,0,nm.pwszVal,-1,nbuf,sizeof(nbuf),0,0);
        if (st==DEVICE_STATE_ACTIVE && (strstr(nbuf,"DualSense")||strstr(nbuf,"USBAudio"))) {
            IAudioClient* ac=nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) && ac) {
                L("    EXCL 4ch/48k/16 mask0 -> 0x%08lx\n", (unsigned long)testExt(ac,4,48000,16,0));
                L("    EXCL 4ch/48k/16 QUAD  -> 0x%08lx\n", (unsigned long)testExt(ac,4,48000,16,KSAUDIO_SPEAKER_QUAD));
                L("    EXCL 4ch/48k/24 mask0 -> 0x%08lx\n", (unsigned long)testExt(ac,4,48000,24,0));
                ac->Release();
            }
        }
        PropVariantClear(&nm); ps->Release(); dev->Release();
    }
    col->Release(); en->Release(); CoUninitialize();
    fclose(g_f); return 0;
}
