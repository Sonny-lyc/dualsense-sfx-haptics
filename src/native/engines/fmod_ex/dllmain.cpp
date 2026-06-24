// ============================================================================
// fmod_probe — FMOD Ex 代理 DLL 探针（只狼 / Sekiro）
// ----------------------------------------------------------------------------
// 目的：只读地观察游戏播放了哪些声音，搞清楚每个声音来自哪个 .fsb bank，
//       从而验证"排除 sm*(音乐) / vm*(语音)、其余全震"这条过滤规则是否成立。
//
// 工作方式（纯 DLL 代理，不依赖 MinHook）：
//   1. 把游戏目录里的真 fmodex64.dll 改名为 fmodex64_orig.dll
//   2. 本 DLL 编译出来也叫 fmodex64.dll，放进游戏目录
//   3. fmodex64.def 把 706 个导出原样转发给 fmodex64_orig.dll，
//      只有 createSound / createStream / playSound 三个换成下面的实现
//   4. 我们的实现包一层日志，再调用真函数，行为对游戏完全透明
//
// 输出：日志写到  %USERPROFILE%\Desktop\fmod_probe_log.txt
//       每行形如：  PLAY ch=-1 bank=smain.fsb sub="670" -> VIBRATE
//                   PLAY ch=-1 bank=sm11.fsb  sub="3"   -> SKIP(music)
//
// ⚠️ 本探针不插手柄、不震动、不改任何游戏行为，纯打印。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <cstdarg>

// --- FMOD 不透明类型，避免引入 FMOD 头文件 ---
typedef int FMOD_RESULT;                 // 0 == FMOD_OK
static const unsigned FMOD_OPENMEMORY       = 0x00000800;
static const unsigned FMOD_OPENMEMORY_POINT = 0x10000000;

// --- 真函数指针（从 fmodex64_orig.dll 解析）---
typedef FMOD_RESULT (*createSound_t)(void* self, const char* nameOrData, unsigned mode, void* exinfo, void** sound);
typedef FMOD_RESULT (*createStream_t)(void* self, const char* nameOrData, unsigned mode, void* exinfo, void** sound);
typedef FMOD_RESULT (*playSound_t)(void* self, int channelid, void* sound, int paused, void** channel);
// SystemI::createSoundInternal —— 事件系统真正用来建声音的内部函数（公开 createSound 不会被调用）
// 签名: (this, const char* nameOrData, unsigned mode, unsigned, unsigned, exinfo*, File*, bool, SoundI** sound)
typedef FMOD_RESULT (*createSoundInternal_t)(void* self, const char* nameOrData, unsigned mode,
                                             unsigned u2, unsigned u3, void* exinfo, void* file,
                                             bool c, void** sound);

// 辅助 C API（用扁平 C 导出，调用最省事）
typedef FMOD_RESULT (*Sound_GetName_t)(void* sound, char* name, int namelen);
typedef FMOD_RESULT (*Sound_GetSubSoundParent_t)(void* sound, void** parent);
typedef FMOD_RESULT (*Sound_GetNumSubSounds_t)(void* sound, int* numsubsounds);
typedef FMOD_RESULT (*Sound_GetSubSound_t)(void* sound, int index, void** subsound);

static createSound_t              g_createSound  = nullptr;
static createStream_t             g_createStream = nullptr;
static createSoundInternal_t      g_createSoundInternal = nullptr;
static playSound_t                g_playSound    = nullptr;
static Sound_GetName_t            g_getName      = nullptr;
static Sound_GetSubSoundParent_t g_getParent    = nullptr;
static Sound_GetNumSubSounds_t   g_getNumSub    = nullptr;
static Sound_GetSubSound_t       g_getSub       = nullptr;

static HMODULE g_orig = nullptr;

// jmp 跳转桩的目标地址表：thunks.asm 里每个 thunk_i 执行 jmp [g_thunk_targets+i*8]。
// do_init() 用 GetProcAddress(g_orig, 序号) 把 360 个原函数地址填进来。
// （为什么不用 PE forwarder：Windows 的 forwarder 解析走受限搜索路径，不含应用目录，
//  转发到游戏目录里的 fmodex64_orig.dll 会失败——实测 err 127。jmp 桩绕开这个限制。）
#include "thunks_gen.h"
extern "C" void* g_thunk_targets[THUNK_COUNT] = { nullptr };

