using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
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
    Label lblStProxy = null!, lblStGame = null!, lblStPad = null!, lblStCable = null!;
    Button btnInstall = null!, btnUninstall = null!;
    RadioButton rbDirect = null!, rbCable = null!;
    Button btnInstallCable = null!;
    CheckBox chkGlobalEnabled = null!, chkUseBuiltinDefaults = null!, chkDumpEnabled = null!;
    NumericUpDown numDefaultGain = null!;
    DataGridView gridEffects = null!;
    Button btnReloadEffects = null!, btnSaveEffects = null!, btnOpenDumpDir = null!, btnPlayDump = null!;
    Button btnReviewMode = null!, btnEnableSelected = null!, btnDisableSelected = null!;
    Panel panelEffects = null!;
    Label lblMsg = null!;
    System.Windows.Forms.Timer statusTimer = null!;

    bool _loading = true;
    string? _cableId = null;

    string ProxySrc => Path.Combine(AppContext.BaseDirectory, "proxy", "fmodex64.dll");
    string AppDir => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "DualSenseSfxHaptics");
    string SettingsFile => Path.Combine(AppDir, "settings.txt");
    string HapticsConfigFile => Path.Combine(AppDir, "haptics.json");
    string SeenEffectsFile => Path.Combine(AppDir, "seen_effects.jsonl");
    string DumpDir => Path.Combine(AppDir, "dumps");
    string TargetFile => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "haptic_target.txt");

    public MainForm()
    {
        Text = "DualSense 音效触觉  v0.1";
        BackColor = BG;
        ForeColor = FG;
        Font = new Font("Microsoft YaHei UI", 9.5f);
        FormBorderStyle = FormBorderStyle.Sizable;
        MaximizeBox = true;
        MinimumSize = new Size(820, 760);
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(960, 900);

        BuildUi();

        _loading = true;
        txtGamePath.Text = LoadGamePath() ?? AutoDetectGame() ?? "";
        LoadOutputSelection();
        LoadEffectsConfig();
        _loading = false;

        statusTimer = new System.Windows.Forms.Timer { Interval = 2000 };
        statusTimer.Tick += (s, e) => RefreshStatus();
        statusTimer.Start();
        RefreshStatus();
    }

    // ============================ UI ============================
    void BuildUi()
    {
        int x = 20, w = ClientSize.Width - 40, y = 16;

        var title = new Label {
            Text = "DualSense 音效触觉",
            Font = new Font("Microsoft YaHei UI", 15f, FontStyle.Bold),
            ForeColor = FG, AutoSize = false, Location = new Point(x, y), Size = new Size(w, 32),
            Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
        };
        Controls.Add(title);
        var sub = new Label {
            Text = "只把《只狼》的音效变成手柄细腻触觉（音乐/语音不震）",
            ForeColor = MUTE, Location = new Point(x, y + 34), Size = new Size(w, 20),
            Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
        };
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
        var btnBrowse = MakeButton("浏览…", 14, 60, 90);
        btnBrowse.Click += (s, e) => BrowseGame();
        gp.Controls.Add(btnBrowse);
        var btnAuto = MakeButton("自动检测", 112, 60, 90);
        btnAuto.Click += (s, e) => { var g = AutoDetectGame(); if (g != null) { txtGamePath.Text = g; SaveGamePath(g); RefreshStatus(); } else Msg("没自动找到只狼，请点\"浏览\"手动选游戏目录", BAD); };
        gp.Controls.Add(btnAuto);
        y += 104;

        // ---- 状态 ----
        var sp = MakePanel("状态", x, y, w, 118);
        sp.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(sp);
        lblStProxy = MakeStatusLine(sp, 30);
        lblStGame = MakeStatusLine(sp, 52);
        lblStPad = MakeStatusLine(sp, 74);
        lblStCable = MakeStatusLine(sp, 96);
        y += 130;

        // ---- 安装按钮 ----
        btnInstall = MakeButton("安装到游戏", x, y, 222, 40, accent: true);
        btnInstall.Click += (s, e) => DoInstall();
        btnInstall.Anchor = AnchorStyles.Top | AnchorStyles.Left;
        Controls.Add(btnInstall);
        btnUninstall = MakeButton("卸载 / 还原", x + 238, y, 222, 40);
        btnUninstall.Click += (s, e) => DoUninstall();
        btnUninstall.Anchor = AnchorStyles.Top | AnchorStyles.Left;
        Controls.Add(btnUninstall);
        y += 54;

        // ---- 输出 ----
        var op = MakePanel("触觉输出", x, y, w, 128);
        op.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(op);
        rbCable = new RadioButton {
            Text = "虚拟声卡 → DSX   ⭐ 推荐·手感最佳（需 VB-CABLE + DSX）",
            Location = new Point(14, 28), Size = new Size(op.Width - 28, 24), ForeColor = FG,
            Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
        };
        rbCable.CheckedChanged += (s, e) => OnOutputChanged();
        op.Controls.Add(rbCable);
        rbDirect = new RadioButton {
            Text = "DualSense ch3/4 直驱（省事，但弹刀等高频音较弱）",
            Location = new Point(14, 56), Size = new Size(op.Width - 28, 24), ForeColor = FG,
            Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
        };
        rbDirect.CheckedChanged += (s, e) => OnOutputChanged();
        op.Controls.Add(rbDirect);
        btnInstallCable = MakeButton("↓ 一键下载安装 VB-CABLE（免费虚拟声卡）", 14, 90, op.Width - 28, 28, accent: true);
        btnInstallCable.Click += async (s, e) => await InstallCable();
        btnInstallCable.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        op.Controls.Add(btnInstallCable);
        y += 140;

        // ---- 音效管理 ----
        panelEffects = MakePanel("音效管理（重启游戏生效）", x, y, w, ClientSize.Height - y - 72);
        panelEffects.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        Controls.Add(panelEffects);
        chkGlobalEnabled = new CheckBox {
            Text = "总开关", Location = new Point(14, 28), Size = new Size(78, 22), ForeColor = FG, Checked = true
        };
        panelEffects.Controls.Add(chkGlobalEnabled);
        chkUseBuiltinDefaults = new CheckBox {
            Text = "内置白名单", Location = new Point(104, 28), Size = new Size(100, 22), ForeColor = FG, Checked = true
        };
        panelEffects.Controls.Add(chkUseBuiltinDefaults);
        chkDumpEnabled = new CheckBox {
            Text = "录遇到的所有音效", Location = new Point(216, 28), Size = new Size(138, 22), ForeColor = FG
        };
        panelEffects.Controls.Add(chkDumpEnabled);
        var gainLabel = new Label { Text = "默认强度", Location = new Point(364, 31), Size = new Size(68, 20), ForeColor = MUTE };
        panelEffects.Controls.Add(gainLabel);
        numDefaultGain = new NumericUpDown {
            Location = new Point(434, 28), Size = new Size(64, 24), DecimalPlaces = 1, Increment = 0.1M,
            Minimum = 0, Maximum = 8, Value = 1.0M, BackColor = Color.FromArgb(28, 30, 36), ForeColor = FG
        };
        panelEffects.Controls.Add(numDefaultGain);
        btnReloadEffects = MakeButton("刷新列表", panelEffects.Width - 214, 25, 86);
        btnReloadEffects.Click += (s, e) => LoadEffectsConfig();
        btnReloadEffects.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        panelEffects.Controls.Add(btnReloadEffects);
        btnSaveEffects = MakeButton("保存配置", panelEffects.Width - 120, 25, 106, accent: true);
        btnSaveEffects.Click += (s, e) => SaveEffectsConfig();
        btnSaveEffects.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        panelEffects.Controls.Add(btnSaveEffects);

        btnPlayDump = MakeButton("试听录音", 14, 56, 92);
        btnPlayDump.Click += (s, e) => PlaySelectedDump();
        panelEffects.Controls.Add(btnPlayDump);
        btnEnableSelected = MakeButton("启用选中", 116, 56, 92);
        btnEnableSelected.Click += (s, e) => SetSelectedEnabled(true);
        panelEffects.Controls.Add(btnEnableSelected);
        btnDisableSelected = MakeButton("禁用选中", 218, 56, 92);
        btnDisableSelected.Click += (s, e) => SetSelectedEnabled(false);
        panelEffects.Controls.Add(btnDisableSelected);
        btnReviewMode = MakeButton("试听标注模式", 320, 56, 118, accent: true);
        btnReviewMode.Click += (s, e) => ApplyReviewMode();
        panelEffects.Controls.Add(btnReviewMode);
        btnOpenDumpDir = MakeButton("打开录音目录", 448, 56, 112);
        btnOpenDumpDir.Click += (s, e) => OpenDumpDir();
        panelEffects.Controls.Add(btnOpenDumpDir);
        var hint = new Label {
            Text = "试听标注模式：只录音不震动；听到想要的 WAV 后勾启用/调强度，保存并重启游戏。同一分组只保留最新 WAV。",
            Location = new Point(14, 88), Size = new Size(panelEffects.Width - 28, 20), ForeColor = MUTE,
            Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
        };
        panelEffects.Controls.Add(hint);

        gridEffects = new DataGridView {
            Location = new Point(14, 112), Size = new Size(panelEffects.Width - 28, panelEffects.Height - 126),
            AllowUserToAddRows = false, AllowUserToDeleteRows = false, RowHeadersVisible = false,
            SelectionMode = DataGridViewSelectionMode.FullRowSelect, MultiSelect = false,
            BackgroundColor = Color.FromArgb(28, 30, 36), BorderStyle = BorderStyle.FixedSingle,
            EnableHeadersVisualStyles = false, AutoSizeRowsMode = DataGridViewAutoSizeRowsMode.None,
            Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right,
            AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.None
        };
        gridEffects.ColumnHeadersDefaultCellStyle.BackColor = Color.FromArgb(48, 52, 62);
        gridEffects.ColumnHeadersDefaultCellStyle.ForeColor = FG;
        gridEffects.DefaultCellStyle.BackColor = Color.FromArgb(28, 30, 36);
        gridEffects.DefaultCellStyle.ForeColor = FG;
        gridEffects.DefaultCellStyle.SelectionBackColor = Color.FromArgb(52, 108, 170);
        gridEffects.DefaultCellStyle.SelectionForeColor = Color.White;
        gridEffects.Columns.Add(new DataGridViewTextBoxColumn { Name = "colIdx", HeaderText = "idx", Width = 54, ReadOnly = true });
        gridEffects.Columns.Add(new DataGridViewTextBoxColumn { Name = "colGroup", HeaderText = "分组", Width = 138, ReadOnly = true });
        gridEffects.Columns.Add(new DataGridViewCheckBoxColumn { Name = "colEnabled", HeaderText = "启用", Width = 52 });
        gridEffects.Columns.Add(new DataGridViewTextBoxColumn { Name = "colGain", HeaderText = "强度", Width = 58 });
        gridEffects.Columns.Add(new DataGridViewCheckBoxColumn { Name = "colDump", HeaderText = "录音", Width = 52 });
        gridEffects.Columns.Add(new DataGridViewTextBoxColumn { Name = "colMeaning", HeaderText = "名称", Width = 150 });
        gridEffects.Columns.Add(new DataGridViewTextBoxColumn { Name = "colBank", HeaderText = "bank", Width = 118, ReadOnly = true });
        gridEffects.Columns.Add(new DataGridViewTextBoxColumn { Name = "colVerdict", HeaderText = "分类", Width = 100, ReadOnly = true });
        gridEffects.Columns.Add(new DataGridViewTextBoxColumn { Name = "colSeen", HeaderText = "发现时间", Width = 140, ReadOnly = true });
        panelEffects.Controls.Add(gridEffects);
        y += panelEffects.Height + 12;

        // ---- 消息 ----
        lblMsg = new Label {
            Location = new Point(x, y), Size = new Size(w, 40), ForeColor = MUTE,
            TextAlign = ContentAlignment.TopLeft,
            Anchor = AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right
        };
        Controls.Add(lblMsg);
    }

    // ============================ 音效配置 ============================
    sealed class EffectRow
    {
        public int Idx;
        public string Group = "";
        public string Bank = "";
        public string Meaning = "";
        public string Verdict = "";
        public string LastSeen = "";
        public bool Enabled;
        public decimal Gain = 1.0M;
        public bool Dump;
    }

    sealed class EffectConfig
    {
        public bool HasEnabled;
        public bool Enabled;
        public decimal Gain = 1.0M;
        public bool Dump;
        public string Name = "";
    }

    void EnsureDefaultHapticsConfig()
    {
        Directory.CreateDirectory(AppDir);
        Directory.CreateDirectory(DumpDir);
        if (File.Exists(HapticsConfigFile)) return;
        var root = new JsonObject {
            ["enabled"] = true,
            ["defaultGain"] = 1.0,
                ["dumpEnabled"] = true,
                ["useBuiltinDefaults"] = false,
            ["effects"] = new JsonObject()
        };
        File.WriteAllText(HapticsConfigFile, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
    }

    void LoadEffectsConfig()
    {
        try
        {
            EnsureDefaultHapticsConfig();
            var rows = new Dictionary<int, EffectRow>();
            var configured = LoadConfiguredEffects(out bool enabled, out bool useBuiltin, out bool dumpEnabled, out decimal defaultGain);

            SeedKnownEffects(rows, useBuiltin);
            LoadSeenEffects(rows);

            chkGlobalEnabled.Checked = enabled;
            chkUseBuiltinDefaults.Checked = useBuiltin;
            chkDumpEnabled.Checked = dumpEnabled;
            numDefaultGain.Value = ClampDecimal(defaultGain, numDefaultGain.Minimum, numDefaultGain.Maximum);

            foreach (var kv in configured)
            {
                if (!rows.TryGetValue(kv.Key, out var row))
                {
                    row = new EffectRow { Idx = kv.Key, Group = GroupKey(kv.Key), Meaning = kv.Value.Name, Enabled = kv.Value.HasEnabled ? kv.Value.Enabled : useBuiltin && IsKnownDefaultIdx(kv.Key), Verdict = "config" };
                    rows[kv.Key] = row;
                }
                if (string.IsNullOrWhiteSpace(row.Group)) row.Group = GroupKey(row.Idx);
                row.Enabled = kv.Value.HasEnabled ? kv.Value.Enabled : row.Enabled;
                row.Gain = kv.Value.Gain;
                row.Dump = kv.Value.Dump;
                if (!string.IsNullOrWhiteSpace(kv.Value.Name)) row.Meaning = kv.Value.Name;
            }

            gridEffects.Rows.Clear();
            foreach (var row in rows.Values.OrderBy(r => r.Idx))
            {
                int r = gridEffects.Rows.Add(row.Idx, row.Group, row.Enabled, row.Gain.ToString("0.0", CultureInfo.InvariantCulture), row.Dump,
                    row.Meaning, row.Bank, row.Verdict, row.LastSeen);
                if (!row.Enabled) gridEffects.Rows[r].DefaultCellStyle.ForeColor = MUTE;
            }
            Msg($"已加载 {rows.Count} 个音效；配置保存在 {HapticsConfigFile}", MUTE);
        }
        catch (Exception ex) { Msg("读取音效配置失败：" + ex.Message, BAD); }
    }

    Dictionary<int, EffectConfig> LoadConfiguredEffects(out bool enabled, out bool useBuiltin, out bool dumpEnabled, out decimal defaultGain)
    {
        enabled = true; useBuiltin = false; dumpEnabled = true; defaultGain = 1.0M;
        var map = new Dictionary<int, EffectConfig>();
        var root = JsonNode.Parse(File.ReadAllText(HapticsConfigFile)) as JsonObject;
        if (root == null) return map;
        enabled = root["enabled"]?.GetValue<bool>() ?? enabled;
        useBuiltin = root["useBuiltinDefaults"]?.GetValue<bool>() ?? useBuiltin;
        dumpEnabled = root["dumpEnabled"]?.GetValue<bool>() ?? dumpEnabled;
        defaultGain = ClampDecimal((decimal)(root["defaultGain"]?.GetValue<double>() ?? 1.0), 0, 8);
        if (root["effects"] is not JsonObject effects) return map;
        foreach (var kv in effects)
        {
            if (!int.TryParse(kv.Key, NumberStyles.Integer, CultureInfo.InvariantCulture, out int idx)) continue;
            if (kv.Value is not JsonObject obj) continue;
            var cfg = new EffectConfig {
                HasEnabled = obj.ContainsKey("enabled"),
                Enabled = obj["enabled"]?.GetValue<bool>() ?? false,
                Gain = ClampDecimal((decimal)(obj["gain"]?.GetValue<double>() ?? 1.0), 0, 8),
                Dump = obj["dump"]?.GetValue<bool>() ?? false,
                Name = obj["name"]?.GetValue<string>() ?? ""
            };
            map[idx] = cfg;
        }
        return map;
    }

    void SaveEffectsConfig()
    {
        try
        {
            if (gridEffects.IsCurrentCellDirty) gridEffects.CommitEdit(DataGridViewDataErrorContexts.Commit);
            gridEffects.EndEdit();
            Directory.CreateDirectory(AppDir);
            var effects = new JsonObject();
            foreach (DataGridViewRow row in gridEffects.Rows)
            {
                if (row.IsNewRow) continue;
                if (!int.TryParse(Convert.ToString(row.Cells["colIdx"].Value, CultureInfo.InvariantCulture), out int idx)) continue;
                decimal gain = ReadGainCell(row.Cells["colGain"].Value);
                string name = Convert.ToString(row.Cells["colMeaning"].Value, CultureInfo.InvariantCulture) ?? "";
                string group = Convert.ToString(row.Cells["colGroup"].Value, CultureInfo.InvariantCulture) ?? "";
                string bank = Convert.ToString(row.Cells["colBank"].Value, CultureInfo.InvariantCulture) ?? "";
                string verdict = Convert.ToString(row.Cells["colVerdict"].Value, CultureInfo.InvariantCulture) ?? "";
                if (verdict.StartsWith("SKIP", StringComparison.OrdinalIgnoreCase)) continue;
                var item = new JsonObject {
                    ["enabled"] = ReadBoolCell(row.Cells["colEnabled"].Value),
                    ["gain"] = (double)gain
                };
                if (ReadBoolCell(row.Cells["colDump"].Value)) item["dump"] = true;
                if (!string.IsNullOrWhiteSpace(name)) item["name"] = name;
                if (!string.IsNullOrWhiteSpace(group)) item["group"] = group;
                if (!string.IsNullOrWhiteSpace(bank)) item["bank"] = bank;
                effects[idx.ToString(CultureInfo.InvariantCulture)] = item;
            }
            var root = new JsonObject {
                ["enabled"] = chkGlobalEnabled.Checked,
                ["defaultGain"] = (double)numDefaultGain.Value,
                ["dumpEnabled"] = chkDumpEnabled.Checked,
                ["useBuiltinDefaults"] = chkUseBuiltinDefaults.Checked,
                ["effects"] = effects
            };
            File.WriteAllText(HapticsConfigFile, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            bool gameRunning = Process.GetProcessesByName("sekiro").Length > 0;
            Msg(gameRunning ? "✓ 已保存配置；请重启只狼后生效。" : "✓ 已保存配置；下次启动只狼时生效。", OK);
        }
        catch (Exception ex) { Msg("保存音效配置失败：" + ex.Message, BAD); }
    }

    void SeedKnownEffects(Dictionary<int, EffectRow> rows, bool useBuiltinDefaults)
    {
        void Add(int idx, string name, bool enabled)
        {
            bool active = useBuiltinDefaults && enabled;
            if (!rows.ContainsKey(idx)) rows[idx] = new EffectRow { Idx = idx, Group = GroupKey(idx), Meaning = name, Enabled = active, Verdict = active ? "builtin" : "manual-off" };
        }
        for (int i = 665; i <= 700; i++) Add(i, "弹刀/格挡", true);
        Add(408, "危攻蓄力", true);
        for (int i = 983; i <= 992; i++) Add(i, "受伤/死亡", true);
        for (int i = 851; i <= 853; i++) Add(i, "处决/破防", true);
        for (int i = 256; i <= 258; i++) Add(i, "闪避/剧烈移动", true);
        Add(330, "不死斩挥刀", true); Add(331, "不死斩挥刀", true); Add(641, "不死斩挥刀", true);
        foreach (int i in new[] { 428, 435, 438, 444, 456 }) Add(i, "UI/菜单音", true);
        Add(353, "濒死心跳", true); Add(354, "濒死心跳", true);
        for (int i = 33; i <= 35; i++) Add(i, "归佛/传送", true);
        for (int i = 57; i <= 59; i++) Add(i, "死亡屏幕", true);
        Add(1031, "地名/到达", true); Add(1032, "地名/到达", true);
        for (int i = 60; i <= 64; i++) Add(i, "脚步", false);
        for (int i = 579; i <= 582; i++) Add(i, "木质脚步", false);
        for (int i = 629; i <= 632; i++) Add(i, "雪地脚步", false);
        Add(1131, "未标注音效", false); Add(1132, "未标注音效", false);
    }

    static bool IsKnownDefaultIdx(int idx)
    {
        if (idx >= 665 && idx <= 700) return true;
        if (idx == 408) return true;
        if (idx >= 983 && idx <= 992) return true;
        if (idx >= 851 && idx <= 853) return true;
        if (idx >= 256 && idx <= 258) return true;
        if (idx == 330 || idx == 331 || idx == 641) return true;
        if (idx == 428 || idx == 435 || idx == 438 || idx == 444 || idx == 456) return true;
        if (idx == 353 || idx == 354) return true;
        if (idx >= 33 && idx <= 35) return true;
        if (idx >= 57 && idx <= 59) return true;
        if (idx == 1031 || idx == 1032) return true;
        return false;
    }

    void LoadSeenEffects(Dictionary<int, EffectRow> rows)
    {
        if (!File.Exists(SeenEffectsFile)) return;
        foreach (string line in File.ReadLines(SeenEffectsFile))
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            try
            {
                if (JsonNode.Parse(line) is not JsonObject obj) continue;
                int idx = obj["idx"]?.GetValue<int>() ?? -1;
                if (idx < 0) continue;
                string verdict = obj["verdict"]?.GetValue<string>() ?? "";
                if (!rows.TryGetValue(idx, out var row))
                {
                    row = new EffectRow { Idx = idx, Group = GroupKey(idx), Enabled = false, Gain = 1.0M };
                    rows[idx] = row;
                }
                if (string.IsNullOrWhiteSpace(row.Group)) row.Group = GroupKey(idx);
                row.Bank = obj["bank"]?.GetValue<string>() ?? row.Bank;
                row.Meaning = obj["meaning"]?.GetValue<string>() ?? row.Meaning;
                row.Verdict = verdict.Length > 0 ? verdict : row.Verdict;
                row.LastSeen = obj["time"]?.GetValue<string>() ?? row.LastSeen;
            }
            catch { }
        }
    }

    void OpenDumpDir()
    {
        Directory.CreateDirectory(DumpDir);
        Process.Start(new ProcessStartInfo(DumpDir) { UseShellExecute = true });
    }

    void SetSelectedEnabled(bool enabled)
    {
        if (gridEffects.CurrentRow == null) { Msg("先选中一个音效。", MUTE); return; }
        string verdict = Convert.ToString(gridEffects.CurrentRow.Cells["colVerdict"].Value, CultureInfo.InvariantCulture) ?? "";
        if (verdict.StartsWith("SKIP", StringComparison.OrdinalIgnoreCase)) { Msg("这是音乐/语音分类，不建议启用。", MUTE); return; }
        gridEffects.CurrentRow.Cells["colEnabled"].Value = enabled;
        gridEffects.CurrentRow.DefaultCellStyle.ForeColor = enabled ? FG : MUTE;
        Msg(enabled ? "已勾选；记得保存配置并重启游戏。" : "已取消勾选；记得保存配置并重启游戏。", MUTE);
    }

    void ApplyReviewMode()
    {
        if (MessageBox.Show(
                "试听标注模式会关闭内置白名单，并取消当前列表里所有音效的启用状态。\n\n游戏里只录候选音效，不会把未确认声音送到手柄。继续吗？",
                "试听标注模式", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
        chkGlobalEnabled.Checked = true;
        chkUseBuiltinDefaults.Checked = false;
        chkDumpEnabled.Checked = true;
        foreach (DataGridViewRow row in gridEffects.Rows)
        {
            if (row.IsNewRow) continue;
            row.Cells["colEnabled"].Value = false;
            row.DefaultCellStyle.ForeColor = MUTE;
        }
        SaveEffectsConfig();
        Msg("✓ 已切到试听标注模式：只录不震。请重启只狼后触发音效，再回来试听勾选。", OK);
    }

    void PlaySelectedDump()
    {
        try
        {
            if (gridEffects.CurrentRow == null) { Msg("先选中一个音效。", MUTE); return; }
            if (!int.TryParse(Convert.ToString(gridEffects.CurrentRow.Cells["colIdx"].Value, CultureInfo.InvariantCulture), out int idx)) return;
            Directory.CreateDirectory(DumpDir);
            string group = Convert.ToString(gridEffects.CurrentRow.Cells["colGroup"].Value, CultureInfo.InvariantCulture) ?? GroupKey(idx);
            var cands = new List<string>();
            if (!string.IsNullOrWhiteSpace(group)) cands.Add(Path.Combine(DumpDir, group + ".wav"));
            cands.Add(Path.Combine(DumpDir, $"idx{idx}.wav"));
            var wav = cands.Select(p => new FileInfo(p)).Where(f => f.Exists).OrderByDescending(f => f.LastWriteTime).FirstOrDefault();
            if (wav == null) { Msg($"{group} / idx {idx} 还没有 WAV。勾“录音”→保存配置→重启游戏→触发一次。", MUTE); return; }
            Process.Start(new ProcessStartInfo(wav.FullName) { UseShellExecute = true });
        }
        catch (Exception ex) { Msg("试听失败：" + ex.Message, BAD); }
    }

    static string GroupKey(int idx)
    {
        if (idx >= 665 && idx <= 700) return "deflect_guard";
        if (idx == 408) return "danger_attack";
        if (idx >= 983 && idx <= 992) return "hurt_death";
        if (idx >= 851 && idx <= 853) return "deathblow_break";
        if (idx >= 256 && idx <= 258) return "dodge_cloth";
        if (idx == 330 || idx == 331 || idx == 641) return "mortal_draw";
        if (idx == 428 || idx == 435 || idx == 438 || idx == 444 || idx == 456) return "ui_menu";
        if (idx == 353 || idx == 354) return "low_hp_heartbeat";
        if (idx >= 33 && idx <= 35) return "travel_buddha";
        if (idx >= 57 && idx <= 59) return "death_screen";
        if (idx == 1031 || idx == 1032) return "area_title";
        if (idx >= 60 && idx <= 64) return "footstep_common";
        if (idx >= 579 && idx <= 582) return "footstep_wood";
        if (idx >= 629 && idx <= 632) return "footstep_snow";
        if (idx >= 401 && idx <= 402) return "ashina_cross_counter";
        return "idx" + idx.ToString(CultureInfo.InvariantCulture);
    }

    static bool ReadBoolCell(object? value)
    {
        if (value is bool b) return b;
        return bool.TryParse(Convert.ToString(value, CultureInfo.InvariantCulture), out bool parsed) && parsed;
    }

    static decimal ReadGainCell(object? value)
    {
        string s = Convert.ToString(value, CultureInfo.InvariantCulture) ?? "1";
        if (!decimal.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out decimal gain) &&
            !decimal.TryParse(s, NumberStyles.Float, CultureInfo.CurrentCulture, out gain)) gain = 1.0M;
        return ClampDecimal(gain, 0, 8);
    }

    static decimal ClampDecimal(decimal value, decimal min, decimal max)
    {
        if (value < min) return min;
        if (value > max) return max;
        return value;
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
        if (dev.cable) { btnInstallCable.Text = "✓ VB-CABLE 已安装"; btnInstallCable.Enabled = false; }
        else if (btnInstallCable.Enabled == false && btnInstallCable.Text.StartsWith("✓")) { } // 安装中不覆盖
        else { btnInstallCable.Text = "↓ 一键下载安装 VB-CABLE（免费虚拟声卡）"; btnInstallCable.Enabled = true; }

        // 新用户(还没设过输出)且检测到 CABLE → 默认走推荐的 DSX 路线，一上手就是最佳手感
        if (dev.cable && !rbCable.Checked && !File.Exists(TargetFile)) rbCable.Checked = true;

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

    // ============================ 一键安装 VB-CABLE(官方下载,不打包) ============================
    async System.Threading.Tasks.Task InstallCable()
    {
        const string page = "https://vb-audio.com/Cable/";
        const string url = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip";
        if (MessageBox.Show(
                "将从 VB-Audio 官方下载 VB-CABLE 虚拟声卡安装器并打开。\n\n" +
                "· VB-CABLE 免费；「虚拟声卡→DSX」手感路线需要它\n" +
                "· 安装需管理员权限，装完请【重启电脑】\n\n是否继续？",
                "下载安装 VB-CABLE", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
        btnInstallCable.Enabled = false;
        try
        {
            Msg("正在从 VB-Audio 官方下载…", MUTE);
            string dir = Path.Combine(Path.GetTempPath(), "VBCABLE_dl");
            Directory.CreateDirectory(dir);
            string zip = Path.Combine(dir, "VBCABLE.zip");
            using (var http = new HttpClient { Timeout = TimeSpan.FromMinutes(3) })
                await File.WriteAllBytesAsync(zip, await http.GetByteArrayAsync(url));
            string ex = Path.Combine(dir, "extracted");
            if (Directory.Exists(ex)) Directory.Delete(ex, true);
            ZipFile.ExtractToDirectory(zip, ex);
            string? setup = Directory.GetFiles(ex, "VBCABLE_Setup_x64.exe", SearchOption.AllDirectories).FirstOrDefault()
                         ?? Directory.GetFiles(ex, "VBCABLE_Setup*.exe", SearchOption.AllDirectories).FirstOrDefault();
            if (setup == null) throw new FileNotFoundException("压缩包里没找到安装器");
            Msg("✓ 已下载。在弹出的安装器里点 Install Driver，装完请【重启电脑】。", OK);
            try { Process.Start(new ProcessStartInfo(setup) { UseShellExecute = true, Verb = "runas" }); }
            catch { Msg("安装器已下载到：" + setup + "，请手动以管理员运行。", MUTE); }
        }
        catch (Exception ex)
        {
            Msg("自动下载失败（" + ex.Message + "），已打开官方页，请手动下载安装。", BAD);
            try { Process.Start(new ProcessStartInfo(page) { UseShellExecute = true }); } catch { }
        }
        finally { btnInstallCable.Enabled = true; }
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
