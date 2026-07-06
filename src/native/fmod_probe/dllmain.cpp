// ============================================================================
// fmod_probe — FMOD Ex 代理 DLL 探针（只狼 / Sekiro）
// ----------------------------------------------------------------------------
// 目的：观察游戏播放了哪些声音，搞清楚每个声音来自哪个 .fsb bank，
//       并把确认过的单个音效 Channel 送到 DualSense / VB-CABLE。
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
// 当前推荐流程：先只录候选 SFX，不震动；试听 WAV 后在 GUI 手动勾选该震的 idx。
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
#include <cmath>
#include <cstdlib>
#include <share.h>   // _fsopen / _SH_DENYWR：日志用共享读打开，游戏运行时外部也能读

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
typedef FMOD_RESULT (*SoundGetParent_t)(void* sound, void** parent);
typedef FMOD_RESULT (*SoundGetNumSub_t)(void* sound, int* numsubsounds);
typedef FMOD_RESULT (*Sound_GetSubSound_t)(void* sound, int index, void** subsound);

static createSound_t              g_createSound  = nullptr;
static createStream_t             g_createStream = nullptr;
static createSoundInternal_t      g_createSoundInternal = nullptr;
static playSound_t                g_playSound    = nullptr;
static Sound_GetName_t            g_getName      = nullptr;
static SoundGetParent_t           g_getParent    = nullptr;
static SoundGetNumSub_t           g_getNumSub    = nullptr;
static Sound_GetSubSound_t       g_getSub       = nullptr;   // 真 C  FMOD_Sound_GetSubSound
static Sound_GetSubSound_t       g_getSubCpp    = nullptr;   // 真 C++ Sound::getSubSound

static HMODULE g_orig = nullptr;

// jmp 跳转桩的目标地址表：thunks.asm 里每个 thunk_i 执行 jmp [g_thunk_targets+i*8]。
// do_init() 用 GetProcAddress(g_orig, 序号) 把 360 个原函数地址填进来。
// （为什么不用 PE forwarder：Windows 的 forwarder 解析走受限搜索路径，不含应用目录，
//  转发到游戏目录里的 fmodex64_orig.dll 会失败——实测 err 127。jmp 桩绕开这个限制。）
#include "thunks_gen.h"
extern "C" void* g_thunk_targets[THUNK_COUNT] = { nullptr };

// Sound* -> 来源 bank 文件名（basename）
static std::unordered_map<void*, std::string> g_soundBank;
// 子声音指针 -> 它在 bank 里的 index（hook getSubSound 时记录，用于在 playSound 认出"是第几号音效"）
static std::unordered_map<void*, int> g_subIndex;
static std::unordered_map<void*, std::string> g_subBank;
static std::mutex g_lock;
static FILE* g_log = nullptr;

// 子声音 index → 含义（自测 + 研究资料，对本版 smain 系 bank 有效）
static const char* sound_meaning(int idx) {
    if (idx >= 665 && idx <= 700) return "弹刀/格挡/clash";   // 自测：681/682=弹刀，整簇为弹刀格挡变体
    if (idx == 408)               return "危攻蓄力";           // 实测确认
    if (idx >= 983 && idx <= 992) return "受伤/死亡";          // 实测确认
    if (idx >= 851 && idx <= 853) return "处决/破防duang";     // 自测：853每次处决刷一次(3处决→3,2处决→2)
    if (idx >= 256 && idx <= 258) return "布料/剧烈移动(闪避)"; // 自测:只在闪避/挥刀/战斗响,轻走路不响(走路0次,闪避10次)→闪避靠它
    if (idx == 428 || idx == 435 || idx == 438 || idx == 444 || idx == 456) return "UI/菜单音"; // 自测:标题/佛雕菜单导航
    if (idx == 353 || idx == 354) return "濒死心跳";          // 自测:濒死状态扑通两下反复
    if (idx >= 33  && idx <= 35)  return "归佛/传送";          // 自测:佛雕点传送(smain_jaj)
    if (idx >= 57  && idx <= 59)  return "死字咚/死亡屏幕";    // 自测:死亡时(smain_jaj)
    if (idx == 1031|| idx == 1032) return "地名咚/到达";       // 自测:到新地点
    if (idx >= 60  && idx <= 64)  return "脚步(通用)";         // 自测:所有地面每步都刷(太频繁,不收)
    if (idx >= 579 && idx <= 582) return "木质脚步";           // 自测:木板脚步(曾误认为闪避)
    if (idx >= 629 && idx <= 632) return "雪地脚步";           // 自测:雪地脚步
    if (idx == 330 || idx == 331 || idx == 641) return "不死斩挥刀"; // 自测：每挥一次刷一次
    if (idx >= 401 && idx <= 402) return "水月反击";           // 研究资料(待实测)
    return "?";
}

static const char* sound_group_key(int idx) {
    if (idx >= 665 && idx <= 700) return "deflect_guard";
    if (idx == 408)               return "danger_attack";
    if (idx >= 983 && idx <= 992) return "hurt_death";
    if (idx >= 851 && idx <= 853) return "deathblow_break";
    if (idx >= 256 && idx <= 258) return "dodge_cloth";
    if (idx == 330 || idx == 331 || idx == 641) return "mortal_draw";
    if (idx == 428 || idx == 435 || idx == 438 || idx == 444 || idx == 456) return "ui_menu";
    if (idx == 353 || idx == 354) return "low_hp_heartbeat";
    if (idx >= 33  && idx <= 35)  return "travel_buddha";
    if (idx >= 57  && idx <= 59)  return "death_screen";
    if (idx == 1031 || idx == 1032) return "area_title";
    if (idx >= 60  && idx <= 64)  return "footstep_common";
    if (idx >= 579 && idx <= 582) return "footstep_wood";
    if (idx >= 629 && idx <= 632) return "footstep_snow";
    if (idx >= 401 && idx <= 402) return "ashina_cross_counter";
    return nullptr;
}

// 内置白名单：保留这些含义映射给 GUI/日志参考。
// 当前推荐工作流是先只录不震，再由 GUI 手动勾选确认过的 idx。
static bool is_haptic_event(int idx) {
    if (idx < 0) return false;
    if (idx == 1131 || idx == 1132) return false; // 手感差，显式排除
    if (idx >= 665 && idx <= 700) return true;   // 弹刀/格挡
    if (idx == 408)               return true;   // 危攻
    if (idx >= 983 && idx <= 992) return true;   // 受伤/死亡
    if (idx >= 851 && idx <= 853) return true;   // 处决/破防 duang
    if (idx >= 256 && idx <= 258) return true;   // 布料/剧烈移动=闪避信号(轻走路不响,只在闪避/挥刀/战斗响)
    if (idx == 330 || idx == 331 || idx == 641) return true;  // 不死斩挥刀
    // 新增 UI/系统事件(均实测游戏内几乎不出现,误触发0-2次):
    if (idx == 428 || idx == 435 || idx == 438 || idx == 444 || idx == 456) return true; // 菜单/标题/选项 UI 音
    if (idx == 353 || idx == 354) return true;   // 濒死心跳(扑通两下=353+354一对反复)
    if (idx >= 33 && idx <= 35)   return true;   // 归佛/佛雕点传送 stinger
    if (idx >= 57 && idx <= 59)   return true;   // "死"字屏幕咚/死亡 stinger
    if (idx == 1031 || idx == 1032) return true; // 到新地点·地名"咚"/到达
    // 注意：故意不收 60-64(通用脚步,走哪震哪太乱)、579-582/629-632(地面脚步)、174/1305/1306(不死斩血焰循环音)、
    //       xm11.fsb idx=0(区域环境音,一直响)、smain_jaj idx=7(该bank也含游戏内声音,故按具体idx收而非整bank)
    return false;
}

