# FMOD 探针（fmod_probe）— 只狼音频观测

一个 **FMOD Ex 代理 DLL**，只读地记录《只狼》播放的每个声音来自哪个 `.fsb` bank，
用来验证"排除 `sm*`(音乐) / `vm*`(语音)、其余全震"这条过滤规则。

**不插手柄、不震动、不改游戏行为，纯打印日志。**

---

## 原理

游戏目录里有真正的 `fmodex64.dll`。我们：

1. 把真 dll 改名为 `fmodex64_orig.dll`
2. 把本项目编出的同名 `fmodex64.dll`（代理）放进游戏目录
3. 代理把 709 个导出里的 706 个**原样转发**给 `fmodex64_orig.dll`，
   只把 `createSound` / `createStream` / `playSound` 三个换成带日志的版本

游戏察觉不到差别，照常运行；我们在旁边记账。

判断逻辑（在 `dllmain.cpp::classify`）：
- 文件名以 `vm` 开头 → `SKIP(voice)`
- 文件名 `sm` + 数字（如 `sm11.fsb`）→ `SKIP(music)`（注意 `smain.fsb` 不算音乐）
- 其余 → `VIBRATE`

---

## 构建（需要 Visual Studio 2019/2022 + CMake）

```powershell
cd "src/native/fmod_probe"
cmake -B build -A x64
cmake --build build --config Release
# 产物：build/Release/fmodex64.dll
```

> 必须是 **x64**。`fmodex64.def` 已由脚本从本机 `fmodex64.dll` 的导出表生成，709 条一一对应。

---

## 部署与测试（不需要手柄）

1. 进入 `C:\Program Files (x86)\Steam\steamapps\common\Sekiro\`
2. **备份**：把 `fmodex64.dll` 复制一份留底
3. 把 `fmodex64.dll` **改名**为 `fmodex64_orig.dll`
4. 把构建出的代理 `fmodex64.dll` 放进该目录
5. 启动游戏，随便打一场（格挡、跑动、触发对话、听 BGM）
6. 退出，查看日志：`%USERPROFILE%\Desktop\fmod_probe_log.txt`

### 恢复原状
删掉代理 `fmodex64.dll`，把 `fmodex64_orig.dll` 改回 `fmodex64.dll` 即可。

---

## 日志怎么读

```
=== fmod_probe loaded ===
orig=... createSound=... playSound=...        ← 指针都非空 = hook 生效
CREATE sound=0x... bank=smain.fsb mode=0x...  ← 某个 bank 被加载
PLAY  ch=-1 sound=0x... bank=smain.fsb sub="670" -> VIBRATE
PLAY  ch=-1 sound=0x... bank=sm11.fsb  sub="3"   -> SKIP(music)
PLAY  ch=-1 sound=0x... bank=vm11.fsb  sub="..." -> SKIP(voice)
```

### 这局要回答的问题（决定最终过滤逻辑）
- [ ] 战斗音（格挡/受伤）的 `bank` 是不是 `smain.fsb` / `c####.fsb`？→ 确认归 VIBRATE
- [ ] BGM 播放时 `bank` 是不是 `sm##.fsb`？→ 确认 `sm+数字` 规则成立
- [ ] 对话时 `bank` 是不是 `vm##.fsb`？
- [ ] 有没有大量 `bank=(unknown)`？→ 说明子音→父音反查没覆盖，需要在 `createSound` 时枚举子音补登记
- [ ] `xm##.fsb`、`smain_<lang>.fsb` 到底装什么、归哪类？

把日志贴回来，我据此把 `classify()` 的规则定死，再进入"输出震动"阶段。

---

## 已知风险 / 备选

- 若游戏加载即崩：可能是 loader-lock 或导出名不匹配。日志里 `createSound=0`/`playSound=0`
  说明导出名与本机 dll 不符——重新跑导出解析脚本核对（见 `src/_fmod_exports.txt`）。
- 若战斗音大量走 `playSound` 以外的路径（如直接走 event 系统 `fmod_event64.dll`），
  则需另做一个 `fmod_event64.dll` 代理，钩 event 触发。本探针会先告诉我们是否需要这一步。
