using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using Microsoft.Win32;
using NAudio.CoreAudioApi;

namespace DualSenseSfxHaptics;

public class MainForm : Form
{
    // ---- 颜色 ----
    static readonly Color OK = Color.FromArgb(46, 160, 67);
    static readonly Color BAD = Color.FromArgb(206, 84, 84);
    static readonly Color MUTE = Color.FromArgb(140, 140, 140);
    static readonly Color BG = Color.FromArgb(32, 34, 40);
    static readonly Color PANEL = Color.FromArgb(42, 45, 53);
    static readonly Color FG = Color.FromArgb(230, 232, 236);

    // ---- 控件 ----
    TextBox txtGamePath = null!;
    Label lblStProxy = null!, lblStGame = null!, lblStPad = null!, lblStIconMod = null!;
    Button btnInstall = null!, btnUninstall = null!;
    Button btnInstallIconMod = null!, btnUninstallIconMod = null!;
    Label lblMsg = null!;
    Button btnLang = null!;
    System.Windows.Forms.Timer statusTimer = null!;

    string ProxySrc => Path.Combine(AppContext.BaseDirectory, "proxy", "fmodex64.dll");
    string IconModSrcDir => Path.Combine(AppContext.BaseDirectory, "iconmod");
    string AppDir => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "DualSenseSfxHaptics");
    string SettingsFile => Path.Combine(AppDir, "settings.txt");
    string HapticsConfigFile => Path.Combine(AppDir, "haptics.json");

    // ============================ 中英文切换 ============================
    // 用法：
    //   ① 一次性创建、之后不会再重新赋值的静态文字（标题/按钮/面板标题）——创建时用 Reg(control, "中文")
    //      注册，切换语言时会自动回填新文字。
    //   ② 会被反复动态赋值的文字（状态行 SetDot、提示 Msg）——直接把中文字面量传给 Tr(...)，
    //      因为这些地方本来就会被状态刷新（每 2 秒的 statusTimer）或用户操作重新调用，
    //      不需要额外注册，下一次刷新自然就是新语言。
    bool _english = false;
    readonly List<(Control ctrl, string zh)> _i18n = new();

    // 中文 -> 英文 对照表。找不到的词条原样返回中文（漏翻不会崩，只是那一条还没翻）。
    static readonly Dictionary<string, string> Dict = new() {
        ["DualSense 音效触觉  v0.2"] = "DualSense SFX Haptics  v0.2",
        ["DualSense 音效触觉"] = "DualSense SFX Haptics",
        ["只把《只狼》的音效变成手柄细腻触觉（音乐/语音不震）"] = "Sekiro SFX -> fine DualSense haptics (no music/voice)",
        ["游戏目录（只狼 Sekiro）"] = "Game folder (Sekiro)",
        ["浏览…"] = "Browse…",
        ["自动检测"] = "Auto-detect",
        ["状态"] = "Status",
        ["安装到游戏"] = "Install to game",
        ["卸载 / 还原"] = "Uninstall / restore",
        ["PS5 按键图标（可选，第三方：Mod Engine + PS5 Glyphs，见 iconmod\\CREDITS.txt）"] = "PS5 button icons (optional, 3rd-party, see iconmod\\CREDITS.txt)",
        ["安装图标模组"] = "Install icon mod",
        ["卸载图标模组"] = "Uninstall icon mod",

        // 状态行
        ["代理已安装到游戏 ✓"] = "Proxy installed ✓",
        ["代理未安装（点\"安装到游戏\"）"] = "Proxy not installed (click \"Install to game\")",
        ["先选择游戏目录"] = "Select the game folder first",
        ["只狼正在运行"] = "Sekiro is running",
        ["只狼未运行"] = "Sekiro is not running",
        ["DualSense 已连接（音频设备）"] = "DualSense connected (audio device)",
        ["未检测到 DualSense（请 USB 连接）"] = "DualSense not detected (connect via USB)",
        ["PS5 按键图标已安装"] = "PS5 icons installed",
        ["PS5 按键图标未安装（可选）"] = "PS5 icons not installed (optional)",

        // 提示消息
        ["没自动找到只狼，请点\"浏览\"手动选游戏目录"] = "Couldn't auto-detect Sekiro, please click \"Browse\" to select the folder manually",
        ["⚠ 找不到打包的代理 DLL（proxy\\fmodex64.dll）。请先编译 fmod_probe。"] = "⚠ Bundled proxy DLL not found (proxy\\fmodex64.dll). Please build fmod_probe first.",
        ["只狼运行中，安装/卸载需先关闭游戏。"] = "Sekiro is running — please close the game before installing/uninstalling.",
        ["请先关闭只狼再安装。"] = "Please close Sekiro before installing.",
        ["找不到代理 DLL。"] = "Proxy DLL not found.",
        ["游戏目录里没有 fmodex64.dll，路径可能不对。"] = "No fmodex64.dll in the game folder — the path may be wrong.",
        ["当前 fmodex64.dll 偏小，可能不是原版引擎（或已被改过）。仍要继续吗？"] = "The current fmodex64.dll is smaller than expected — it may not be the original engine (or was already modified). Continue anyway?",
        ["确认"] = "Confirm",
        ["✓ 安装成功！用 Steam Input 启动只狼，USB 接 DualSense 即可。"] = "✓ Installed! Launch Sekiro via Steam Input with the DualSense connected over USB.",
        ["权限不足：请右键\"以管理员身份运行\"本程序后重试。"] = "Insufficient permissions: right-click and \"Run as administrator\", then retry.",
        ["安装失败："] = "Install failed: ",
        ["请先关闭只狼再卸载。"] = "Please close Sekiro before uninstalling.",
        ["找不到备份 fmodex64_orig.dll，无法自动还原。"] = "Backup fmodex64_orig.dll not found — cannot auto-restore.",
        ["✓ 已卸载，游戏恢复原状。"] = "✓ Uninstalled — the game is back to its original state.",
        ["权限不足：请以管理员身份运行后重试。"] = "Insufficient permissions: run as administrator, then retry.",
        ["卸载失败："] = "Uninstall failed: ",
        ["请先关闭只狼再安装图标模组。"] = "Please close Sekiro before installing the icon mod.",
        ["找不到打包的图标模组素材（iconmod\\）。"] = "Bundled icon mod assets not found (iconmod\\).",
        ["请先选择只狼游戏目录。"] = "Please select the Sekiro game folder first.",
        ["✓ PS5 图标模组已安装（Mod Engine + PS5 Glyphs），下次启动只狼生效。"] = "✓ PS5 icon mod installed (Mod Engine + PS5 Glyphs) — takes effect next time Sekiro starts.",
        ["安装图标模组失败："] = "Icon mod install failed: ",
        ["请先关闭只狼再卸载图标模组。"] = "Please close Sekiro before uninstalling the icon mod.",
        ["✓ 已卸载图标模组（删掉 dinput8.dll 即可，游戏恢复 Xbox 图标）。"] = "✓ Icon mod uninstalled (dinput8.dll removed — the game is back to Xbox-style icons).",
        ["卸载图标模组失败："] = "Icon mod uninstall failed: ",
        ["选择只狼安装目录（包含 sekiro.exe）"] = "Select the Sekiro install folder (containing sekiro.exe)",
        ["提醒：该目录下没找到 sekiro.exe，请确认选对了。"] = "Note: sekiro.exe was not found in that folder — please double-check.",
    };

    // 动态文字统一走这里：中文找不到英文对照就原样返回，不会崩、只是暂时没翻。
    string Tr(string zh) => _english ? (Dict.TryGetValue(zh, out var en) ? en : zh) : zh;

    // 静态控件创建时注册：立刻应用当前语言，并记住以便切换时刷新
    T Reg<T>(T c, string zh) where T : Control
    {
        _i18n.Add((c, zh));
        c.Text = Tr(zh);
        return c;
    }

    void ToggleLanguage()
    {
        _english = !_english;
        foreach (var (c, zh) in _i18n) c.Text = Tr(zh);
        btnLang.Text = _english ? "中文" : "EN";
        Text = Tr("DualSense 音效触觉  v0.2");
        RefreshStatus(); // 状态行/按钮上的提示立刻跟着刷新，不用等 2 秒计时器
    }

    public MainForm()
    {
        Text = "DualSense 音效触觉  v0.2";
        BackColor = BG;
        ForeColor = FG;
        Font = new Font("Microsoft YaHei UI", 9.5f);
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(760, 480);

        BuildUi();

        // 确保初始语言状态（标题栏、语言按钮文字）跟 _english 的初始值保持一致，
        // 不用等用户真正点一次切换按钮才同步。
        Text = Tr("DualSense 音效触觉  v0.2");
        btnLang.Text = _english ? "中文" : "EN";

        txtGamePath.Text = LoadGamePath() ?? AutoDetectGame() ?? "";
        EnsureDefaultHapticsConfig();

        statusTimer = new System.Windows.Forms.Timer { Interval = 2000 };
        statusTimer.Tick += (s, e) => RefreshStatus();
        statusTimer.Start();
        RefreshStatus();
    }

    // ============================ UI ============================
    void BuildUi()
    {
        int x = 20, w = ClientSize.Width - 40, y = 16;

        // ---- 语言切换按钮（右上角） ----
        btnLang = MakeButton("EN", ClientSize.Width - 90, 16, 70, 26);
        btnLang.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        btnLang.Click += (s, e) => ToggleLanguage();
        Controls.Add(btnLang);

        var title = Reg(new Label {
            Font = new Font("Microsoft YaHei UI", 15f, FontStyle.Bold),
            ForeColor = FG, AutoSize = false, Location = new Point(x, y), Size = new Size(w - 80, 32),
            Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
        }, "DualSense 音效触觉");
        Controls.Add(title);
        var sub = Reg(new Label {
            ForeColor = MUTE, Location = new Point(x, y + 34), Size = new Size(w, 20),
            Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
        }, "只把《只狼》的音效变成手柄细腻触觉（音乐/语音不震）");
        Controls.Add(sub);
        y += 66;

        // ---- 游戏路径 ----
        var gp = MakePanel("游戏目录（只狼 Sekiro）", x, y, w, 92);
        gp.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(gp);
        txtGamePath = new TextBox {
            Location = new Point(14, 30), Size = new Size(gp.Width - 28, 26),
            BackColor = Color.FromArgb(28, 30, 36), ForeColor = FG, BorderStyle = BorderStyle.FixedSingle,
            ReadOnly = true, Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
        };
        gp.Controls.Add(txtGamePath);
        var btnBrowse = Reg(MakeButton("浏览…", 14, 60, 110), "浏览…");
        btnBrowse.Click += (s, e) => BrowseGame();
        gp.Controls.Add(btnBrowse);
        var btnAuto = Reg(MakeButton("自动检测", 132, 60, 170), "自动检测");
        btnAuto.Click += (s, e) => { var g = AutoDetectGame(); if (g != null) { txtGamePath.Text = g; SaveGamePath(g); RefreshStatus(); } else Msg(Tr("没自动找到只狼，请点\"浏览\"手动选游戏目录"), BAD); };
        gp.Controls.Add(btnAuto);
        y += 104;

        // ---- 状态 ----
        var sp = MakePanel("状态", x, y, w, 118);
        sp.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(sp);
        lblStProxy = MakeStatusLine(sp, 30);
        lblStGame = MakeStatusLine(sp, 52);
        lblStPad = MakeStatusLine(sp, 74);
        lblStIconMod = MakeStatusLine(sp, 96);
        y += 130;

        // ---- 安装按钮 ----
        btnInstall = Reg(MakeButton("安装到游戏", x, y, 222, 40, accent: true), "安装到游戏");
        btnInstall.Click += (s, e) => DoInstall();
        btnInstall.Anchor = AnchorStyles.Top | AnchorStyles.Left;
        Controls.Add(btnInstall);
        btnUninstall = Reg(MakeButton("卸载 / 还原", x + 238, y, 222, 40), "卸载 / 还原");
        btnUninstall.Click += (s, e) => DoUninstall();
        btnUninstall.Anchor = AnchorStyles.Top | AnchorStyles.Left;
        Controls.Add(btnUninstall);
        y += 54;

        // ---- PS5 按键图标（可选，第三方 Mod Engine + PS5 Glyphs） ----
        var mp = MakePanel("PS5 按键图标（可选，第三方：Mod Engine + PS5 Glyphs，见 iconmod\\CREDITS.txt）", x, y, w, 78);
        mp.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(mp);
        btnInstallIconMod = Reg(MakeButton("安装图标模组", 14, 30, 176, 34, accent: true), "安装图标模组");
        btnInstallIconMod.Click += (s, e) => DoInstallIconMod();
        mp.Controls.Add(btnInstallIconMod);
        btnUninstallIconMod = Reg(MakeButton("卸载图标模组", 200, 30, 176, 34), "卸载图标模组");
        btnUninstallIconMod.Click += (s, e) => DoUninstallIconMod();
        mp.Controls.Add(btnUninstallIconMod);
        y += 90;

        // ---- 消息 ----
        lblMsg = new Label {
            Location = new Point(x, y), Size = new Size(w, 40), ForeColor = MUTE,
            TextAlign = ContentAlignment.TopLeft,
            Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
        };
        Controls.Add(lblMsg);
    }

    // ============================ 音效配置 ============================
    // 不再暴露"音效管理"界面——直接用引擎内置白名单，装上就是全部该震的都震，不用手动勾。
    void EnsureDefaultHapticsConfig()
    {
        Directory.CreateDirectory(AppDir);
        if (File.Exists(HapticsConfigFile)) return;
        var root = new JsonObject {
            ["enabled"] = true,
            ["defaultGain"] = 1.0,
            ["dumpEnabled"] = false,
            ["useBuiltinDefaults"] = true,
            ["effects"] = new JsonObject()
        };
        File.WriteAllText(HapticsConfigFile, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
    }

    Panel MakePanel(string title, int x, int y, int w, int h)
    {
        var p = new Panel { Location = new Point(x, y), Size = new Size(w, h), BackColor = PANEL };
        p.Paint += (s, e) => {
            using var pen = new Pen(Color.FromArgb(60, 63, 72));
            e.Graphics.DrawRectangle(pen, 0, 0, p.Width - 1, p.Height - 1);
        };
        var t = new Label { Location = new Point(12, 6), AutoSize = true, Font = new Font("Microsoft YaHei UI", 9f, FontStyle.Bold),
            ForeColor = Color.FromArgb(150, 200, 255) };
        Reg(t, title);
        p.Controls.Add(t);
        return p;
    }

    Label MakeStatusLine(Panel parent, int y)
    {
        var l = new Label { Location = new Point(14, y), Size = new Size(parent.Width - 28, 20), ForeColor = MUTE, Text = "…" };
        parent.Controls.Add(l);
        return l;
    }

    Button MakeButton(string text, int x, int y, int w, int h = 28, bool accent = false)
    {
        var b = new Button {
            Text = text, Location = new Point(x, y), Size = new Size(w, h),
            FlatStyle = FlatStyle.Flat, ForeColor = FG,
            BackColor = accent ? Color.FromArgb(52, 108, 170) : Color.FromArgb(56, 60, 70),
            UseVisualStyleBackColor = false, Cursor = Cursors.Hand
        };
        b.FlatAppearance.BorderColor = Color.FromArgb(80, 84, 94);
        return b;
    }

    // ============================ 状态刷新 ============================
    void RefreshStatus()
    {
        string dir = txtGamePath.Text.Trim();
        bool proxyBundled = File.Exists(ProxySrc);
        bool installed = IsInstalled(dir);
        bool gameRunning = Process.GetProcessesByName("sekiro").Length > 0;
        bool iconModInstalled = IsIconModInstalled(dir);
        bool iconModBundled = Directory.Exists(IconModSrcDir);

        bool pad = false;
        try { pad = DetectDualSense(); } catch { }

        SetDot(lblStProxy, installed, installed ? Tr("代理已安装到游戏 ✓") :
            (Directory.Exists(dir) ? Tr("代理未安装（点\"安装到游戏\"）") : Tr("先选择游戏目录")));
        SetDot(lblStGame, gameRunning ? (bool?)true : null,
            gameRunning ? Tr("只狼正在运行") : Tr("只狼未运行"));
        SetDot(lblStPad, pad, pad ? Tr("DualSense 已连接（音频设备）") : Tr("未检测到 DualSense（请 USB 连接）"));
        SetDot(lblStIconMod, iconModInstalled ? (bool?)true : null,
            iconModInstalled ? Tr("PS5 按键图标已安装") : Tr("PS5 按键图标未安装（可选）"));

        // 按钮可用性
        btnInstall.Enabled = proxyBundled && Directory.Exists(dir) && !gameRunning && !installed;
        btnUninstall.Enabled = installed && !gameRunning;
        btnInstallIconMod.Enabled = iconModBundled && Directory.Exists(dir) && !gameRunning && !iconModInstalled;
        btnUninstallIconMod.Enabled = iconModInstalled && !gameRunning;

        if (!proxyBundled) Msg(Tr("⚠ 找不到打包的代理 DLL（proxy\\fmodex64.dll）。请先编译 fmod_probe。"), BAD);
        else if (gameRunning && !installed) Msg(Tr("只狼运行中，安装/卸载需先关闭游戏。"), MUTE);
    }

    void SetDot(Label lbl, bool? ok, string text)
    {
        Color c = ok == true ? OK : ok == false ? BAD : MUTE;
        lbl.ForeColor = c;
        lbl.Text = (ok == true ? "● " : ok == false ? "● " : "○ ") + text;
    }

    void Msg(string text, Color color) { lblMsg.ForeColor = color; lblMsg.Text = text; }

    // ============================ 安装/卸载 ============================
    bool IsInstalled(string dir)
    {
        try {
            if (!Directory.Exists(dir) || !File.Exists(ProxySrc)) return false;
            string gameDll = Path.Combine(dir, "fmodex64.dll");
            string orig = Path.Combine(dir, "fmodex64_orig.dll");
            if (!File.Exists(gameDll) || !File.Exists(orig)) return false;
            return new FileInfo(gameDll).Length == new FileInfo(ProxySrc).Length;
        } catch { return false; }
    }

    void DoInstall()
    {
        string dir = txtGamePath.Text.Trim();
        try {
            if (Process.GetProcessesByName("sekiro").Length > 0) { Msg(Tr("请先关闭只狼再安装。"), BAD); return; }
            if (!File.Exists(ProxySrc)) { Msg(Tr("找不到代理 DLL。"), BAD); return; }
            string gameDll = Path.Combine(dir, "fmodex64.dll");
            string orig = Path.Combine(dir, "fmodex64_orig.dll");
            string backup = Path.Combine(dir, "fmodex64.dll.backup_original");

            if (!File.Exists(orig))
            {
                if (!File.Exists(gameDll)) { Msg(Tr("游戏目录里没有 fmodex64.dll，路径可能不对。"), BAD); return; }
                long sz = new FileInfo(gameDll).Length;
                if (sz < 500 * 1024) {
                    if (MessageBox.Show(Tr("当前 fmodex64.dll 偏小，可能不是原版引擎（或已被改过）。仍要继续吗？"),
                        Tr("确认"), MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
                }
                File.Copy(gameDll, orig);                       // 备份真引擎为 _orig
                if (!File.Exists(backup)) File.Copy(gameDll, backup);
            }
            File.Copy(ProxySrc, gameDll, true);                 // 覆盖为我们的代理
            Msg(Tr("✓ 安装成功！用 Steam Input 启动只狼，USB 接 DualSense 即可。"), OK);
        }
        catch (UnauthorizedAccessException) { Msg(Tr("权限不足：请右键\"以管理员身份运行\"本程序后重试。"), BAD); }
        catch (Exception ex) { Msg(Tr("安装失败：") + ex.Message, BAD); }
        RefreshStatus();
    }

    void DoUninstall()
    {
        string dir = txtGamePath.Text.Trim();
        try {
            if (Process.GetProcessesByName("sekiro").Length > 0) { Msg(Tr("请先关闭只狼再卸载。"), BAD); return; }
            string gameDll = Path.Combine(dir, "fmodex64.dll");
            string orig = Path.Combine(dir, "fmodex64_orig.dll");
            if (!File.Exists(orig)) { Msg(Tr("找不到备份 fmodex64_orig.dll，无法自动还原。"), BAD); return; }
            if (File.Exists(gameDll)) File.Delete(gameDll);
            File.Move(orig, gameDll);                            // 还原真引擎
            Msg(Tr("✓ 已卸载，游戏恢复原状。"), OK);
        }
        catch (UnauthorizedAccessException) { Msg(Tr("权限不足：请以管理员身份运行后重试。"), BAD); }
        catch (Exception ex) { Msg(Tr("卸载失败：") + ex.Message, BAD); }
        RefreshStatus();
    }

    // ============================ 设备检测 ============================
    static bool DetectDualSense()
    {
        using var en = new MMDeviceEnumerator();
        foreach (var d in en.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active))
        {
            if ((d.FriendlyName ?? "").IndexOf("DualSense", StringComparison.OrdinalIgnoreCase) >= 0) return true;
        }
        return false;
    }

    // ============================ PS5 按键图标模组（可选，第三方） ============================
    bool IsIconModInstalled(string dir)
    {
        try {
            if (!Directory.Exists(dir)) return false;
            return File.Exists(Path.Combine(dir, "dinput8.dll")) &&
                   File.Exists(Path.Combine(dir, "mods", "menu", "hi", "01_common.tpf.dcx"));
        } catch { return false; }
    }

    void DoInstallIconMod()
    {
        string dir = txtGamePath.Text.Trim();
        try {
            if (Process.GetProcessesByName("sekiro").Length > 0) { Msg(Tr("请先关闭只狼再安装图标模组。"), BAD); return; }
            if (!Directory.Exists(IconModSrcDir)) { Msg(Tr("找不到打包的图标模组素材（iconmod\\）。"), BAD); return; }
            if (!Directory.Exists(dir)) { Msg(Tr("请先选择只狼游戏目录。"), BAD); return; }
            foreach (string src in Directory.GetFiles(IconModSrcDir, "*", SearchOption.AllDirectories))
            {
                string rel = Path.GetRelativePath(IconModSrcDir, src);
                if (rel.Equals("CREDITS.txt", StringComparison.OrdinalIgnoreCase)) continue; // 署名文件留在安装包里，不用拷进游戏目录
                string dest = Path.Combine(dir, rel);
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                File.Copy(src, dest, true);
            }
            Msg(Tr("✓ PS5 图标模组已安装（Mod Engine + PS5 Glyphs），下次启动只狼生效。"), OK);
        }
        catch (UnauthorizedAccessException) { Msg(Tr("权限不足：请以管理员身份运行后重试。"), BAD); }
        catch (Exception ex) { Msg(Tr("安装图标模组失败：") + ex.Message, BAD); }
        RefreshStatus();
    }

    void DoUninstallIconMod()
    {
        string dir = txtGamePath.Text.Trim();
        try {
            if (Process.GetProcessesByName("sekiro").Length > 0) { Msg(Tr("请先关闭只狼再卸载图标模组。"), BAD); return; }
            string dinput = Path.Combine(dir, "dinput8.dll");
            if (File.Exists(dinput)) File.Delete(dinput);
            // modengine.ini 和 mods 文件夹留着不影响原版（下次重装 dinput8.dll 会自动认得），不强制删
            Msg(Tr("✓ 已卸载图标模组（删掉 dinput8.dll 即可，游戏恢复 Xbox 图标）。"), OK);
        }
        catch (UnauthorizedAccessException) { Msg(Tr("权限不足：请以管理员身份运行后重试。"), BAD); }
        catch (Exception ex) { Msg(Tr("卸载图标模组失败：") + ex.Message, BAD); }
        RefreshStatus();
    }

    // ============================ 游戏路径 ============================
    void BrowseGame()
    {
        using var fbd = new FolderBrowserDialog { Description = Tr("选择只狼安装目录（包含 sekiro.exe）") };
        if (Directory.Exists(txtGamePath.Text)) fbd.SelectedPath = txtGamePath.Text;
        if (fbd.ShowDialog() == DialogResult.OK) {
            string dir = fbd.SelectedPath;
            if (!File.Exists(Path.Combine(dir, "sekiro.exe")))
                Msg(Tr("提醒：该目录下没找到 sekiro.exe，请确认选对了。"), BAD);
            txtGamePath.Text = dir; SaveGamePath(dir); RefreshStatus();
        }
    }

    static string? AutoDetectGame()
    {
        var cands = new System.Collections.Generic.List<string>();
        try {
            using var k = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
            if (k?.GetValue("SteamPath") is string steam) {
                cands.Add(Path.Combine(steam, "steamapps", "common", "Sekiro"));
                string vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
                if (File.Exists(vdf))
                    foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s*\"(.+?)\""))
                        cands.Add(Path.Combine(m.Groups[1].Value.Replace(@"\\", @"\"), "steamapps", "common", "Sekiro"));
            }
        } catch { }
        cands.Add(@"C:\Program Files (x86)\Steam\steamapps\common\Sekiro");
        foreach (var drive in DriveInfo.GetDrives())
            try { if (drive.IsReady) cands.Add(Path.Combine(drive.Name, "SteamLibrary", "steamapps", "common", "Sekiro")); } catch { }
        foreach (var c in cands)
            try { if (File.Exists(Path.Combine(c, "sekiro.exe"))) return c; } catch { }
        return null;
    }

    string? LoadGamePath()
    {
        try { if (File.Exists(SettingsFile)) { var s = File.ReadAllText(SettingsFile).Trim(); if (Directory.Exists(s)) return s; } } catch { }
        return null;
    }
    void SaveGamePath(string dir)
    {
        try { Directory.CreateDirectory(Path.GetDirectoryName(SettingsFile)!); File.WriteAllText(SettingsFile, dir); } catch { }
    }
}