// ----------------------------------------------------------------------------
static const char* basename_ascii(const char* path) {
    if (!path) return "";
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

// 按文件名分类：music = sm + 数字, voice = vm*, 其余 = SFX 候选
// （smain.fsb 不是音乐：sm 后面是 'a' 不是数字）
static const char* classify(const std::string& base) {
    auto starts = [&](const char* p) {
        return base.size() >= strlen(p) && _strnicmp(base.c_str(), p, strlen(p)) == 0;
    };
    if (starts("vm")) return "SKIP(voice)";
    if (starts("sm") && base.size() > 2 && isdigit((unsigned char)base[2])) return "SKIP(music)";
    return "VIBRATE";
}

static bool starts_with_ci(const std::string& s, const char* prefix) {
    return s.size() >= strlen(prefix) && _strnicmp(s.c_str(), prefix, strlen(prefix)) == 0;
}

static bool is_default_haptic_bank(const std::string& bank) {
    return starts_with_ci(bank, "smain") || starts_with_ci(bank, "main");
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

struct EffectRule {
    bool hasEnabled = false;
    bool enabled = false;
    bool hasGain = false;
    float gain = 1.0f;
    bool hasDump = false;
    bool dump = false;
};

struct HapticConfig {
    bool enabled = true;
    bool dumpEnabled = true;
    bool useBuiltinDefaults = false;
    float defaultGain = 1.0f;
    std::unordered_map<int, EffectRule> effects;
};

struct HapticDecision {
    bool enabled = false;
    bool dump = false;
    bool attach = false;
    float gain = 1.0f;
};

static std::once_flag g_appPathOnce;
static char g_appDir[MAX_PATH] = "";
static char g_cfgPath[MAX_PATH] = "";
static char g_seenPath[MAX_PATH] = "";
static char g_dumpDir[MAX_PATH] = "";
static std::mutex g_cfgLock;
static HapticConfig g_cfg;
static std::once_flag g_cfgOnce;
static std::mutex g_seenLock;
static std::unordered_set<std::string> g_seenKeys;

static void ensure_app_paths() {
    std::call_once(g_appPathOnce, []() {
        char* app = nullptr; size_t n = 0;
        if (_dupenv_s(&app, &n, "APPDATA") == 0 && app) {
            _snprintf_s(g_appDir, sizeof(g_appDir), _TRUNCATE, "%s\\DualSenseSfxHaptics", app);
            free(app);
        } else {
            strncpy_s(g_appDir, sizeof(g_appDir), ".\\DualSenseSfxHaptics", _TRUNCATE);
        }
        CreateDirectoryA(g_appDir, nullptr);
        _snprintf_s(g_cfgPath, sizeof(g_cfgPath), _TRUNCATE, "%s\\haptics.json", g_appDir);
        _snprintf_s(g_seenPath, sizeof(g_seenPath), _TRUNCATE, "%s\\seen_effects.jsonl", g_appDir);
        _snprintf_s(g_dumpDir, sizeof(g_dumpDir), _TRUNCATE, "%s\\dumps", g_appDir);
        CreateDirectoryA(g_dumpDir, nullptr);
    });
}

static std::string read_all_text(const char* path) {
    FILE* f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f) return std::string();
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return std::string(); }
    std::string s;
    s.resize((size_t)len);
    fread(&s[0], 1, (size_t)len, f);
    fclose(f);
    return s;
}

static void write_default_config_if_missing() {
    ensure_app_paths();
    DWORD attrs = GetFileAttributesA(g_cfgPath);
    if (attrs != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = nullptr;
    fopen_s(&f, g_cfgPath, "wb");
    if (!f) return;
    const char* text =
        "{\n"
        "  \"enabled\": true,\n"
        "  \"defaultGain\": 1.0,\n"
        "  \"dumpEnabled\": false,\n"
        "  \"useBuiltinDefaults\": true,\n"
        "  \"effects\": {}\n"
        "}\n";
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static const char* skip_ws(const char* p, const char* end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}

static bool find_json_bool(const std::string& text, const char* key, bool* out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    const char* begin = text.c_str();
    const char* p = skip_ws(begin + pos + 1, begin + text.size());
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static bool find_json_float(const std::string& text, const char* key, float* out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    const char* begin = text.c_str();
    const char* p = skip_ws(begin + pos + 1, begin + text.size());
    char* endp = nullptr;
    float v = strtof(p, &endp);
    if (endp == p) return false;
    *out = v;
    return true;
}

static float clamp_gain(float gain) {
    if (gain < 0.0f) return 0.0f;
    if (gain > 8.0f) return 8.0f;
    return gain;
}

static HapticConfig parse_config(const std::string& text) {
    HapticConfig cfg;
    size_t effectsPos = text.find("\"effects\"");
    std::string root = effectsPos == std::string::npos ? text : text.substr(0, effectsPos);
    bool b = false; float f = 0;
    if (find_json_bool(root, "enabled", &b)) cfg.enabled = b;
    if (find_json_bool(root, "dumpEnabled", &b)) cfg.dumpEnabled = b;
    if (find_json_bool(root, "useBuiltinDefaults", &b)) cfg.useBuiltinDefaults = b;
    if (find_json_float(root, "defaultGain", &f)) cfg.defaultGain = clamp_gain(f);

    if (effectsPos == std::string::npos) return cfg;
    size_t open = text.find('{', effectsPos);
    if (open == std::string::npos) return cfg;
    int depth = 0;
    size_t close = std::string::npos;
    for (size_t i = open; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}' && --depth == 0) { close = i; break; }
    }
    if (close == std::string::npos) return cfg;
    size_t pos = open + 1;
    while (pos < close) {
        size_t q1 = text.find('"', pos);
        if (q1 == std::string::npos || q1 >= close) break;
        size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= close) break;
        std::string key = text.substr(q1 + 1, q2 - q1 - 1);
        char* endp = nullptr;
        int idx = (int)strtol(key.c_str(), &endp, 10);
        size_t objOpen = text.find('{', q2);
        if (!endp || *endp != 0 || objOpen == std::string::npos || objOpen >= close) { pos = q2 + 1; continue; }
        int objDepth = 0;
        size_t objClose = std::string::npos;
        for (size_t i = objOpen; i < close; ++i) {
            if (text[i] == '{') ++objDepth;
            else if (text[i] == '}' && --objDepth == 0) { objClose = i; break; }
        }
        if (objClose == std::string::npos) break;
        std::string body = text.substr(objOpen, objClose - objOpen + 1);
        EffectRule rule;
        if (find_json_bool(body, "enabled", &b)) { rule.hasEnabled = true; rule.enabled = b; }
        if (find_json_float(body, "gain", &f)) { rule.hasGain = true; rule.gain = clamp_gain(f); }
        if (find_json_bool(body, "dump", &b)) { rule.hasDump = true; rule.dump = b; }
        cfg.effects[idx] = rule;
        pos = objClose + 1;
    }
    return cfg;
}

static void load_config_once() {
    std::call_once(g_cfgOnce, []() {
        ensure_app_paths();
        write_default_config_if_missing();
        std::string text = read_all_text(g_cfgPath);
        HapticConfig cfg = parse_config(text);
        {
            std::lock_guard<std::mutex> lock(g_cfgLock);
            g_cfg = cfg;
        }
        logf("CFG: loaded %s enabled=%d defaultGain=%.2f dump=%d effects=%zu",
             g_cfgPath, cfg.enabled ? 1 : 0, cfg.defaultGain, cfg.dumpEnabled ? 1 : 0, cfg.effects.size());
    });
}

static HapticDecision decide_haptic(int idx, const std::string& bank, bool bankAllowsHaptics) {
    load_config_once();
    HapticDecision d;
    if (!bankAllowsHaptics) return d;
    std::lock_guard<std::mutex> lock(g_cfgLock);
    if (!g_cfg.enabled) return d;
    bool enabled = g_cfg.useBuiltinDefaults && (is_default_haptic_bank(bank) || (bank == "(unknown)" && is_haptic_event(idx)));
    float gain = g_cfg.defaultGain;
    bool dump = g_cfg.dumpEnabled;
    auto it = idx >= 0 ? g_cfg.effects.find(idx) : g_cfg.effects.end();
    if (it != g_cfg.effects.end()) {
        const EffectRule& rule = it->second;
        if (rule.hasEnabled) enabled = rule.enabled;
        if (rule.hasGain) gain = rule.gain;
        if (rule.hasDump && rule.dump) dump = true;
    }
    d.enabled = enabled;
    d.gain = clamp_gain(gain);
    d.dump = dump;
    d.attach = enabled || dump;
    return d;
}

static void json_escape(FILE* f, const char* s) {
    if (!s) return;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\r') fputs("\\r", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p >= 32) fputc(*p, f);
    }
}

