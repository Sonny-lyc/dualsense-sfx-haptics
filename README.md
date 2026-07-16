# 🎮 DualSense 音效触觉（只狼）

给 PC 版《只狼：影逝二度》补上 **Sony DualSense 手柄的细腻音效触觉** ——
**只有音效震动，音乐和语音不震。**

> 现有方案（如 DSX 的 Audio-to-Haptics）会把**所有声音**都转成震动，音乐、对白全在抖。
> 本工具通过 Hook 游戏的 FMOD 音频引擎，**从源头识别每一个声音**，只让战斗音效、闪避、
> 处决、濒死心跳等**该震的事件**驱动手柄触觉音圈，音乐/语音保持安静。

弹刀、命中、危攻、处决那一下，用的是**游戏真实音频**喂进手柄的触觉音圈，手感接近第一方游戏。

---

## ⚠️ 先看这里（重要）

- **仅限 PC 单机《只狼》**。声音编号是针对当前 Steam 版《只狼》实测标定的，换游戏/大版本可能要重新标定。
- **DualSense 必须用 USB 连接**（细腻触觉走手柄的 ch3/4 音频音圈，蓝牙用不了）。
- 手柄进游戏靠 **Steam Input**（模拟 Xbox 手柄）；本工具只负责触觉输出，不抢输入。
- **不需要 DSX / VB-CABLE**：v0.2 引擎直接给 DualSense 的 ch3/4 触觉音圈做逐 idx 分层整形（+ 弹刀/忍杀合成脉冲），手感已经不依赖第三方触觉软件；GUI 里也不再有输出方式的选择，装上就是这一种。延迟也优化过（环形缓冲从 ~170ms 收紧到 ~20-30ms）。
- 白名单是内置的、装上自动生效，不需要在 GUI 里手动管理每个音效。
- 首次运行 exe 和注入 DLL 可能被 **Windows SmartScreen / 杀软**拦截（未签名 + 代理 DLL）。本工具只读观察单机游戏音频、不联机、不改存档、不规避反作弊；介意者可自行看源码自编译。

---

## 📥 下载与安装

1. 到 [Releases](../../releases) 下载最新的 `DualSenseSfxHaptics-vX.Y.zip`，解压到任意文件夹。
2. 双击 **`DualSenseSfxHaptics.exe`** 打开控制面板。
3. **游戏目录**：一般会自动检测到只狼；没有的话点"浏览"选到含 `sekiro.exe` 的目录。
4. **关掉只狼**（如果开着），点 **「安装到游戏」**。
   工具会把游戏原本的 `fmodex64.dll` 备份为 `fmodex64_orig.dll`，再放入我们的代理 DLL。
5. 界面显示 **「代理已安装 ✓」** 即成功。
6. （可选）点 **「安装 PS5 按键图标」**，把游戏内的 Xbox 按键提示图标换成 PS5 风格（第三方 Mod Engine + PS5 Glyphs，详见 `iconmod\CREDITS.txt`；不装也不影响触觉功能）。
7. **USB** 接 DualSense → Steam 给只狼开 **Steam Input** → 启动游戏，开玩。

**卸载/还原**：面板点「卸载 / 还原」即可恢复游戏原状（或手动把 `fmodex64_orig.dll` 改回 `fmodex64.dll`）。图标模组同样可在面板里一键卸载。

白名单内置在代理 DLL 里，装上即自动生效，不需要在 GUI 里手动勾选/管理每个音效。

---

## 🎛️ 目前会震动的事件

战斗（真实音频，手感最佳）：**弹刀/格挡、命中、危攻、受伤/死亡、不死斩挥刀、处决**。
其它：**闪避、濒死心跳、菜单/UI、归佛传送、到新地点地名音**。
**不震**：背景音乐、人物语音、普通走路（可选）。

---

## 🧩 工作原理（简）

游戏目录里有真正的 `fmodex64.dll`。本工具放一个**同名代理 DLL**：把 700+ 个导出原样转发给真引擎（游戏无感），
只拦几个关键函数，用 `getSubSound` 记录**每个声音在 bank 里的编号**，playSound 时反查 →
在白名单（弹刀/命中/闪避…）命中时，**给那个音效自己的通道单独挂一个透明 DSP**，
只抓**这一路纯净的音效**（不含并发的音乐/危攻），做触觉整形（高通取瞬态 + 包络）后喂给 DualSense 的 ch3/4 触觉音圈。

**逐通道隔离**是 v0.2 的关键升级：早期"喂 master 混音"会把并发音混在一起（如危攻盖住识破）；
现在每个音效**各抓各的通道**，天然不漏音乐、也不互相吞。**不解密、不分离音频、不改游戏行为，纯只读观测 + 触觉输出。**

触觉行为由 `%AppData%\DualSenseSfxHaptics\haptics.json` 配置，默认走内置白名单（`useBuiltinDefaults: true`），装上自动生效，不需要手动编辑。
技术实现细节见贡献者 [wanshuai12138 的实现说明](https://github.com/wanshuai12138/dualsense-sfx-haptics)；研发历程见 [`PROJECT_CHANGES.md`](PROJECT_CHANGES.md)。

---

## 🛠️ 从源码自编译（开发者）

**环境**：Windows 10/11 x64、Visual Studio 2022 Build Tools（含 C++/MASM）、CMake、.NET 8 SDK。

```bash
# 1) 编译代理 DLL
cmake -S src/native/fmod_probe -B build -A x64
cmake --build build --config Release      # 产物 build/Release/fmodex64.dll

# 2) 编译 GUI（会自动打包上面的代理 DLL）
dotnet build src/gui/DualSenseHaptics.csproj -c Release

# 或发布自包含单文件（别人不用装 .NET）
dotnet publish src/gui/DualSenseHaptics.csproj -c Release -r win-x64 \
    --self-contained true -p:PublishSingleFile=true
```

---

## 📋 系统要求

- Windows 10 / 11（x64）
- PC 版《只狼：影逝二度》（Steam）
- Sony DualSense 手柄（**USB 有线**）
- Steam Input（把手柄当 Xbox 用）
- 不需要额外软件（不依赖 VB-CABLE / DSX）

---

## 🙏 致谢

- **[wanshuai12138](https://github.com/wanshuai12138/dualsense-sfx-haptics)** —— 贡献了 v0.2 的核心引擎：
  **逐通道透明 DSP 隔离**（每个音效抓自己的通道，稳定不崩、天然不漏音乐）、逐通道触觉整形、
  每事件寿命、左右马达空间化、配置化系统。本项目的音频隔离难题因此得解。

---

## 📝 协议

MIT License。仅供个人使用：只读观察单机游戏音频，不修改存档、不联机、不规避反作弊。
《只狼：影逝二度》及其素材版权归 FromSoftware / Activision 所有；本仓库**不含任何游戏文件**。

---

**当前版本**：0.2（引擎逐 idx 分层整形、弹刀/忍杀合成脉冲、换音即断生命周期、白名单大幅校准）
