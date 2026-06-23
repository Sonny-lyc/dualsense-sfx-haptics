# 🎮 DualSense 游戏音效→震动转换工具

一个 Windows 桌面应用，通过 **Hook 游戏的 FMOD 音频引擎**，把游戏音效实时映射到
Sony DualSense 手柄的触觉马达上。

**目标：为 PS5 发售前的经典游戏（如《只狼》《鬼泣5》）补上 DualSense 音效马达反馈。**

---

## 🎯 核心概念

### 问题背景
- PS5 发售前的游戏没有 DualSense 适配
- DualSense 拥有出色的**音频触觉马达**
- 现有方案（如 DSX）把**所有声音**都转成震动，音乐、对话全在抖，干扰严重
- 我们只想要**音效**震动，**音乐和对话不震**

### 解决方案核心

> **不分析声音波形，而是 Hook FMOD 引擎，按"声音来自哪个 bank"来过滤。**

```
游戏触发声音 → Hook FMOD playSound → 看来源 bank
            → 音乐(sm*) / 语音(vm*) → 跳过
            → 其余音效            → 输出震动
```

这套方案的关键优势（相对"捕获混音后音频再用频率猜哪些是音效"）：

| | 音频分析方案（已废弃） | FMOD Hook 方案（当前） |
|---|---|---|
| 分类依据 | FFT 频率特征，靠猜 | 声音来源 bank 文件名，确定性 |
| 区分音乐/语音 | 频率重叠，易误判 | 100% 准确（游戏自己分好了 bank） |
| 延迟 | 要做 FFT | 触发瞬间即知，近乎零延迟 |
| 是否需要解密/分离 | 需要 | **都不需要** |

---

## 💡 为什么可行：游戏自己把音频分好了类

调研《只狼》实际文件后确认：它用 **FMOD Ex** 引擎，音频按 `.fsb` bank **分门别类**存放。
我们只需在运行时读出"正在播的声音属于哪个 bank"，就能直接判断震不震——
**不需要识别具体是什么音效，只需要知道它不是音乐、不是语音。**

用户规则极简：**除了对话和音乐，其余一律震动。**
→ 于是判断逻辑塌缩成一句话：**这声音来自 `sm*`(音乐) 或 `vm*`(语音) 吗？**

---

## 🏗️ 技术架构

### FMOD Ex 代理 DLL

游戏目录里有真正的 `fmodex64.dll`。我们做一个**同名代理 DLL**：

```
┌─────────────────────────────────────────────┐
│  游戏进程 (只狼)                              │
│     │ 调用 fmodex64.dll 的导出函数            │
│     ↓                                         │
│  代理 fmodex64.dll  ← 我们的 DLL              │
│     ├─ 706 个导出 → 原样转发给 真dll          │
│     └─ createSound / createStream / playSound │
│            └─ 包一层：记录来源 bank + 判断     │
│            ↓ 调用真函数                         │
│  fmodex64_orig.dll (真引擎，行为不变)          │
└─────────────────────────────────────────────┘
            ↓ "该震了" + 强度
      DualSense 手柄（已有的震动控制）
```

- **transparent**：706/709 个导出原样转发，游戏察觉不到差别
- 只拦截 3 个函数：`createSound`/`createStream`（记录 `Sound* → bank 文件名`）、
  `playSound`（用 `FMOD_Sound_GetSubSoundParent` 反查父 bank，判断震不震）
- **不依赖 DLL 注入框架**，纯导出转发；初始化惰性执行，避开 loader-lock

### 《只狼》bank 分类（已由社区 mod 资料 + 实测导出表证实）

| 文件 | 内容 | 是否震动 |
|------|------|---------|
| `smain.fsb` / `main.fsb` | 通用音效：格挡、死亡、受伤、回血 | ✅ 震 |
| `c####.fsb` | 角色专属音（敌人死亡等） | ✅ 震 |
| `rm##.fsb` | 区域环境/定位音 | ✅ 震 |
| `sm##.fsb` | 地图音乐（BGM） | ❌ 不震 |
| `vm##.fsb` | 地图对白（语音，带 `_enu/_jaj` 语言后缀） | ❌ 不震 |

> 注：`smain.fsb`（音效）≠ `sm##.fsb`（音乐）。判断规则：`sm` 后接数字才是音乐。

---

## 🚦 当前状态

