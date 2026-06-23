// 往 DualSense 音频设备的 ch3/4(触觉音圈) 播放测试波形（WASAPI 独占+事件驱动）。
// ch1/2(头戴)填0，ch3/4 放一串阻尼正弦"啵"，让用户实际感受细腻触觉。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <avrt.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

const PROPERTYKEY PK_DevFmt = { {0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}}, 0 };
static const double PI = 3.14159265358979;

#define CK(hr, msg) do{ if(FAILED(hr)){ printf("FAIL %s hr=0x%08lx\n", msg, (unsigned long)hr); return 1; } }while(0)

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);

    // 找激活的、名字含 DualSense 的渲染端点
    IMMDeviceCollection* col = nullptr;
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    UINT n = 0; col->GetCount(&n);
    IMMDevice* dsdev = nullptr;
    printf("=== active render endpoints ===\n");
    for (UINT i = 0; i < n; ++i) {
        IMMDevice* d = nullptr; col->Item(i, &d);
        IPropertyStore* ps = nullptr; d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT nm; PropVariantInit(&nm); ps->GetValue(PKEY_Device_FriendlyName, &nm);
        char nb[512] = ""; if (nm.vt==VT_LPWSTR) WideCharToMultiByte(CP_UTF8,0,nm.pwszVal,-1,nb,sizeof(nb),0,0);
        printf("  [%u] %s\n", i, nb);
        if (!dsdev && strstr(nb, "DualSense")) { dsdev = d; d->AddRef(); }
        PropVariantClear(&nm); ps->Release(); d->Release();
    }
    col->Release();
    if (!dsdev) { printf("!! 没找到激活的 DualSense 渲染端点\n"); return 1; }
    printf(">> 选中 DualSense 端点\n");

    IAudioClient* ac = nullptr;
    CK(dsdev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac), "Activate");

    // 直接用设备的真实硬件格式 blob 去初始化（别自己拼，否则掩码/cbSize 对不上被拒）
    IPropertyStore* ps = nullptr; dsdev->OpenPropertyStore(STGM_READ, &ps);
    PROPVARIANT df; PropVariantInit(&df); ps->GetValue(PK_DevFmt, &df);
    if (!(df.vt==VT_BLOB && df.blob.cbSize>=sizeof(WAVEFORMATEX))) { printf("!! 读不到 DeviceFormat\n"); return 1; }
    WAVEFORMATEX* pfmt = (WAVEFORMATEX*)malloc(df.blob.cbSize);
    memcpy(pfmt, df.blob.pBlobData, df.blob.cbSize);
    printf(">> DeviceFormat ch=%u rate=%lu bits=%u tag=0x%04x cb=%u\n",
           pfmt->nChannels, pfmt->nSamplesPerSec, pfmt->wBitsPerSample, pfmt->wFormatTag, pfmt->cbSize);
    PropVariantClear(&df); ps->Release();

    const int CH = pfmt->nChannels;
    const int RATE = pfmt->nSamplesPerSec;
    const int BITS = pfmt->wBitsPerSample;
    int hapL = (CH >= 4) ? 2 : 0;   // ch3/4 = 索引 2/3
    int hapR = (CH >= 4) ? 3 : (CH-1);

    HRESULT hr = ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, pfmt, nullptr);
    printf(">> IsFormatSupported(EXCLUSIVE %dch/%d/%d) = 0x%08lx\n", CH, RATE, BITS, (unsigned long)hr);

    REFERENCE_TIME defP=0, minP=0; ac->GetDevicePeriod(&defP, &minP);
    REFERENCE_TIME dur = defP;
    hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, pfmt, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 fr=0; ac->GetBufferSize(&fr);
        dur = (REFERENCE_TIME)(10000.0*1000/RATE*fr + 0.5);
        ac->Release(); dsdev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
        hr = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, dur, pfmt, nullptr);
    }
    CK(hr, "Initialize");

    HANDLE evt = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    ac->SetEventHandle(evt);
    UINT32 bufFrames=0; ac->GetBufferSize(&bufFrames);
    IAudioRenderClient* rc = nullptr; CK(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc), "GetService");
    printf(">> bufFrames=%u, hapticCh=%d/%d, 开始播放测试波形...\n", bufFrames, hapL+1, hapR+1);

    // 测试节目：阻尼正弦"啵"序列
    struct Pulse { double f, tau; int gapMs; };
    Pulse seq[] = {
        {94,0.08,400},{94,0.08,400},{94,0.08,400},{94,0.08,700},
        {60,0.10,400},{60,0.10,400},{60,0.10,700},
        {180,0.05,300},{180,0.05,300},{180,0.05,300},{180,0.05,700},
        {120,0.50,1200},   // 一个长一点的持续型
    };
    int nSeq = sizeof(seq)/sizeof(seq[0]);

    DWORD taskIdx=0; AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    BYTE* p=nullptr; if (SUCCEEDED(rc->GetBuffer(bufFrames, &p))) rc->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    ac->Start();

    int cur=0; double t=0; int pulseFrames=0; bool inGap=true;
    int gapFrames = RATE/1000 * 300;        // 先静音 300ms
    double amp = 0.7 * 32767.0;
    bool done=false;
    int totalFramesPlayed=0; int maxFrames = RATE * 16;
    while (totalFramesPlayed < maxFrames && !done) {
        if (WaitForSingleObject(evt, 2000) != WAIT_OBJECT_0) break;
        UINT32 pad=0; ac->GetCurrentPadding(&pad);
        UINT32 avail = bufFrames - pad;
        if (avail == 0) continue;
        if (FAILED(rc->GetBuffer(avail, &p))) break;
        short* out = (short*)p;
        for (UINT32 i=0;i<avail;i++) {
            short v = 0;
            if (inGap) {
                if (--gapFrames <= 0) {
                    if (cur >= nSeq) { done=true; }
                    else { inGap=false; t=0; pulseFrames = (int)(seq[cur].tau*6*RATE); }
                }
            } else {
                double env = exp(-t/seq[cur].tau);
                v = (short)(amp * env * sin(2*PI*seq[cur].f*t));
                t += 1.0/RATE;
                if (--pulseFrames <= 0) { inGap=true; gapFrames = RATE/1000*seq[cur].gapMs; cur++; }
            }
            for (int c=0;c<CH;c++) out[i*CH+c] = (c==hapL||c==hapR)? v : 0;
        }
        rc->ReleaseBuffer(avail, 0);
        totalFramesPlayed += avail;
    }
    ac->Stop();
    printf(">> 播放结束\n");
    rc->Release(); CloseHandle(evt); ac->Release(); dsdev->Release(); en->Release();
    CoUninitialize();
    return 0;
}