// Sound* -> 来源 bank 文件名（basename）
static std::unordered_map<void*, std::string> g_soundBank;
static std::mutex g_lock;
static FILE* g_log = nullptr;

// ----------------------------------------------------------------------------
static const char* basename_ascii(const char* path) {
    if (!path) return "";
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

// 按文件名分类：music = sm + 数字, voice = vm*, 其余 = 震动
// （smain.fsb 不是音乐：sm 后面是 'a' 不是数字）
static const char* classify(const std::string& base) {
    auto starts = [&](const char* p) {
        return base.size() >= strlen(p) && _strnicmp(base.c_str(), p, strlen(p)) == 0;
    };
    if (starts("vm")) return "SKIP(voice)";
    if (starts("sm") && base.size() > 2 && isdigit((unsigned char)base[2])) return "SKIP(music)";
    return "VIBRATE";
}

static void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// 查某个 Sound*（可能是子音）属于哪个 bank
static std::string lookup_bank(void* sound) {
    if (!sound) return "(null)";
    {
        std::lock_guard<std::mutex> g(g_lock);
        auto it = g_soundBank.find(sound);
        if (it != g_soundBank.end()) return it->second;
    }
    // 不是直接登记的 Sound：尝试反查父音
    if (g_getParent) {
        void* parent = nullptr;
        if (g_getParent(sound, &parent) == 0 && parent && parent != sound) {
            std::lock_guard<std::mutex> g(g_lock);
            auto it = g_soundBank.find(parent);
            if (it != g_soundBank.end()) return it->second;
        }
    }
    return "(unknown)";
}

// 纯只读诊断：对前若干个不同的 unknown 声音，打印它的结构信息，
// 用来搞清楚 playSound 拿到的 Sound* 到底是什么、能不能反查到已登记的 bank。
// 只用只读查询（getParent / GetName / GetNumSubSounds），绝不调用 GetSubSound。
static std::unordered_set<void*> g_diag;
static int g_diagCount = 0;
static void diagnose_unknown(void* sound) {
    if (!sound || g_diagCount > 150) return;
    {
        std::lock_guard<std::mutex> g(g_lock);
        if (g_diag.count(sound)) return;
        g_diag.insert(sound);
        g_diagCount++;
    }
    // 1 级父
    void* parent = nullptr; int pr = -1;
    if (g_getParent) pr = g_getParent(sound, &parent);
    std::string pbank = "(noparent)";
    void* gp = nullptr;
    if (parent) {
        { std::lock_guard<std::mutex> g(g_lock);
          auto it = g_soundBank.find(parent);
          pbank = (it != g_soundBank.end()) ? it->second : "(parent-unreg)"; }
        if (g_getParent) g_getParent(parent, &gp);  // 2 级父
    }
    std::string gpbank = "";
    if (gp) { std::lock_guard<std::mutex> g(g_lock);
              auto it = g_soundBank.find(gp);
              gpbank = (it != g_soundBank.end()) ? it->second : "(gp-unreg)"; }
    char nm[128] = ""; if (g_getName) { if (g_getName(sound, nm, sizeof(nm)) != 0) nm[0] = '\0'; }
    int nsub = -1; if (g_getNumSub) g_getNumSub(sound, &nsub);
    logf("DIAG  sound=%p name=\"%s\" nsub=%d | parent=%p(r=%d) pbank=%s | gp=%p %s",
         sound, nm, nsub, parent, pr, pbank.c_str(), gp, gpbank.c_str());
}

// ============================================================================
// 震动输出：音效(VIBRATE)通道路由到我们建的声道组，组上挂一个捕获 DSP。
// DSP 回调在 FMOD 混音线程上拿到【连续的音效混音 PCM】，降混单声道写入环形缓冲；
// haptic_out 渲染线程从环形缓冲读 → DualSense ch3/4 触觉音圈。
// → 不跨线程碰通道(不崩)、PCM 连续(不突突突)、只含音效(音乐不进组)。
// ============================================================================
#include "haptic_out.h"

// --- FMOD Ex 4.44 的 DSP 描述结构体（布局必须与该版本一致，否则崩） ---
typedef struct FMOD_DSP_STATE_ { void* instance; void* plugindata; unsigned short speakermask; } FMOD_DSP_STATE_;
// FMOD Ex 4.44 read 回调：最后两个参数都是 int 按值(inchannels, outchannels)，不是指针！
typedef FMOD_RESULT (*FMOD_DSP_READCB)(FMOD_DSP_STATE_*, float*, float*, unsigned int, int, int);
typedef struct FMOD_DSP_DESCRIPTION_ {
    char name[32]; unsigned int version; int channels;
    void* create; void* release; void* reset;
    FMOD_DSP_READCB read; void* setposition;
    int numparameters; void* paramdesc;
    void* setparameter; void* getparameter; void* config;
    int configwidth; int configheight; void* userdata;
} FMOD_DSP_DESCRIPTION_;

typedef FMOD_RESULT (*CreateCG_t)(void*, const char*, void**);
typedef FMOD_RESULT (*CreateDSP_t)(void*, const FMOD_DSP_DESCRIPTION_*, void**);
typedef FMOD_RESULT (*AddDSP_t)(void*, void*, void**);
typedef FMOD_RESULT (*SetCG_t)(void*, void*);
typedef FMOD_RESULT (*GetMaster_t)(void*, void**);
static CreateCG_t  g_createCG  = nullptr;
static CreateDSP_t g_createDSP = nullptr;
static AddDSP_t    g_addDSP    = nullptr;
static SetCG_t     g_setCG     = nullptr;
static GetMaster_t g_getMaster = nullptr;

// 环形缓冲（单生产者=DSP混音线程，单消费者=WASAPI线程）
static const unsigned RING = 16384, RMASK = RING - 1;
static float g_ring[RING];
static std::atomic<unsigned> g_wr{0}, g_rd{0};

// 门控：音效播放时打开(=1)，之后逐采样衰减。haptic 输出 = master音频 × 门。
static std::atomic<float> g_gate{0.0f};
static const float GATE_DK = 0.99975f;   // 每采样衰减（≈200ms 尾巴）

// DSP 回调：透传 + 降混单声道入环。务必防 in/out 为空、inch 异常。
static std::atomic<int> g_dspLogged{0};
static FMOD_RESULT dsp_read(FMOD_DSP_STATE_*, float* in, float* out, unsigned int length, int inch, int outch) {
    if (g_dspLogged.exchange(1) == 0)
        logf("DSP first read: in=%p out=%p length=%u inch=%d outch=%d", in, out, length, inch, outch);
    if (inch <= 0 || inch > 32) return 0;
    int ch = (outch > 0 && outch < inch) ? outch : inch;   // 透传的声道数
    if (out && in) memcpy(out, in, (size_t)length * ch * sizeof(float));
    if (!in) return 0;                                      // 无输入时不入环
    unsigned w = g_wr.load(std::memory_order_relaxed);
    float dpeak = 0;
    for (unsigned int i = 0; i < length; ++i) {
        float s = 0; for (int c = 0; c < inch; ++c) s += in[i * inch + c];
        if (inch > 1) s /= inch;
        g_ring[w & RMASK] = s; ++w;
        float a = s < 0 ? -s : s; if (a > dpeak) dpeak = a;
    }
    g_wr.store(w, std::memory_order_release);
    // 诊断：每约 200 次回调（~4s）报一次本批峰值
    static std::atomic<int> dc{0};
    static float dmax = 0; if (dpeak > dmax) dmax = dpeak;
    if (dc.fetch_add(1) % 200 == 199) { logf("DSP peak(last4s)=%.4f", dmax); dmax = 0; }
    return 0;  // FMOD_OK
}

// 由 haptic_out 渲染线程调用：从环形缓冲连续读 n 个单声道样本
extern "C" int haptic_pull_audio(float* out, int n) {
    unsigned wr = g_wr.load(std::memory_order_acquire);
    unsigned rd = g_rd.load(std::memory_order_relaxed);
    unsigned avail = wr - rd;
    if (avail > (unsigned)n + RING / 2) rd = wr - n;   // 落后太多→跳近，限延迟
    float gate = g_gate.load(std::memory_order_relaxed);
    int got = 0; float peak = 0;
    for (int i = 0; i < n; ++i) {
        float v = 0;
        if (rd != wr) { v = g_ring[rd & RMASK]; ++rd; got = 1; }
        v *= gate; gate *= GATE_DK;                     // 乘门 + 衰减
        out[i] = v;
        float a = v < 0 ? -v : v; if (a > peak) peak = a;
    }
    g_rd.store(rd, std::memory_order_relaxed);
    g_gate.store(gate, std::memory_order_relaxed);
    // 诊断：每约 300 次（~3s）报一次消费端
    static std::atomic<int> pc{0}; static float pmax = 0; static unsigned amax = 0;
    if (peak > pmax) pmax = peak; if (avail > amax) amax = avail;
    if (pc.fetch_add(1) % 300 == 299) { logf("PULL peak=%.4f availMax=%u", pmax, amax); pmax = 0; amax = 0; }
    return (got && peak >= 0.004f) ? 1 : 0;
}

// 懒创建捕获组+DSP（需要 System*，由 playSound 的 self 提供）
static void* g_capSystem = nullptr;
static void* g_hapGroup  = nullptr;
static std::once_flag g_capOnce;
static void capture_init() {
    if (!g_createDSP || !g_addDSP || !g_capSystem) { logf("CAP: 函数指针/System 缺失"); return; }
    if (g_createCG) g_createCG(g_capSystem, "haptic", &g_hapGroup);   // 备用组（暂不挂DSP）
    FMOD_DSP_DESCRIPTION_ d{};
    d.name[0] = 'h'; d.name[1] = 'a'; d.name[2] = 'p'; d.name[3] = 0;
    d.version = 1; d.channels = 0; d.read = dsp_read;
    void* dsp = nullptr;
    if (g_createDSP(g_capSystem, &d, &dsp) != 0 || !dsp) { logf("CAP: CreateDSP 失败"); return; }
    // 测试：把 DSP 挂到 master 主组（全部音频都过它），确认捕获机制是否工作
    void* master = nullptr; void* conn = nullptr;
    if (g_getMaster && g_getMaster(g_capSystem, &master) == 0 && master) {
        g_addDSP(master, dsp, &conn);
        logf("CAP: DSP 挂到 MASTER 主组(测试)");
    } else {
        logf("CAP: 取 master 失败");
    }
}
static inline void ensure_capture(void* system) {
    if (!g_capSystem) g_capSystem = system;
    std::call_once(g_capOnce, capture_init);
}

static std::once_flag g_hapticOnce;
static void haptic_init() { haptic_out_start(); logf("HAPTIC: out started"); }
static inline void ensure_haptic() { std::call_once(g_hapticOnce, haptic_init); }

// ----------------------------------------------------------------------------
// 惰性初始化：首次进入任一拦截点时执行，避开 DllMain 的 loader-lock。
// ----------------------------------------------------------------------------
static std::once_flag g_once;
static void do_init();
static inline void ensure_init() { std::call_once(g_once, do_init); }

// ----------------------------------------------------------------------------
// 三个被拦截的导出（.def 里 alias 到这些符号；extern "C" 保证名字不被修饰）
// ----------------------------------------------------------------------------
extern "C" FMOD_RESULT createSound_detour(void* self, const char* nameOrData, unsigned mode, void* exinfo, void** sound) {
    ensure_init();
    FMOD_RESULT r = g_createSound(self, nameOrData, mode, exinfo, sound);
    bool isMem = (mode & (FMOD_OPENMEMORY | FMOD_OPENMEMORY_POINT)) != 0;
    if (r == 0 && sound && *sound && !isMem && nameOrData) {
        std::string base = basename_ascii(nameOrData);
        { std::lock_guard<std::mutex> g(g_lock); g_soundBank[*sound] = base; }
        logf("CREATE sound=%p bank=%s mode=0x%08x", *sound, base.c_str(), mode);
    }
    return r;
}

extern "C" FMOD_RESULT createStream_detour(void* self, const char* nameOrData, unsigned mode, void* exinfo, void** sound) {
    ensure_init();
    FMOD_RESULT r = g_createStream(self, nameOrData, mode, exinfo, sound);
    bool isMem = (mode & (FMOD_OPENMEMORY | FMOD_OPENMEMORY_POINT)) != 0;
    if (r == 0 && sound && *sound && !isMem && nameOrData) {
        std::string base = basename_ascii(nameOrData);
        { std::lock_guard<std::mutex> g(g_lock); g_soundBank[*sound] = base; }
        logf("STREAM sound=%p bank=%s mode=0x%08x", *sound, base.c_str(), mode);
    }
    return r;
}

// 事件系统建声音走这里。记录来源（文件名/内存）并登记 SoundI*->bank。
extern "C" FMOD_RESULT createSoundInternal_detour(void* self, const char* nameOrData, unsigned mode,
                                                  unsigned u2, unsigned u3, void* exinfo, void* file,
                                                  bool c, void** sound) {
    ensure_init();
    FMOD_RESULT r = g_createSoundInternal(self, nameOrData, mode, u2, u3, exinfo, file, c, sound);
    bool isMem = (mode & (FMOD_OPENMEMORY | FMOD_OPENMEMORY_POINT)) != 0;
    if (r == 0 && sound && *sound) {
        std::string base = (!isMem && nameOrData) ? basename_ascii(nameOrData) : "(mem)";
        { std::lock_guard<std::mutex> g(g_lock); g_soundBank[*sound] = base; }
        // 同时取 FMOD_Sound_GetName，看是否能拿到 FSB 内部名
        char nm[256] = "";
        if (g_getName) { if (g_getName(*sound, nm, sizeof(nm)) != 0) nm[0] = '\0'; }
        logf("CSI   sound=%p bank=%s mode=0x%08x name=\"%s\"", *sound, base.c_str(), mode, nm);
    }
    return r;
}

extern "C" FMOD_RESULT playSound_detour(void* self, int channelid, void* sound, int paused, void** channel) {
    ensure_init();
    FMOD_RESULT r = g_playSound(self, channelid, sound, paused, channel);

    std::string bank = lookup_bank(sound);
    char subname[256] = "";
    if (g_getName && sound) {
        if (g_getName(sound, subname, sizeof(subname)) != 0) subname[0] = '\0';
    }
    // 分类规则（实测确定）：
    //  - 来源 sm##(音乐) / vm##(语音)：这些是流式 bank，直接以"主声音"播放，能命中 → SKIP
    //  - (unknown)：采样音效以"子声音"播放，getParent 反查不到母对象，但实测这些子声音
    //    的父从不指向已登记的 sm/vm 主声音 → 它们都是音效 → VIBRATE
    //  - 已登记的非 sm/vm（smain 等）→ classify() 也归 VIBRATE
    const char* verdict;
    if (bank == "(null)")           verdict = "?";
    else if (bank == "(unknown)")   verdict = "VIBRATE(sfx-sub)";
    else                            verdict = classify(bank);
    logf("PLAY  ch=%d sound=%p bank=%s sub=\"%s\" -> %s",
         channelid, sound, bank.c_str(), subname, verdict);
    if (verdict[0] == 'V') {            // VIBRATE → 开门，让 master 音频在此刻透到触觉
        ensure_haptic();
        ensure_capture(self);
        g_gate.store(1.0f, std::memory_order_relaxed);
    }
    if (bank == "(unknown)") diagnose_unknown(sound);  // 仍只读记录少量样本，便于复核
    return r;
}

// ----------------------------------------------------------------------------
// 注：所有非 detour 导出（706 个，含 C 风格 FMOD_* 与 C++ 修饰名）都由 thunks.asm
// 的 jmp 桩透明转交，无需在此手写包装函数。do_init() 负责把 g_thunk_targets 填好。
// ----------------------------------------------------------------------------
static void do_init() {
    // 日志文件：桌面
    char path[MAX_PATH] = "fmod_probe_log.txt";
    char* up = nullptr; size_t n = 0;
    if (_dupenv_s(&up, &n, "USERPROFILE") == 0 && up) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\Desktop\\fmod_probe_log.txt", up);
        free(up);
    }
    g_log = nullptr;
    fopen_s(&g_log, path, "w");

    g_orig = LoadLibraryA("fmodex64_orig.dll");
    if (!g_orig) { logf("FATAL: cannot load fmodex64_orig.dll (err=%lu)", GetLastError()); return; }

    g_createSound  = (createSound_t)  GetProcAddress(g_orig, "?createSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z");
    g_createStream = (createStream_t) GetProcAddress(g_orig, "?createStream@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z");
    g_createSoundInternal = (createSoundInternal_t) GetProcAddress(g_orig, "?createSoundInternal@SystemI@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIIIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVFile@2@_NPEAPEAVSoundI@2@@Z");
    g_playSound    = (playSound_t)    GetProcAddress(g_orig, "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PEAVSound@2@_NPEAPEAVChannel@2@@Z");
    g_getName      = (Sound_GetName_t)            GetProcAddress(g_orig, "FMOD_Sound_GetName");
    g_getParent    = (Sound_GetSubSoundParent_t)  GetProcAddress(g_orig, "FMOD_Sound_GetSubSoundParent");
    g_getNumSub    = (Sound_GetNumSubSounds_t)    GetProcAddress(g_orig, "FMOD_Sound_GetNumSubSounds");
    g_getSub       = (Sound_GetSubSound_t)        GetProcAddress(g_orig, "FMOD_Sound_GetSubSound");
    // 震动用：声道组 + 捕获 DSP
    g_createCG  = (CreateCG_t)  GetProcAddress(g_orig, "FMOD_System_CreateChannelGroup");
    g_createDSP = (CreateDSP_t) GetProcAddress(g_orig, "FMOD_System_CreateDSP");
    g_addDSP    = (AddDSP_t)    GetProcAddress(g_orig, "FMOD_ChannelGroup_AddDSP");
    g_setCG     = (SetCG_t)     GetProcAddress(g_orig, "FMOD_Channel_SetChannelGroup");
    g_getMaster = (GetMaster_t) GetProcAddress(g_orig, "FMOD_System_GetMasterChannelGroup");

    // 填全部 jmp 桩的目标（按序号从原 dll 取）
    int thunkNull = 0;
    for (int i = 0; i < THUNK_COUNT; ++i) {
        g_thunk_targets[i] = (void*)GetProcAddress(g_orig, MAKEINTRESOURCEA(g_thunk_ords[i]));
        if (!g_thunk_targets[i]) thunkNull++;
    }

    logf("=== fmod_probe loaded ===");
    logf("thunks filled: %d/%d (null=%d)", THUNK_COUNT - thunkNull, THUNK_COUNT, thunkNull);
    logf("orig=%p createSound=%p createStream=%p createSoundInternal=%p playSound=%p getName=%p getParent=%p",
         g_orig, g_createSound, g_createStream, g_createSoundInternal, g_playSound, g_getName, g_getParent);
    if (!g_createSound || !g_playSound)
        logf("WARNING: 关键函数指针为空，导出名可能与本机 fmodex64.dll 不符，需重新核对");
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        // 提前初始化：加载 fmodex64_orig.dll 并填好 360 个 jmp 桩的目标地址。
        // 必须在此完成——任何导出（修饰名桩）都可能在游戏第一帧就被调用，
        // 而桩没有自初始化能力（纯 jmp）。加载真 FMOD dll（DllMain 极简）在
        // loader-lock 下是安全的，这也是 dxgi/d3d9 等代理 dll 的通用做法。
        ensure_init();
    }
    return TRUE;
}