### ✅ 已完成
- DualSense 手柄检测 + 基础震动控制（Electron + node-hid）
- 《只狼》音频引擎调研：确认 FMOD Ex，导出表实测解析（709 导出，C++ API，4.x 世代）
- bank 分类表（音乐/语音/音效）确认
- **FMOD 代理探针**（`src/native/fmod_probe/`）：记录每个声音来源 bank 的只读观测工具

- **探针已编译、部署并验证可被 `fmod_event64.dll` 正常加载**（2026-06-22）：
  代理 `fmodex64.dll` 共 **709 个导出**（706 个 **jmp 跳转桩** + 3 个拦截 detour，与原 dll 数量一致、完全透明）。
  ⚠️ 原方案"PE forwarder 按序号转发"运行时跑不通（forwarder 解析不含应用目录 + .def 引号被写进导出名），
  已改为 jmp 桩 + DllMain 提前初始化。详见 `PROJECT_CHANGES.md` 傍晚那条日志。

- **音效过滤逻辑已实跑验证**（2026-06-22）：`sm##`/`vm##` 不震、其余震；规则塌缩为「除音乐/语音外一律震」。
- **细腻触觉打通、实战可用**（2026-06-23）：捕获游戏音频 → 喂进 **DualSense ch3/4 触觉音圈**，
  实测爽快（打穿幻影破戒僧）。详见 `PROJECT_CHANGES.md` 6-23 日志。

### 🔄 进行中
- 输出方案 = **master 捕获 + 音效门控**：DSP 挂 master 主组拿连续 PCM → WASAPI 写 ch3/4，
  用音效检测开/关"门"（音乐独奏不震）。⚠️ 必须 **USB 有线**（ch3/4 触觉不走蓝牙）。
- 调参（增益/低通/门衰减）、减少战斗中音乐"漏进"触觉。

### ⏳ 待做
- 更精确的"只含音效"捕获（绕开 FMOD Event 系统抢路由的问题）
- 逐事件精细触觉预设（hook `getSubSound` 观察取 670-673=格挡等）
- UI / 配置 / 适配其他游戏

### 📐 关键技术点（输出端）
- DualSense **USB** 暴露 4 声道音频设备，**ch3/4 = 左右触觉音圈**；用设备真实格式 blob 做 WASAPI 独占输出。
- FMOD Ex **4.44** 的 DSP read 回调末参是 `int outchannels`（按值，非指针）——踩坑见日志。
- FMOD Event 系统会覆盖 `SetChannelGroup` 路由，故改用 master 捕获 + 门控。

### ❌ 已废弃（代码保留但不再使用）
- DirectSound Hook 方案（`DirectSoundHook.js`）
- 混音后 FFT + 规则检测 SFX（`AudioAnalyzer.js` / `SFXDetector.js` 的频率路径）
- 基于 `freqRange` 的 `sekiro.json` 配置

---

## 🔬 探针：第一步要回答的问题

部署见 [`src/native/fmod_probe/README.md`](src/native/fmod_probe/README.md)。跑一局后看
`%USERPROFILE%\Desktop\fmod_probe_log.txt`，确认：

- [ ] 战斗音（格挡/受伤）是否走 `playSound`、归在 `smain.fsb` / `c####.fsb`
- [ ] BGM 是否归 `sm##.fsb`、对话是否归 `vm##.fsb`
- [ ] 是否有大量 `bank=(unknown)`（需补登记子音）
- [ ] `xm##.fsb`、`smain_<lang>.fsb` 装什么、归哪类
- [ ] 若战斗音不走 `playSound` 而走 event 系统 → 需再给 `fmod_event64.dll` 做代理

---

## 🛠️ 技术栈

### 已有
- **Frontend**: React 18, Electron 27
- **Hardware**: node-hid（DualSense 通信）

### Native 层（Hook）
- **C++ / Win32**：FMOD Ex 代理 DLL（纯导出转发，无第三方 Hook 框架）
- 构建：CMake + MSVC（x64）

---

## 📋 系统要求

- **操作系统**: Windows 10 / 11（x64）
- **硬件**: Sony DualSense 手柄（USB 或蓝牙）
- **游戏**: 使用 FMOD 引擎的单机游戏（首个适配目标：《只狼》）
- **开发环境**: Node.js v14+，Visual Studio + CMake（编译 native 探针）

---

## 📝 协议

MIT License。**仅供个人使用**：本工具只读地观察单机游戏音频，不修改存档、不联机、不规避反作弊。

---

**版本**: 0.3.0（细腻触觉打通：音频 → DualSense ch3/4）
**最后更新**: 2026-06-23
**当前阶段**: ✅ 实战可用（音效→触觉音圈）→ 调参 & 减少音乐漏入