static void record_seen_effect(int idx, const std::string& bank, const char* subname, const char* verdict, bool haptic, float gain) {
    ensure_app_paths();
    char key[512];
    _snprintf_s(key, sizeof(key), _TRUNCATE, "%d|%s", idx, bank.c_str());
    std::lock_guard<std::mutex> lock(g_seenLock);
    if (g_seenKeys.find(key) != g_seenKeys.end()) return;
    g_seenKeys.insert(key);
    FILE* f = nullptr;
    fopen_s(&f, g_seenPath, "ab");
    if (!f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(f, "{\"time\":\"%04u-%02u-%02u %02u:%02u:%02u\",\"idx\":%d,\"bank\":\"",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, idx);
    json_escape(f, bank.c_str());
    fputs("\",\"sub\":\"", f);
    json_escape(f, subname);
    fputs("\",\"verdict\":\"", f);
    json_escape(f, verdict);
    fputs("\",\"meaning\":\"", f);
    json_escape(f, sound_meaning(idx));
    fprintf(f, "\",\"haptic\":%s,\"gain\":%.3f}\n", haptic ? "true" : "false", gain);
    fclose(f);
}

// 查某个 Sound*（可能是子音）属于哪个 bank
static std::string lookup_bank(void* sound) {
    if (!sound) return "(null)";
    {
        std::lock_guard<std::mutex> g(g_lock);
        auto it = g_soundBank.find(sound);
        if (it != g_soundBank.end()) return it->second;
        auto sub = g_subBank.find(sound);
        if (sub != g_subBank.end()) return sub->second;
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
typedef FMOD_RESULT (*FMOD_DSP_RELEASECB)(FMOD_DSP_STATE_*);
typedef struct FMOD_DSP_DESCRIPTION_ {
    char name[32]; unsigned int version; int channels;
    void* create; FMOD_DSP_RELEASECB release; void* reset;
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
typedef FMOD_RESULT (*ChannelAddDSP_t)(void*, void*, void**);
typedef FMOD_RESULT (*ChannelSetPaused_t)(void*, int);
typedef FMOD_RESULT (*DSPRelease_t)(void*);
typedef FMOD_RESULT (*ChannelSetPan_t)(void*, float);
typedef FMOD_RESULT (*ChannelSetSpeakerMix_t)(void*, float, float, float, float, float, float, float, float);
struct FMOD_VECTOR_ { float x, y, z; };
typedef FMOD_RESULT (*ChannelSet3DAttributes_t)(void*, const FMOD_VECTOR_*, const FMOD_VECTOR_*);
static CreateCG_t  g_createCG  = nullptr;
static CreateDSP_t g_createDSP = nullptr;
static AddDSP_t    g_addDSP    = nullptr;
static SetCG_t     g_setCG     = nullptr;
static GetMaster_t g_getMaster = nullptr;
static ChannelAddDSP_t   g_chAddDSP    = nullptr;
static ChannelSetPaused_t g_chSetPaused = nullptr;
static DSPRelease_t      g_dspRelease  = nullptr;
static ChannelSetPan_t       g_chSetPan = nullptr;
static ChannelSetSpeakerMix_t g_chSetSpeakerMix = nullptr;
static ChannelSet3DAttributes_t g_chSet3DAttributes = nullptr;

// 声道组枚举（诊断：看游戏 event 系统的总线结构）
typedef FMOD_RESULT (*CG_NumGroups_t)(void*, int*);
typedef FMOD_RESULT (*CG_GetGroup_t)(void*, int, void**);
typedef FMOD_RESULT (*CG_GetName_t)(void*, char*, int);
typedef FMOD_RESULT (*CG_NumChans_t)(void*, int*);
static CG_NumGroups_t g_cgNumGroups = nullptr;
static CG_GetGroup_t  g_cgGetGroup  = nullptr;
static CG_GetName_t   g_cgGetName   = nullptr;
static CG_NumChans_t  g_cgNumChans  = nullptr;
static void enum_groups(void* grp, int depth) {
    if (!grp || depth > 5) return;
    char name[256] = ""; if (g_cgGetName) g_cgGetName(grp, name, sizeof(name));
    int nch = 0; if (g_cgNumChans) g_cgNumChans(grp, &nch);
    int ng = 0; if (g_cgNumGroups) g_cgNumGroups(grp, &ng);
    logf("GROUP d=%d \"%s\" channels=%d subgroups=%d", depth, name, nch, ng);
    for (int i = 0; i < ng; ++i) { void* sub = nullptr; if (g_cgGetGroup && g_cgGetGroup(grp, i, &sub) == 0 && sub) enum_groups(sub, depth + 1); }
}

// 触觉混音环形缓冲：每个 Channel DSP 按同一条输出时间轴叠加写入，
// WASAPI 线程按时间顺序拉取并清空。这样多个音效同时触发时会混合，而不是排队串起来。
static const unsigned RING = 16384, RMASK = RING - 1;
static const unsigned MIX_LATENCY_FRAMES = 512; // ~10.7ms@48k，给多个 DSP 回调一个对齐窗口
static float g_ringL[RING], g_ringR[RING];             // 已确认音效的直接混音
static float g_ringGatedL[RING], g_ringGatedR[RING];   // master fallback 的门控混音
static std::atomic<unsigned> g_wr{0}, g_rd{0};
static std::mutex g_ringLock;

// 门控：音效播放时打开(=1)，之后逐采样衰减。haptic 输出 = master音频 × 门。
static std::atomic<float> g_gate{0.0f};
static const float GATE_DK = 0.99975f;   // 每采样衰减（≈200ms 尾巴）

struct SpatialState {
    float pan = 0.0f;
    float left = 1.0f;
    float right = 1.0f;
    float distanceGain = 1.0f;
    bool hasSpeaker = false;
    bool has3d = false;
};

static std::mutex g_spatialLock;
static std::unordered_map<void*, SpatialState> g_channelSpatial;

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static SpatialState default_spatial() { return SpatialState{}; }

static void update_pan_spatial(void* channel, float pan) {
    if (!channel) return;
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    std::lock_guard<std::mutex> lock(g_spatialLock);
    SpatialState& s = g_channelSpatial[channel];
    s.pan = pan;
    if (!s.hasSpeaker && !s.has3d) {
        s.left = sqrtf(clamp01((1.0f - pan) * 0.5f));
        s.right = sqrtf(clamp01((1.0f + pan) * 0.5f));
    }
}

static void update_speaker_spatial(void* channel, float fl, float fr, float center, float lfe, float bl, float br, float sl, float sr) {
    if (!channel) return;
    float left = fabsf(fl) + fabsf(bl) * 0.7f + fabsf(sl) * 0.8f + fabsf(center) * 0.35f + fabsf(lfe) * 0.15f;
    float right = fabsf(fr) + fabsf(br) * 0.7f + fabsf(sr) * 0.8f + fabsf(center) * 0.35f + fabsf(lfe) * 0.15f;
    float maxv = left > right ? left : right;
    if (maxv < 0.0001f) { left = right = 1.0f; maxv = 1.0f; }
    std::lock_guard<std::mutex> lock(g_spatialLock);
    SpatialState& s = g_channelSpatial[channel];
    s.left = clamp01(left / maxv);
    s.right = clamp01(right / maxv);
    s.pan = (right - left) / (right + left + 0.0001f);
    s.hasSpeaker = true;
}

static void update_3d_spatial(void* channel, const FMOD_VECTOR_* pos) {
    if (!channel || !pos) return;
    float pan = pos->x / (fabsf(pos->z) + fabsf(pos->x) + 0.001f);
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    float dist = sqrtf(pos->x * pos->x + pos->y * pos->y + pos->z * pos->z);
    float distanceGain = 1.0f / (1.0f + dist * 0.025f);
    if (distanceGain < 0.25f) distanceGain = 0.25f;
    std::lock_guard<std::mutex> lock(g_spatialLock);
    SpatialState& s = g_channelSpatial[channel];
    s.pan = pan;
    s.left = sqrtf(clamp01((1.0f - pan) * 0.5f));
    s.right = sqrtf(clamp01((1.0f + pan) * 0.5f));
    s.distanceGain = distanceGain;
    s.has3d = true;
}

static SpatialState get_spatial(void* channel) {
    std::lock_guard<std::mutex> lock(g_spatialLock);
    auto it = g_channelSpatial.find(channel);
    return it != g_channelSpatial.end() ? it->second : default_spatial();
}

static inline void clear_mix_slot(unsigned pos) {
    unsigned p = pos & RMASK;
    g_ringL[p] = g_ringR[p] = 0.0f;
    g_ringGatedL[p] = g_ringGatedR[p] = 0.0f;
}

static void clear_mix_range(unsigned begin, unsigned end) {
    for (unsigned p = begin; p < end; ++p) clear_mix_slot(p);
}

static float soft_limit(float x) {
    float ax = x < 0.0f ? -x : x;
    if (ax <= 0.80f) return x;
    float y = 0.80f + (ax - 0.80f) / (1.0f + 4.0f * (ax - 0.80f));
    if (y > 1.0f) y = 1.0f;
    return x < 0.0f ? -y : y;
}

static float push_audio_to_ring(const float* in, unsigned int length, int inch, bool direct, float gain = 1.0f,
                                unsigned* sourceCursor = nullptr, bool* sourceStarted = nullptr,
                                float leftGain = 1.0f, float rightGain = 1.0f) {
    if (!in || length == 0 || inch <= 0 || inch > 32) return 0.0f;
    gain = clamp_gain(gain);
    std::lock_guard<std::mutex> lock(g_ringLock);
    static unsigned fallbackCursor = 0;
    static bool fallbackStarted = false;
    if (!sourceCursor) sourceCursor = &fallbackCursor;
    if (!sourceStarted) sourceStarted = &fallbackStarted;

    unsigned frames = length;
    if (frames > RING / 2) frames = RING / 2;
    unsigned wr = g_wr.load(std::memory_order_relaxed);
    unsigned rd = g_rd.load(std::memory_order_relaxed);
    unsigned anchor = rd + MIX_LATENCY_FRAMES;
    if (!*sourceStarted || *sourceCursor < anchor) {
        *sourceCursor = anchor;
        *sourceStarted = true;
    }
    unsigned maxStart = rd + RING - frames - 1;
    if (*sourceCursor > maxStart) *sourceCursor = maxStart;

    unsigned start = *sourceCursor;
    unsigned end = start + frames;
    if (end > wr) clear_mix_range(wr, end);

    float* dstL = direct ? g_ringL : g_ringGatedL;
    float* dstR = direct ? g_ringR : g_ringGatedR;
    float peak = 0;
    for (unsigned int i = 0; i < frames; ++i) {
        float s = 0;
        if (inch >= 2) {
            float l = in[i * inch];
            float r = in[i * inch + 1];
            float center = (l + r) * 0.5f;
            dstL[(start + i) & RMASK] += center * gain * leftGain;
            dstR[(start + i) & RMASK] += center * gain * rightGain;
            float a = fabsf(center * gain);
            if (a > peak) peak = a;
            continue;
        }
        for (int c = 0; c < inch; ++c) s += in[i * inch + c];
        s *= gain;
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        dstL[(start + i) & RMASK] += s * leftGain;
        dstR[(start + i) & RMASK] += s * rightGain;
        float a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    *sourceCursor = end;
    if (end > wr) g_wr.store(end, std::memory_order_release);
    return peak;
}

static float push_mono_to_ring(const float* mono, unsigned int length, bool direct, float gain,
                               unsigned* sourceCursor, bool* sourceStarted,
                               float leftGain = 1.0f, float rightGain = 1.0f) {
    if (!mono || length == 0) return 0.0f;
    return push_audio_to_ring(mono, length, 1, direct, gain, sourceCursor, sourceStarted, leftGain, rightGain);
}

// DSP 回调：透传 + 降混单声道入环。务必防 in/out 为空、inch 异常。
static std::atomic<int> g_dspLogged{0};
static FMOD_RESULT dsp_read(FMOD_DSP_STATE_*, float* in, float* out, unsigned int length, int inch, int outch) {
    if (g_dspLogged.exchange(1) == 0)
        logf("DSP first read: in=%p out=%p length=%u inch=%d outch=%d", in, out, length, inch, outch);
    if (inch <= 0 || inch > 32) return 0;
    int ch = (outch > 0 && outch < inch) ? outch : inch;   // 透传的声道数
    if (out && in) memcpy(out, in, (size_t)length * ch * sizeof(float));
    if (!in) return 0;                                      // 无输入时不入环
    float dpeak = push_audio_to_ring(in, length, inch, false);
    // 诊断：每约 200 次回调（~4s）报一次本批峰值
    static std::atomic<int> dc{0};
    static float dmax = 0; if (dpeak > dmax) dmax = dpeak;
    if (dc.fetch_add(1) % 200 == 199) { logf("DSP peak(last4s)=%.4f", dmax); dmax = 0; }
    return 0;  // FMOD_OK
}

// 由 haptic_out 渲染线程调用：从环形缓冲连续读 n 个单声道样本
extern "C" int haptic_pull_audio(float* outL, float* outR, int n) {
    float gate = g_gate.load(std::memory_order_relaxed);
    int got = 0; float peak = 0;
    unsigned avail = 0;
    {
        std::lock_guard<std::mutex> lock(g_ringLock);
        unsigned wr = g_wr.load(std::memory_order_acquire);
        unsigned rd = g_rd.load(std::memory_order_relaxed);
        avail = wr - rd;
        // 只保留 ~3 个 WASAPI 帧的 backlog：把延迟从 ~170ms(RING/2) 砍到 ~20-30ms，
        // 并让"门"对齐到实时音频(否则门乘的是170ms前的旧音频、弹刀真音到时门已衰减)。
        // 太小会欠载爆音/断音；触觉对 glitch 较宽容，可从此值(3)往下调。
        unsigned target = 3u * (unsigned)n;
        if (avail > target) {
            unsigned newRd = wr - target;
            clear_mix_range(rd, newRd);
            rd = newRd;
        }
        for (int i = 0; i < n; ++i) {
            float vl = 0, vr = 0;
            if (rd != wr) {
                unsigned pos = rd & RMASK;
                vl = g_ringL[pos] + g_ringGatedL[pos] * gate;
                vr = g_ringR[pos] + g_ringGatedR[pos] * gate;
                clear_mix_slot(rd);
                ++rd;
                got = 1;
            }
            gate *= GATE_DK;
            if (gate < 0.0001f) gate = 0.0f;
            vl = soft_limit(vl);
            vr = soft_limit(vr);
            outL[i] = vl;
            outR[i] = vr;
            float a = fabsf(vl); if (fabsf(vr) > a) a = fabsf(vr); if (a > peak) peak = a;
        }
        g_rd.store(rd, std::memory_order_relaxed);
    }
    g_gate.store(gate, std::memory_order_relaxed);
    // 诊断：每约 300 次（~3s）报一次消费端
    static std::atomic<int> pc{0}; static float pmax = 0; static unsigned amax = 0;
    if (peak > pmax) pmax = peak; if (avail > amax) amax = avail;
    // PULL 刷屏日志已关闭(每帧写会独占锁死文件+刷爆日志)。需要诊断时再开。
    (void)pc; (void)pmax; (void)amax;
    return (got && peak >= 0.004f) ? 1 : 0;
}

static std::atomic<int> g_chDspLogged{0};
static const int DUMP_MAX_FRAMES = 48000 * 3;
static std::mutex g_dumpLock;

struct DspContext {
    void* channel;
    int idx;
    float gain;
    bool dump;
    bool haptic;
    unsigned mixCursor;
    bool mixStarted;
    unsigned hapticFrames;
    unsigned quietFrames;
    bool hapticDone;
    float hpPrevIn;
    float hpPrevOut;
    float env;
};

static std::unordered_map<void*, DspContext> g_dspContext;

static const unsigned HAPTIC_FALLBACK_MAX_FRAMES = 48000 / 4; // 250ms：未知事件默认短促
static const unsigned HAPTIC_QUIET_FRAMES = 48000 / 40;       // 25ms 低电平后停止
static const float HAPTIC_QUIET_PEAK = 0.010f;
static const float HAPTIC_HP_A = 0.985f;       // 约 115Hz 高通，削掉持续嗡嗡的低频拖尾
static const float HAPTIC_ENV_ATTACK = 0.45f;  // 快速抓瞬态
static const float HAPTIC_ENV_DECAY = 0.965f;  // 快速回落，避免傻震
static const float HAPTIC_TRANSIENT_GAIN = 1.65f;

static unsigned haptic_max_frames_for_idx(int idx) {
    if (idx >= 665 && idx <= 700) return 48000 / 8;   // 125ms 弹刀/格挡：短促冲击
    if (idx == 408)               return 48000 / 8;   // 125ms 危攻提示
    if (idx >= 851 && idx <= 853) return 48000 / 5;   // 200ms 处决/破防
    if (idx >= 983 && idx <= 992) return 48000 / 5;   // 200ms 受伤/死亡
    if (idx >= 256 && idx <= 258) return 48000 / 10;  // 100ms 闪避/布料
    if (idx == 330 || idx == 331 || idx == 641) return 48000 / 5;
    return HAPTIC_FALLBACK_MAX_FRAMES;
}

static float shape_haptic_sample(float mono, DspContext& ctx) {
    float hp = HAPTIC_HP_A * (ctx.hpPrevOut + mono - ctx.hpPrevIn);
    ctx.hpPrevIn = mono;
    ctx.hpPrevOut = hp;
    float absHp = hp < 0.0f ? -hp : hp;
    if (absHp > ctx.env) ctx.env += (absHp - ctx.env) * HAPTIC_ENV_ATTACK;
    else ctx.env *= HAPTIC_ENV_DECAY;
    float shaped = hp * HAPTIC_TRANSIENT_GAIN + ctx.env * 0.35f;
    if (shaped > 1.0f) shaped = 1.0f;
    if (shaped < -1.0f) shaped = -1.0f;
    return shaped;
}

struct ChannelDump {
    int idx;
    int channels;
    int frames;
    FILE* file;
    char path[MAX_PATH];
};

static void wav_write_header(FILE* f, int channels, int frames) {
    if (!f) return;
    unsigned sampleRate = 48000;
    unsigned bits = 16;
    unsigned dataBytes = (unsigned)frames * (unsigned)channels * (bits / 8);
    unsigned riffBytes = 36 + dataBytes;
    unsigned byteRate = sampleRate * (unsigned)channels * (bits / 8);
    unsigned short blockAlign = (unsigned short)(channels * (bits / 8));
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riffBytes, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); unsigned fmtSize = 16; fwrite(&fmtSize, 4, 1, f);
    unsigned short audioFormat = 1; fwrite(&audioFormat, 2, 1, f);
    unsigned short ch = (unsigned short)channels; fwrite(&ch, 2, 1, f);
    fwrite(&sampleRate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
}

static ChannelDump* create_channel_dump(int idx, int channels) {
    ChannelDump* dump = new ChannelDump{};
    dump->idx = idx;
    dump->channels = (channels > 0 && channels <= 8) ? channels : 2;
    ensure_app_paths();
    CreateDirectoryA(g_dumpDir, nullptr);
    const char* key = sound_group_key(idx);
    char file[96] = "";
    if (key && key[0]) _snprintf_s(file, sizeof(file), _TRUNCATE, "%s.wav", key);
    else _snprintf_s(file, sizeof(file), _TRUNCATE, "idx%d.wav", idx);
    _snprintf_s(dump->path, sizeof(dump->path), _TRUNCATE, "%s\\%s", g_dumpDir, file);
    fopen_s(&dump->file, dump->path, "wb");
    if (!dump->file) { delete dump; return nullptr; }
    wav_write_header(dump->file, dump->channels, 0);
    logf("CHDUMP: open %s ch=%d idx=%d group=%s", dump->path, dump->channels, idx, key ? key : "idx");
    return dump;
}

static void write_channel_dump(ChannelDump* dump, const float* in, unsigned int length, int inch) {
    if (!dump || !dump->file || !in || length == 0 || inch <= 0) return;
    int remaining = DUMP_MAX_FRAMES - dump->frames;
    if (remaining <= 0) return;
    unsigned int frames = length < (unsigned int)remaining ? length : (unsigned int)remaining;
    for (unsigned int i = 0; i < frames; ++i) {
        for (int c = 0; c < dump->channels; ++c) {
            float s = in[i * inch + (c < inch ? c : inch - 1)];
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            short v = (short)lrintf(s * 32767.0f);
            fwrite(&v, sizeof(v), 1, dump->file);
        }
    }
    dump->frames += (int)frames;
    long endPos = ftell(dump->file);
    wav_write_header(dump->file, dump->channels, dump->frames);
    fseek(dump->file, endPos, SEEK_SET);
    fflush(dump->file);
}

static void close_channel_dump(ChannelDump* dump) {
    if (!dump) return;
    if (dump->file) {
        wav_write_header(dump->file, dump->channels, dump->frames);
        fclose(dump->file);
        logf("CHDUMP: close %s frames=%d idx=%d", dump->path, dump->frames, dump->idx);
    }
    delete dump;
}

static FMOD_RESULT channel_tap_release(FMOD_DSP_STATE_* state) {
    void* dsp = state ? state->instance : nullptr;
    logf("CHDSP: release dsp=%p", dsp);
    if (state && state->plugindata) {
        close_channel_dump((ChannelDump*)state->plugindata);
        state->plugindata = nullptr;
    }
    if (dsp) {
        std::lock_guard<std::mutex> lock(g_dumpLock);
        auto it = g_dspContext.find(dsp);
        void* channel = it != g_dspContext.end() ? it->second.channel : nullptr;
        g_dspContext.erase(dsp);
        if (channel) {
            std::lock_guard<std::mutex> slock(g_spatialLock);
            g_channelSpatial.erase(channel);
        }
    }
    return 0;
}

static FMOD_RESULT channel_tap_read(FMOD_DSP_STATE_* state, float* in, float* out, unsigned int length, int inch, int outch) {
    if (g_chDspLogged.exchange(1) == 0)
        logf("CHDSP first read: in=%p out=%p length=%u inch=%d outch=%d", in, out, length, inch, outch);
    if (inch <= 0 || inch > 32) return 0;
    int ch = (outch > 0 && outch < inch) ? outch : inch;
    if (out && in) memcpy(out, in, (size_t)length * ch * sizeof(float));
    if (!in) return 0;
    DspContext ctx{ nullptr, -1, 1.0f, false, false, 0, false, 0, 0, false, 0.0f, 0.0f, 0.0f };
    bool haveCtx = false;
    if (state) {
        std::lock_guard<std::mutex> lock(g_dumpLock);
        auto it = g_dspContext.find(state->instance);
        if (it != g_dspContext.end()) { ctx = it->second; haveCtx = true; }
    }
    if (state && !state->plugindata) {
        state->plugindata = ctx.dump ? create_channel_dump(ctx.idx, ch) : nullptr;
    }
    if (state && state->plugindata) write_channel_dump((ChannelDump*)state->plugindata, in, length, inch);
    float peak = 0.0f;
    if (ctx.haptic && !ctx.hapticDone) {
        unsigned maxFrames = haptic_max_frames_for_idx(ctx.idx);
        unsigned remaining = ctx.hapticFrames < maxFrames ? maxFrames - ctx.hapticFrames : 0;
        unsigned frames = length < remaining ? length : remaining;
        float blockPeak = 0.0f;
        if (frames > 0) {
            float energyL = 0.0f, energyR = 0.0f;
            for (unsigned int i = 0; i < frames; ++i) {
                float s = 0.0f;
                if (inch >= 2) {
                    energyL += fabsf(in[i * inch]);
                    energyR += fabsf(in[i * inch + 1]);
                }
                for (int c = 0; c < inch; ++c) s += in[i * inch + c];
                if (inch > 1) s /= inch;
                float a = s < 0.0f ? -s : s;
                if (a > blockPeak) blockPeak = a;
            }
            SpatialState spatial = get_spatial(ctx.channel);
            float audioLeft = 1.0f, audioRight = 1.0f;
            if (inch >= 2 && (energyL + energyR) > 0.0001f) {
                float maxEnergy = energyL > energyR ? energyL : energyR;
                audioLeft = sqrtf(clamp01(energyL / maxEnergy));
                audioRight = sqrtf(clamp01(energyR / maxEnergy));
            }
            float leftWeight = (0.55f * audioLeft + 0.45f * spatial.left) * spatial.distanceGain;
            float rightWeight = (0.55f * audioRight + 0.45f * spatial.right) * spatial.distanceGain;
            float maxWeight = leftWeight > rightWeight ? leftWeight : rightWeight;
            if (maxWeight > 1.0f) { leftWeight /= maxWeight; rightWeight /= maxWeight; }
            static thread_local float shaped[4096];
            unsigned offset = 0;
            for (unsigned int i = 0; i < frames; ++i) {
                float s = 0.0f;
                for (int c = 0; c < inch; ++c) s += in[i * inch + c];
                if (inch > 1) s /= inch;
                shaped[offset++] = shape_haptic_sample(s, ctx);
                if (offset == (unsigned)(sizeof(shaped) / sizeof(shaped[0]))) {
                    float p = push_mono_to_ring(shaped, offset, true, ctx.gain, &ctx.mixCursor, &ctx.mixStarted, leftWeight, rightWeight);
                    if (p > peak) peak = p;
                    offset = 0;
                }
            }
            if (offset > 0) {
                float p = push_mono_to_ring(shaped, offset, true, ctx.gain, &ctx.mixCursor, &ctx.mixStarted, leftWeight, rightWeight);
                if (p > peak) peak = p;
            }
            ctx.hapticFrames += frames;
            if (blockPeak < HAPTIC_QUIET_PEAK) ctx.quietFrames += frames;
            else ctx.quietFrames = 0;
        }
        if (remaining == 0 || frames < length || ctx.quietFrames >= HAPTIC_QUIET_FRAMES) {
            ctx.hapticDone = true;
            ctx.haptic = false;
            logf("CHDSP: haptic stop idx=%d frames=%u quiet=%u peak=%.4f", ctx.idx, ctx.hapticFrames, ctx.quietFrames, blockPeak);
        }
        if (state && haveCtx) {
            std::lock_guard<std::mutex> lock(g_dumpLock);
            auto it = g_dspContext.find(state->instance);
            if (it != g_dspContext.end()) {
                it->second.mixCursor = ctx.mixCursor;
                it->second.mixStarted = ctx.mixStarted;
                it->second.hapticFrames = ctx.hapticFrames;
                it->second.quietFrames = ctx.quietFrames;
                it->second.hapticDone = ctx.hapticDone;
                it->second.haptic = ctx.haptic;
                it->second.hpPrevIn = ctx.hpPrevIn;
                it->second.hpPrevOut = ctx.hpPrevOut;
                it->second.env = ctx.env;
            }
        }
    }
    static std::atomic<int> pc{0}; static float pmax = 0;
    if (peak > pmax) pmax = peak;
    if (pc.fetch_add(1) % 400 == 399) { logf("CHDSP peak(last)=%.4f", pmax); pmax = 0; }
    return 0;
}

static bool attach_channel_tap(void* system, void* channel, int subIdx, float gain, bool dump, bool haptic) {
    if (!system || !channel || !g_createDSP || !g_chAddDSP) return false;
    FMOD_DSP_DESCRIPTION_ d{};
    d.name[0] = 'c'; d.name[1] = 'h'; d.name[2] = 't'; d.name[3] = 'a'; d.name[4] = 'p'; d.name[5] = 0;
    d.version = 1; d.channels = 0; d.read = channel_tap_read; d.release = channel_tap_release;
    void* dsp = nullptr;
    FMOD_RESULT cr = g_createDSP(system, &d, &dsp);
    if (cr != 0 || !dsp) { logf("CHDSP: CreateDSP failed r=%d channel=%p idx=%d", cr, channel, subIdx); return false; }
    {
        std::lock_guard<std::mutex> lock(g_dumpLock);
        g_dspContext[dsp] = DspContext{ channel, subIdx, gain, dump, haptic, 0, false, 0, 0, false, 0.0f, 0.0f, 0.0f };
    }
    void* conn = nullptr;
    FMOD_RESULT ar = g_chAddDSP(channel, dsp, &conn);
    if (ar != 0) {
        logf("CHDSP: Channel_AddDSP failed r=%d channel=%p dsp=%p idx=%d", ar, channel, dsp, subIdx);
        {
            std::lock_guard<std::mutex> lock(g_dumpLock);
            g_dspContext.erase(dsp);
        }
        if (g_dspRelease) g_dspRelease(dsp);
        return false;
    }
    logf("CHDSP: attached channel=%p dsp=%p conn=%p idx=%d gain=%.2f dump=%d haptic=%d (%s)", channel, dsp, conn, subIdx, gain, dump ? 1 : 0, haptic ? 1 : 0, sound_meaning(subIdx));
    return true;
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
    void* master = nullptr; void* conn = nullptr;
    if (g_getMaster && g_getMaster(g_capSystem, &master) == 0 && master) {
        logf("=== 声道组结构枚举(诊断) ===");
        enum_groups(master, 0);                 // 只读：打印 event 系统的总线树
        logf("=== 枚举结束 ===");
        g_addDSP(master, dsp, &conn);           // 仍挂 master(临时,让本局有触觉)
        logf("CAP: DSP 挂到 MASTER 主组");
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

// 拦截 getSubSound（C++ 与 C 两个入口）：只读记录 子声音指针 → index，
// 这样 playSound(子声音) 时能反查出"这是 bank 里第几号"，从而识别弹刀/受击/脚步等。
extern "C" FMOD_RESULT getSubSoundCpp_detour(void* self, int index, void** subsound) {
    ensure_init();
    FMOD_RESULT r = g_getSubCpp ? g_getSubCpp(self, index, subsound) : -1;
    if (r == 0 && subsound && *subsound) {
        std::lock_guard<std::mutex> g(g_lock);
        g_subIndex[*subsound] = index;
        auto it = g_soundBank.find(self);
        if (it != g_soundBank.end()) g_subBank[*subsound] = it->second;
    }
    return r;
}
extern "C" FMOD_RESULT getSubSoundC_detour(void* self, int index, void** subsound) {
    ensure_init();
    FMOD_RESULT r = g_getSub ? g_getSub(self, index, subsound) : -1;
    if (r == 0 && subsound && *subsound) {
        std::lock_guard<std::mutex> g(g_lock);
        g_subIndex[*subsound] = index;
        auto it = g_soundBank.find(self);
        if (it != g_soundBank.end()) g_subBank[*subsound] = it->second;
    }
    return r;
}

extern "C" FMOD_RESULT playSound_detour(void* self, int channelid, void* sound, int paused, void** channel) {
    ensure_init();
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
    // 反查子声音 index（getSubSound hook 记录的）
    int subIdx = -1;
    { std::lock_guard<std::mutex> g(g_lock); auto it = g_subIndex.find(sound); if (it != g_subIndex.end()) subIdx = it->second; }
    HapticDecision haptic = decide_haptic(subIdx, bank, verdict[0] == 'V');
    bool shouldHaptic = haptic.enabled;
    int playPaused = shouldHaptic ? 1 : paused;
    FMOD_RESULT r = g_playSound(self, channelid, sound, playPaused, channel);

    record_seen_effect(subIdx, bank, subname, verdict, shouldHaptic, haptic.gain);
    logf("PLAY  ch=%d sound=%p bank=%s sub=\"%s\" -> %s  idx=%d gain=%.2f dump=%d (%s)",
         channelid, sound, bank.c_str(), subname, verdict, subIdx, haptic.gain, haptic.dump ? 1 : 0, subIdx >= 0 ? sound_meaning(subIdx) : "-");
    if (haptic.attach) {
        if (shouldHaptic) ensure_haptic();
        bool tapped = (r == 0 && channel && *channel) ? attach_channel_tap(self, *channel, subIdx, haptic.gain, haptic.dump, shouldHaptic) : false;
        if (g_chSetPaused && channel && *channel)
            g_chSetPaused(*channel, paused ? 1 : 0);
        if (shouldHaptic && !tapped) {
            ensure_capture(self);
            logf("CHDSP: fallback master gate channel=%p r=%d idx=%d", channel ? *channel : nullptr, r, subIdx);
            g_gate.store(haptic.gain, std::memory_order_relaxed);
        }
    }
    if (bank == "(unknown)") diagnose_unknown(sound);  // 仍只读记录少量样本，便于复核
    return r;
}

extern "C" FMOD_RESULT channelSetPan_detour(void* channel, float pan) {
    ensure_init();
    update_pan_spatial(channel, pan);
    return g_chSetPan ? g_chSetPan(channel, pan) : -1;
}

extern "C" FMOD_RESULT channelSetSpeakerMix_detour(void* channel, float fl, float fr, float center, float lfe,
                                                   float bl, float br, float sl, float sr) {
    ensure_init();
    update_speaker_spatial(channel, fl, fr, center, lfe, bl, br, sl, sr);
    return g_chSetSpeakerMix ? g_chSetSpeakerMix(channel, fl, fr, center, lfe, bl, br, sl, sr) : -1;
}

extern "C" FMOD_RESULT channelSet3DAttributes_detour(void* channel, const FMOD_VECTOR_* pos, const FMOD_VECTOR_* vel) {
    ensure_init();
    update_3d_spatial(channel, pos);
    return g_chSet3DAttributes ? g_chSet3DAttributes(channel, pos, vel) : -1;
}

// ----------------------------------------------------------------------------
// 注：所有非 detour 导出（706 个，含 C 风格 FMOD_* 与 C++ 修饰名）都由 thunks.asm
// 的 jmp 桩透明转交，无需在此手写包装函数。do_init() 负责把 g_thunk_targets 填好。
// ----------------------------------------------------------------------------
static void do_init() {
    // 日志文件：%TEMP%（调试日志，不脏用户桌面）
    char path[MAX_PATH] = "fmod_probe_log.txt";
    char* up = nullptr; size_t n = 0;
    if (_dupenv_s(&up, &n, "TEMP") == 0 && up) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\fmod_probe_log.txt", up);
        free(up);
    }
    g_log = nullptr;
    g_log = _fsopen(path, "w", _SH_DENYWR);   // 共享读：DLL 写时外部进程仍可读日志(否则被独占锁死)

    g_orig = LoadLibraryA("fmodex64_orig.dll");
    if (!g_orig) { logf("FATAL: cannot load fmodex64_orig.dll (err=%lu)", GetLastError()); return; }

    g_createSound  = (createSound_t)  GetProcAddress(g_orig, "?createSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z");
    g_createStream = (createStream_t) GetProcAddress(g_orig, "?createStream@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z");
    g_createSoundInternal = (createSoundInternal_t) GetProcAddress(g_orig, "?createSoundInternal@SystemI@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIIIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVFile@2@_NPEAPEAVSoundI@2@@Z");
    g_playSound    = (playSound_t)    GetProcAddress(g_orig, "?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PEAVSound@2@_NPEAPEAVChannel@2@@Z");
    g_getName      = (Sound_GetName_t)            GetProcAddress(g_orig, "FMOD_Sound_GetName");
    g_getParent    = (SoundGetParent_t) GetProcAddress(g_orig, "FMOD_Sound_GetSubSoundParent");
    g_getNumSub    = (SoundGetNumSub_t) GetProcAddress(g_orig, "FMOD_Sound_GetNumSubSounds");
    g_getSub       = (Sound_GetSubSound_t)        GetProcAddress(g_orig, "FMOD_Sound_GetSubSound");
    g_getSubCpp    = (Sound_GetSubSound_t)        GetProcAddress(g_orig, "?getSubSound@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAPEAV12@@Z");
    // 震动用：声道组 + 捕获 DSP
    g_createCG  = (CreateCG_t)  GetProcAddress(g_orig, "FMOD_System_CreateChannelGroup");
    g_createDSP = (CreateDSP_t) GetProcAddress(g_orig, "FMOD_System_CreateDSP");
    g_addDSP    = (AddDSP_t)    GetProcAddress(g_orig, "FMOD_ChannelGroup_AddDSP");
    g_setCG     = (SetCG_t)     GetProcAddress(g_orig, "FMOD_Channel_SetChannelGroup");
    g_getMaster = (GetMaster_t) GetProcAddress(g_orig, "FMOD_System_GetMasterChannelGroup");
    g_chAddDSP    = (ChannelAddDSP_t)   GetProcAddress(g_orig, "FMOD_Channel_AddDSP");
    g_chSetPaused = (ChannelSetPaused_t)GetProcAddress(g_orig, "FMOD_Channel_SetPaused");
    g_dspRelease  = (DSPRelease_t)      GetProcAddress(g_orig, "FMOD_DSP_Release");
    g_chSetPan = (ChannelSetPan_t)GetProcAddress(g_orig, "FMOD_Channel_SetPan");
    g_chSetSpeakerMix = (ChannelSetSpeakerMix_t)GetProcAddress(g_orig, "FMOD_Channel_SetSpeakerMix");
    g_chSet3DAttributes = (ChannelSet3DAttributes_t)GetProcAddress(g_orig, "FMOD_Channel_Set3DAttributes");
    g_cgNumGroups = (CG_NumGroups_t) GetProcAddress(g_orig, "FMOD_ChannelGroup_GetNumGroups");
    g_cgGetGroup  = (CG_GetGroup_t)  GetProcAddress(g_orig, "FMOD_ChannelGroup_GetGroup");
    g_cgGetName   = (CG_GetName_t)   GetProcAddress(g_orig, "FMOD_ChannelGroup_GetName");
    g_cgNumChans  = (CG_NumChans_t)  GetProcAddress(g_orig, "FMOD_ChannelGroup_GetNumChannels");

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
        logf("haptic funcs: createDSP=%p chAddDSP=%p chSetPaused=%p dspRelease=%p",
            g_createDSP, g_chAddDSP, g_chSetPaused, g_dspRelease);
    logf("spatial funcs: setPan=%p setSpeakerMix=%p set3D=%p", g_chSetPan, g_chSetSpeakerMix, g_chSet3DAttributes);
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
