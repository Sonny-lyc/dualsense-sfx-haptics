using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
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
    Label lblStProxy = null!, lblStGame = null!, lblStPad = null!, lblStCable = null!;
    Button btnInstall = null!, btnUninstall = null!;
    RadioButton rbDirect = null!, rbCable = null!;
    Label lblMsg = null!;
    System.Windows.Forms.Timer statusTimer = null!;

    bool _loading = true;
    string? _cableId = null;

    string ProxySrc => Path.Combine(AppContext.BaseDirectory, "proxy", "fmodex64.dll");
    string SettingsFile => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "DualSenseSfxHaptics", "settings.txt");
    string TargetFile => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "haptic_target.txt");

    public MainForm()
    {
        Text = "DualSense 音效触觉  v0.1";
        BackColor = BG;
        ForeColor = FG;
        Font = new Font("Microsoft YaHei UI", 9.5f);
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(500, 590);

        BuildUi();

        _loading = true;
        txtGamePath.Text = LoadGamePath() ?? AutoDetectGame() ?? "";
        LoadOutputSelection();
        _loading = false;

        statusTimer = new System.Windows.Forms.Timer { Interval = 2000 };
        statusTimer.Tick += (s, e) => RefreshStatus();
        statusTimer.Start();
        RefreshStatus();
    }

    // ============================ UI ============================
    void BuildUi()
    {
        int x = 20, w = 460, y = 16;

        var title = new Label {
            Text = "DualSense 音效触觉",
            Font = new Font("Microsoft YaHei UI", 15f, FontStyle.Bold),
            ForeColor = FG, AutoSize = false, Location = new Point(x, y), Size = new Size(w, 32)
        };
        Controls.Add(title);
        var sub = new Label {
            Text = "只把《只狼》的音效变成手柄细腻触觉（音乐/语音不震）",
            ForeColor = MUTE, Location = new Point(x, y + 34), Size = new Size(w, 20)
        };
        Controls.Add(sub);
        y += 66;

        // ---- 游戏路径 ----
        var gp = MakePanel("游戏目录（只狼 Sekiro）", x, y, w, 92);
        Controls.Add(gp);
        txtGamePath = new TextBox {
            Location = new Point(14, 30), Size = new Size(gp.Width - 28, 26),
            BackColor = Color.FromArgb(28, 30, 36), ForeColor = FG, BorderStyle = BorderStyle.FixedSingle,
            ReadOnly = true
        };
        gp.Controls.Add(txtGamePath);
        var btnBrowse = MakeButton("浏览…", 14, 60, 90);
        btnBrowse.Click += (s, e) => BrowseGame();
        gp.Controls.Add(btnBrowse);
        var btnAuto = MakeButton("自动检测", 112, 60, 90);
        btnAuto.Click += (s, e) => { var g = AutoDetectGame(); if (g != null) { txtGamePath.Text = g; SaveGamePath(g); RefreshStatus(); } else Msg("没自动找到只狼，请点\"浏览\"手动选游戏目录", BAD); };
        gp.Controls.Add(btnAuto);
        y += 104;

        // ---- 状态 ----
        var sp = MakePanel("状态", x, y, w, 118);
        Controls.Add(sp);
        lblStProxy = MakeStatusLine(sp, 30);
        lblStGame = MakeStatusLine(sp, 52);
        lblStPad = MakeStatusLine(sp, 74);
        lblStCable = MakeStatusLine(sp, 96);
        y += 130;

        // ---- 安装按钮 ----
        btnInstall = MakeButton("安装到游戏", x, y, 222, 40, accent: true);
        btnInstall.Click += (s, e) => DoInstall();
        Controls.Add(btnInstall);
        btnUninstall = MakeButton("卸载 / 还原", x + 238, y, 222, 40);
        btnUninstall.Click += (s, e) => DoUninstall();
        Controls.Add(btnUninstall);
        y += 54;

        // ---- 输出 ----
        var op = MakePanel("触觉输出", x, y, w, 96);
        Controls.Add(op);
        rbDirect = new RadioButton {
            Text = "DualSense 手柄 ch3/4 直驱（推荐，无需额外软件）",
            Location = new Point(14, 28), Size = new Size(op.Width - 28, 24), ForeColor = FG
        };
        rbDirect.CheckedChanged += (s, e) => OnOutputChanged();
        op.Controls.Add(rbDirect);
        rbCable = new RadioButton {
            Text = "虚拟声卡 → DSX（需已安装 VB-CABLE）",
            Location = new Point(14, 56), Size = new Size(op.Width - 28, 24), ForeColor = FG
        };
        rbCable.CheckedChanged += (s, e) => OnOutputChanged();
        op.Controls.Add(rbCable);
        y += 108;

        // ---- 消息 ----
        lblMsg = new Label {
            Location = new Point(x, y), Size = new Size(w, 40), ForeColor = MUTE,
            TextAlign = ContentAlignment.TopLeft
        };
        Controls.Add(lblMsg);
    }

    Panel MakePanel(string title, int x, int y, int w, int h)
    {
        var p = new Panel { Location = new Point(x, y), Size = new Size(w, h), BackColor = PANEL };
        p.Paint += (s, e) => {
            using var pen = new Pen(Color.FromArgb(60, 63, 72));
            e.Graphics.DrawRectangle(pen, 0, 0, p.Width - 1, p.Height - 1);
        };
        var t = new Label { Text = title, ForeColor = Color.FromArgb(150, 200, 255),
            Location = new Point(12, 6), AutoSize = true, Font = new Font("Microsoft YaHei UI", 9f, FontStyle.Bold) };
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

        (bool pad, bool cable, string? cableId) dev = (false, false, null);
        try { dev = DetectDevices(); } catch { }
        _cableId = dev.cableId;

        SetDot(lblStProxy, installed, installed ? "代理已安装到游戏 ✓" :
            (Directory.Exists(dir) ? "代理未安装（点\"安装到游戏\"）" : "先选择游戏目录"));
        SetDot(lblStGame, gameRunning ? (bool?)true : null,
            gameRunning ? "只狼正在运行" : "只狼未运行");
        SetDot(lblStPad, dev.pad, dev.pad ? "DualSense 已连接（音频设备）" : "未检测到 DualSense（请 USB 连接）");
        SetDot(lblStCable, dev.cable ? (bool?)true : null,
            dev.cable ? "检测到 VB-CABLE（可选 DSX 路线）" : "未检测到 VB-CABLE（用直驱即可）");

        // 按钮/单选可用性
        btnInstall.Enabled = proxyBundled && Directory.Exists(dir) && !gameRunning && !installed;
        btnUninstall.Enabled = installed && !gameRunning;
        rbCable.Enabled = dev.cable;
        if (!dev.cable && rbCable.Checked) { _loading = true; rbDirect.Checked = true; _loading = false; }

        if (!proxyBundled) Msg("⚠ 找不到打包的代理 DLL（proxy\\fmodex64.dll）。请先编译 fmod_probe。", BAD);
        else if (gameRunning && !installed) Msg("只狼运行中，安装/卸载需先关闭游戏。", MUTE);
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
            if (Process.GetProcessesByName("sekiro").Length > 0) { Msg("请先关闭只狼再安装。", BAD); return; }
            if (!File.Exists(ProxySrc)) { Msg("找不到代理 DLL。", BAD); return; }
            string gameDll = Path.Combine(dir, "fmodex64.dll");
            string orig = Path.Combine(dir, "fmodex64_orig.dll");
            string backup = Path.Combine(dir, "fmodex64.dll.backup_original");

            if (!File.Exists(orig))
            {
                if (!File.Exists(gameDll)) { Msg("游戏目录里没有 fmodex64.dll，路径可能不对。", BAD); return; }
                long sz = new FileInfo(gameDll).Length;
                if (sz < 500 * 1024) {
                    if (MessageBox.Show("当前 fmodex64.dll 偏小，可能不是原版引擎（或已被改过）。仍要继续吗？",
                        "确认", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
                }
                File.Copy(gameDll, orig);                       // 备份真引擎为 _orig
                if (!File.Exists(backup)) File.Copy(gameDll, backup);
            }
            File.Copy(ProxySrc, gameDll, true);                 // 覆盖为我们的代理
            Msg("✓ 安装成功！用 Steam Input 启动只狼，USB 接 DualSense 即可。", OK);
        }
        catch (UnauthorizedAccessException) { Msg("权限不足：请右键\"以管理员身份运行\"本程序后重试。", BAD); }
        catch (Exception ex) { Msg("安装失败：" + ex.Message, BAD); }
        RefreshStatus();
    }

    void DoUninstall()
    {
        string dir = txtGamePath.Text.Trim();
        try {
            if (Process.GetProcessesByName("sekiro").Length > 0) { Msg("请先关闭只狼再卸载。", BAD); return; }
            string gameDll = Path.Combine(dir, "fmodex64.dll");
            string orig = Path.Combine(dir, "fmodex64_orig.dll");
            if (!File.Exists(orig)) { Msg("找不到备份 fmodex64_orig.dll，无法自动还原。", BAD); return; }
            if (File.Exists(gameDll)) File.Delete(gameDll);
            File.Move(orig, gameDll);                            // 还原真引擎
            Msg("✓ 已卸载，游戏恢复原状。", OK);
        }
        catch (UnauthorizedAccessException) { Msg("权限不足：请以管理员身份运行后重试。", BAD); }
        catch (Exception ex) { Msg("卸载失败：" + ex.Message, BAD); }
        RefreshStatus();
    }

    // ============================ 输出切换 ============================
    void OnOutputChanged()
    {
        if (_loading) return;
        try {
            string content = rbCable.Checked && _cableId != null ? _cableId : "DualSense";
            File.WriteAllText(TargetFile, content);
            Msg(rbCable.Checked
                ? "输出：虚拟声卡 → DSX。（游戏运行中需重开生效）"
                : "输出：DualSense ch3/4 直驱。（游戏运行中需重开生效）", MUTE);
        } catch (Exception ex) { Msg("写输出配置失败：" + ex.Message, BAD); }
    }

    void LoadOutputSelection()
    {
        try {
            if (File.Exists(TargetFile)) {
                string t = File.ReadAllText(TargetFile).Trim();
                if (t.StartsWith("{")) { rbCable.Checked = true; return; }
            }
        } catch { }
        rbDirect.Checked = true;
    }

    // ============================ 设备检测 ============================
    static (bool pad, bool cable, string? cableId) DetectDevices()
    {
        bool pad = false, cable = false; string? cableId = null;
        using var en = new MMDeviceEnumerator();
        foreach (var d in en.EnumerateAudioEndPoints(DataFlow.Render, DeviceState.Active))
        {
            string name = d.FriendlyName ?? "";
            if (name.IndexOf("DualSense", StringComparison.OrdinalIgnoreCase) >= 0) pad = true;
            if (name.IndexOf("CABLE Input", StringComparison.OrdinalIgnoreCase) >= 0) { cable = true; cableId = d.ID; }
        }
        return (pad, cable, cableId);
    }

    // ============================ 游戏路径 ============================
    void BrowseGame()
    {
        using var fbd = new FolderBrowserDialog { Description = "选择只狼安装目录（包含 sekiro.exe）" };
        if (Directory.Exists(txtGamePath.Text)) fbd.SelectedPath = txtGamePath.Text;
        if (fbd.ShowDialog() == DialogResult.OK) {
            string dir = fbd.SelectedPath;
            if (!File.Exists(Path.Combine(dir, "sekiro.exe")))
                Msg("提醒：该目录下没找到 sekiro.exe，请确认选对了。", BAD);
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
