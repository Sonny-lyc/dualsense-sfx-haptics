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
- 这是**早期版本**：战斗撞击手感已经很好；闪避/UI/心跳/传送等系统音还在打磨（可能偏糊或有延迟）。
- 首次运行 exe 和注入 DLL 可能被 **Windows SmartScreen / 杀软**拦截（未签名 + 代理 DLL）。本工具只读观察单机游戏音频、不联机、不改存档、不规避反作弊；介意者可自行看源码自编译。

---

## 📥 下载与安装

1. 到 [Releases](../../releases) 下载最新的 `DualSenseSfxHaptics-vX.Y.zip`，解压到任意文件夹。
2. 双击 **`DualSenseSfxHaptics.exe`** 打开控制面板。
3. **游戏目录**：一般会自动检测到只狼；没有的话点"浏览"选到含 `sekiro.exe` 的目录。
4. **关掉只狼**（如果开着），点 **「安装到游戏」**。
   工具会把游戏原本的 `fmodex64.dll` 备份为 `fmodex64_orig.dll`，再放入我们的代理 DLL。
5. 界面显示 **「代理已安装 ✓」** 即成功。

**卸载/还原**：面板点「卸载 / 还原」即可恢复游戏原状（或手动把 `fmodex64_orig.dll` 改回 `fmodex64.dll`）。

---

## 🎮 使用：两种触觉输出

| 方式 | 需要的软件 | 手感 | 适合 |
|---|---|---|---|
| **虚拟声卡 → DSX** ⭐**强烈推荐** | VB-CABLE（免费）+ DSX（Steam 付费） | **最佳** | 想要真正的细腻触觉 |
| DualSense ch3/4 直驱 | 无，开箱即用 | 一般（**弹刀等高频音较弱/几乎不震**） | 手头没 DSX，先随便试试 |

> **为什么强烈推荐 DSX 那条路？**
> 手柄的触觉音圈只能有效感受 **~50–300Hz 的低频**。直驱是把音频**原样**喂给音圈——像**弹刀**
> 这种高频金属声会被音圈滤掉、几乎不震。而 **DSX 的 Audio-to-Haptics 会做"包络提取 + 低频合成"**：
> 把高频音的"强度"转成低频冲击，所以**不管什么音效都能变成手上摸得到的细腻触觉**。
> 本工具负责"只把音效抓出来"，DSX 负责"把音效变成好触觉"，**两者结合才是完全体**。

---

### 🅰️ 推荐路线：VB-CABLE + DSX（手感最佳）

#### 先搞懂音频怎么流转（理解了就不会配错）

```
 只狼的音效 ──本工具抓取──▶  CABLE Input  ──DSX 从这里监听──▶  DSX ──▶ 手柄触觉
（音乐/语音不抓）            (虚拟声卡·播放端)                    转触觉
```

- **VB-CABLE 是一根"虚拟音频线"**：装好后系统里会出现一对设备——**CABLE Input**（播放端）和 **CABLE Output**（录音端）。
- 本工具把**抓到的音效**送进 **CABLE Input**；**DSX 直接监听 CABLE Input**（loopback 抓取这个播放端的声音），转成手柄触觉。
  （所以我们用到的是 **CABLE Input** 这一端；CABLE Output 用不到。）
- ✅ **游戏正常声音不受影响**：本工具是**单独复制一份音效**送去虚拟声卡，你的**喇叭/耳机照常出声**，
  **不需要改 Windows 默认播放设备**。

#### 步骤

**① 安装 VB-CABLE（免费）**
- **最简单**：打开本工具 → 点「触觉输出」区的 **「↓ 一键下载安装 VB-CABLE」** → 同意 →
  工具从 VB-Audio 官方下载并打开安装器 → 点 `Install Driver` → **重启电脑**。
- **或手动**：打开 https://vb-audio.com/Cable/ 下载 → 右键 `VBCABLE_Setup_x64.exe` 以管理员运行 →
  `Install Driver` → **重启电脑**。
- 装好后系统里应能看到 **CABLE Input** 和 **CABLE Output** 两个设备（本工具状态栏也会显示「✓ VB-CABLE 已安装」）。

**② 安装 DSX** —— 在 **Steam** 搜索安装 **DSX（DualSenseX）**（付费应用）。

**③ 安装本工具代理** —— 打开 `DualSenseSfxHaptics.exe` → 选游戏目录 → **关掉只狼** →
点 **「安装到游戏」** → 显示「代理已安装 ✓」。

**④ 本工具输出切到虚拟声卡** —— 面板「触觉输出」选 **「虚拟声卡 → DSX」**。
工具会自动把音效送进 CABLE Input，**你不用在 Windows 声音设置里做任何改动**。

**⑤ 配置 DSX 的音频来源（关键！最容易选错的一步）**
1. 打开 DSX，找到 **Audio to Haptics**（音频→触觉）功能，**开启**。
2. 把它的**音频输入/监听设备**选成 👉 **`CABLE Input (VB-Audio Virtual Cable)`**
   —— 就是那个带 **CABLE** 字样的虚拟声卡（本工具把音效送进这里、DSX 从这里 loopback 监听）；不是你的喇叭、不是"默认设备"。
   > 若你的 DSX 版本里选 Input 没反应，再试 **CABLE Output**；核心是选那个带 **CABLE** 字样的虚拟声卡。
3. （可选）在 DSX 里把强度、低通调到你喜欢的手感。

**⑥ 开玩** —— **USB** 接 DualSense → Steam 给只狼开 **Steam Input** → 启动游戏 →
战斗时手柄就有**只含音效**的细腻触觉了 🎉

> ⚠️ **常见坑**
> - **DSX 里设备选错**（选成喇叭/默认设备）→ 会把**所有声音**都转触觉，音乐也震 → **必须选带 CABLE 字样的虚拟声卡（先试 CABLE Input）**。
> - **别**把 CABLE 设成 Windows 默认播放设备（那样游戏声音会跑进虚拟线、喇叭反而没声）。本工具已单独送 CABLE，无需这么做。
> - **不震/没声**：确认 VB-CABLE 已装且**重启过**、本工具输出是「虚拟声卡→DSX」、DSX 的 Audio to Haptics 已开且选了 CABLE Input。

---

### 🅱️ 省事路线：DualSense ch3/4 直驱（不装任何额外软件）

1. 双击 `DualSenseSfxHaptics.exe` → 关游戏 → **「安装到游戏」**。
2. 触觉输出选 **「DualSense ch3/4 直驱」**。
3. **USB** 接 DualSense → Steam Input 启动只狼 → 开玩。

> 直驱由本工具自己驱动手柄的 ch3/4 触觉音圈，不需要 VB-CABLE / DSX，手感也不错，只是不如 DSX 处理细腻。

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

触觉行为由 `%AppData%\DualSenseSfxHaptics\haptics.json` 配置（GUI 的"音效管理"面板可视化调）。
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
- 可选：VB-CABLE + DSX（走虚拟声卡那条路时）

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

**当前版本**：0.1（GUI 成品化：一键安装/状态/输出切换）
