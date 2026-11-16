# mosaico.ps1: one window over the ultrawide showing the 8 training copies (SacredA1..A8) live, 4x2, through DWM
# thumbnails (no capture, no input to the games). Each cell: the game, and its agent's mind at work (the player,
# 2026-11-11: «montones de gráficos y mediciones», «quiero ver su cerebro funcionando»), read from its SacredBild.log:
# a raster of its decisions over the last 10 minutes (one row per kind), its thoughts as they come, its numbers
# (experience and deaths an hour, fights, kills, potions, quests, CPU, memory, stutters), the lowest health of each
# fight and the experience of each minute.
# Double click a cell: it fills the window; again: back to the grid. Esc closes the mosaic (the copies play on).
# The control bar on top and a right click on a cell run the copies without the coordinator (the player, 2026-11-15:
# «necesito todas las herramientas para no interrumpir tu trabajo»): save, restart, pause the agent, the stuck ones
# revived, the launcher started again. Only the SacredA1..A8 copies are ever touched, never the player's own folder.
param([int]$Cols = 4, [int]$Rows = 2)

Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing, System.Management -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Linq;
using System.Management;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

public class Hero
{
    public struct Ev { public DateTime At; public int Kind; public string Text; }
    public struct Fight { public DateTime At; public int Exp, Killed, Potions, Lowest, Before, Seconds; }
    public string Path;
    public long Offset = -1;
    public string Class = "", Intent = "", Difficulty = "", Campaign = "";
    public int Level, Quests;
    public List<Ev> Events = new List<Ev>();
    public List<Fight> Fights = new List<Fight>();
    public List<DateTime> Deaths = new List<DateTime>();
    public List<DateTime> Hitches = new List<DateTime>();
    public List<DateTime> Potions = new List<DateTime>();
    public DateTime LastLine = DateTime.MinValue;
    public DateTime StuckAt = DateTime.MinValue;
    public DateTime LastProgress = DateTime.MinValue;   // a fight, loot, quest, sale or level: the agent getting on
    public double Cpu; public long Ram; public TimeSpan LastCpu; public DateTime LastCpuAt;
}

public class Mosaico : Form
{
    [StructLayout(LayoutKind.Sequential)] struct RECT { public int Left, Top, Right, Bottom; public RECT(int l, int t, int r, int b) { Left = l; Top = t; Right = r; Bottom = b; } }
    [StructLayout(LayoutKind.Sequential)] struct THUMBPROPS { public uint dwFlags; public RECT rcDestination; public RECT rcSource; public byte opacity; public bool fVisible; public bool fSourceClientAreaOnly; }
    [DllImport("dwmapi.dll")] static extern int DwmRegisterThumbnail(IntPtr dest, IntPtr src, out IntPtr thumb);
    [DllImport("dwmapi.dll")] static extern int DwmUnregisterThumbnail(IntPtr thumb);
    [DllImport("dwmapi.dll")] static extern int DwmUpdateThumbnailProperties(IntPtr thumb, ref THUMBPROPS props);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    const uint DWM_TNP_RECTDESTINATION = 1, DWM_TNP_OPACITY = 4, DWM_TNP_VISIBLE = 8, DWM_TNP_SOURCECLIENTAREAONLY = 16;

    const int Bar = 24;
    readonly int cols, rows;
    readonly IntPtr[] source = new IntPtr[8];
    readonly IntPtr[] thumb = new IntPtr[8];
    readonly int[] pid = new int[8];
    readonly Hero[] heroes = new Hero[8];
    int zoomed = -1;
    readonly Timer timer = new Timer();
    readonly Font title = new Font("Segoe UI", 10.5f, FontStyle.Bold);
    readonly Font small = new Font("Segoe UI", 8.5f);
    readonly Font tiny = new Font("Segoe UI", 7.5f);
    readonly Font big = new Font("Segoe UI", 13f, FontStyle.Bold);
    int tick;

    // The kinds of decision, one row each in the raster.
    static readonly string[] Kinds = { "Combate", "Tácticas", "Botín", "Misión", "Movimiento", "Peligro", "Equipo" };
    static readonly Color[] KindColor = {
        Color.FromArgb(235, 80, 70), Color.FromArgb(180, 120, 255), Color.FromArgb(240, 200, 60), Color.FromArgb(90, 170, 255),
        Color.FromArgb(110, 210, 120), Color.FromArgb(255, 140, 40), Color.FromArgb(160, 160, 170) };

    // What a log line says, in the player's words: (pattern, kind, text). The first that matches.
    static readonly Tuple<Regex, int, string>[] Rules = {
        R(@"CombatLearn: fight over", 0, null),
        R(@"AutoAttack: target [0-9a-f]+, the one shooting", 0, "Ataca al que le dispara"),
        R(@"AutoAttack: target [0-9a-f]+ \(left for now", 0, "Vuelve a por uno que dejó: ya está en melé"),
        R(@"AutoAttack: target ", 0, "Elige objetivo"),
        R(@"AutoAttack: hit by arrow|AutoAttack: arrow", 0, "Le disparan: esquiva o busca al tirador"),
        R(@"keeping the distance", 0, "Mantiene la distancia con el arma de rango"),
        R(@"a hopeless fight", 5, "Pelea sin esperanza: se retira"),
        R(@"stepping back|retreat", 5, "Retirada"),
        R(@"out of reach|walled|unreachable", 5, "Fuera de alcance: lo deja"),
        R(@"too strong", 5, "Demasiado fuerte: no ahora"),
        R(@"AgentStuck: ", 5, "¡Atascado! Aviso al coordinador"),
        R(@"stuck|hemmed in|no way on", 5, "Atasco: busca otra salida"),
        R(@"careful from now on", 5, "Modo prudente"),
        R(@"AgentSave: death|AutoAttack: died", 5, "Muere"),
        R(@"Tactics: .*cast|Tactics: art", 1, "Lanza un arte de combate"),
        R(@"Tactics: ", 1, "Piensa la táctica"),
        R(@"AutoPotion: drinks|AutoPotion: healed", 1, "Bebe una poción"),
        R(@"AutoAttack: collect over|AutoAttack: collect |AutoAttack: loot|AutoAttack: container|AutoAttack: fixed thing", 2, "Recoge botín"),
        R(@"chest", 2, "Va a por un cofre"),
        R(@"journal moved on", 3, "¡Misión completada!"),
        R(@"AutoAttack: off to the quest|quest target", 3, "Va al objetivo de la misión"),
        R(@"speaking to the quest creature|quest dialog|spoke to", 3, "Habla con un personaje de la misión"),
        R(@"AutoAttack: quest book", 3, "Lee el diario de misiones"),
        R(@"farming zone", 4, "Elige zona de farmeo"),
        R(@"Click walk|no nearer to tile|no way found", 4, "Busca camino"),
        R(@"nothing around|roam", 4, "Explora"),
        R(@"mounting horse|horse", 4, "Caballo"),
        R(@"AgentSave: saving|AgentSave: saved", 6, "Guarda la partida"),
        R(@"HeroBuild: skill point|HeroBuild: attribute point|HeroBuild: new skill", 6, "Sube habilidad o atributo"),
        R(@"Gear: bought|opening the shop|craftsman", 6, "Tienda o artesano"),
        R(@"Gear: (equips|puts on|weapon slot)", 6, "Cambia de equipo"),
        R(@"Intent: ", 4, null) };
    static Tuple<Regex, int, string> R(string p, int k, string t) { return Tuple.Create(new Regex(p, RegexOptions.Compiled), k, t); }

    static readonly Regex LineRx = new Regex(@"^(\d\d):(\d\d):(\d\d)\.(\d+) \[\s*\d+\] (.*)$", RegexOptions.Compiled);
    static readonly Regex FightRx = new Regex(@"fight over, (\d+) s, (\d+) enemies, (\d+) killed, health (\d+) -> (\d+) \(lowest (\d+)\), (\d+) potions, (\d+) experience", RegexOptions.Compiled);
    static readonly Regex HeroRx = new Regex(@"HeroProfile: ([^(]+)\(class \d+\), level (\d+)", RegexOptions.Compiled);
    static readonly Regex ModeRx = new Regex(@"\| dificultad (\S+) \| campaña (\S+)", RegexOptions.Compiled);
    static readonly Regex IntentRx = new Regex(@"Intent: (\w+)", RegexOptions.Compiled);
    static readonly Regex QuestRx = new Regex(@"journal moved on: (\d+) done", RegexOptions.Compiled);
    // The same patterns as vigilante.ps1's revive (keep both alike).
    static readonly Regex ProgressRx = new Regex(@"^(CombatLearn: fight over|AutoAttack: collect over|.*journal moved on|Gear: (sold|bought|equips|puts on)|HeroBuild: |AutoAttack: target )", RegexOptions.Compiled);
    const int StaleMin = 10;     // the badge; the vigilante revives at 20

    // The control bar and the copies' files.
    const int TopH = 34;
    const string Common = @"D:\SteamLibrary\steamapps\common\";
    readonly string tools;
    readonly Dictionary<int, DateTime> restarting = new Dictionary<int, DateTime>();   // saved, closed 30 s later
    readonly Dictionary<int, DateTime> saving = new Dictionary<int, DateTime>();       // the save-now flag, gone after 30 s
    Label status;
    Button reviveButton;
    bool launcherUp, watcherUp;
    static string Dir(int i) { return Common + "SacredA" + (i + 1); }
    static readonly Dictionary<string, string> Intents = new Dictionary<string, string> {
        {"quest", "Misión"}, {"farm", "Farmeo"}, {"shop", "Tienda"}, {"fight", "Combate"}, {"roam", "Explora"},
        {"rest", "Descansa"}, {"travel", "Viaja"}, {"loot", "Botín"}, {"retreat", "Retirada"}, {"chest", "Cofre"} };

    public Mosaico(int cols, int rows, string tools)
    {
        this.cols = cols; this.rows = rows; this.tools = tools;
        for (int i = 0; i < 8; i++) heroes[i] = new Hero { Path = @"D:\SteamLibrary\steamapps\common\SacredA" + (i + 1) + @"\SacredBild.log" };
        Text = "Sacred Entrenamiento";
        FormBorderStyle = FormBorderStyle.None;
        BackColor = Color.FromArgb(12, 12, 16);
        DoubleBuffered = true;
        StartPosition = FormStartPosition.Manual;
        Screen s = Screen.AllScreens.OrderByDescending(x => (double)x.Bounds.Width / x.Bounds.Height).First();
        Bounds = s.WorkingArea;
        KeyPreview = true;
        KeyDown += (o, e) => { if (e.KeyCode == Keys.Escape) Close(); else if (e.KeyCode == Keys.R) ShowWheel(!WheelOpen); };
        MouseDoubleClick += (o, e) => { int c = CellAt(e.Location); zoomed = zoomed >= 0 ? -1 : c; PlaceThumbs(); Invalidate(); };
        MouseUp += (o, e) => { if (e.Button == MouseButtons.Right) { int c = CellAt(e.Location); if (c >= 0) CellMenu(c).Show(this, e.Location); } };
        BuildBar();
        timer.Interval = 1000;
        timer.Tick += (o, e) => Tick();
        Shown += (o, e) => { Tick(); timer.Start(); ShowWheel(false); };
        FormClosed += (o, e) => { for (int i = 0; i < 8; i++) if (thumb[i] != IntPtr.Zero) DwmUnregisterThumbnail(thumb[i]); };
    }

    Rectangle Cell(int i)
    {
        Rectangle area = new Rectangle(0, TopH, ClientSize.Width, ClientSize.Height - TopH);
        if (zoomed >= 0) return i == zoomed ? area : Rectangle.Empty;
        int w = area.Width / cols, h = area.Height / rows;
        return new Rectangle((i % cols) * w, area.Y + (i / cols) * h, w, h);
    }

    // ---- The control bar ----

    Button Btn(Control parent, string text, Color back, EventHandler click, string tip)
    {
        var b = new Button { Text = text, AutoSize = true, FlatStyle = FlatStyle.Flat, ForeColor = Color.White, BackColor = back,
                             Font = small, Margin = new Padding(3, 3, 3, 3), Cursor = Cursors.Hand };
        b.FlatAppearance.BorderColor = Color.FromArgb(70, 70, 84);
        b.Click += click;
        new ToolTip().SetToolTip(b, tip);
        parent.Controls.Add(b);
        return b;
    }

    void BuildBar()
    {
        var bar = new FlowLayoutPanel { Dock = DockStyle.Top, Height = TopH, BackColor = Color.FromArgb(24, 24, 32), WrapContents = false,
                                        Padding = new Padding(4, 1, 4, 0) };
        Controls.Add(bar);
        Color blue = Color.FromArgb(40, 70, 120), red = Color.FromArgb(120, 40, 40), grey = Color.FromArgb(50, 50, 62), green = Color.FromArgb(35, 95, 55);
        Btn(bar, "Reiniciar parados", blue, (o, e) => { foreach (int i in StaleCopies()) Restart(i); },
            "Guarda y reinicia las copias que llevan " + StaleMin + " min sin progreso (sin combates, botín, misiones ni ventas)");
        Btn(bar, "Guardar todas", green, (o, e) => { for (int i = 0; i < 8; i++) SaveNow(i); }, "Pide a las 8 copias que guarden ahora");
        Btn(bar, "Reiniciar todas", red, (o, e) => {
            if (Ask("¿Guardar y reiniciar las 8 copias?")) for (int i = 0; i < 8; i++) Restart(i);
        }, "Guarda y reinicia las 8 copias (el lanzador las vuelve a abrir)");
        Btn(bar, "Pausar agentes", grey, (o, e) => { for (int i = 0; i < 8; i++) SetPaused(i, true); },
            "Los 8 agentes dejan de actuar (fichero SacredBild-stop); el juego sigue abierto");
        Btn(bar, "Reanudar agentes", grey, (o, e) => { for (int i = 0; i < 8; i++) SetPaused(i, false); }, "Quita la pausa de los 8 agentes");
        reviveButton = Btn(bar, "", grey, (o, e) => ToggleRevive(),
            "El vigilante guarda y reinicia solo las copias que llevan 20 min sin progreso");
        Btn(bar, "Relanzar lanzador", grey, (o, e) => { if (Ask("¿Parar y volver a arrancar el lanzador de las copias?")) RestartLauncher(); },
            "Para todos los lanzadores (agentes.sh) y arranca uno; reabre las copias que falten");
        Btn(bar, "Rueda", Color.FromArgb(110, 35, 35), (o, e) => ShowWheel(!WheelOpen),
            "Abre o pliega la rueda (también con un clic en la bolita roja, que se arrastra a donde quieras): elige un grupo (A1–A4, A5–A8 o todas) y una acción solo para ese grupo (tecla R)");
        Btn(bar, "Registro del vigilante", grey, (o, e) => Open("notepad.exe", Path.Combine(tools, "vigilante.log")), "Lo que el vigilante ha cerrado o reiniciado");
        Btn(bar, "Atascos", grey, (o, e) => Open("explorer.exe", Path.Combine(tools, "atascos")), "Carpeta de atascos guardados para el solucionador");
        status = new Label { AutoSize = true, ForeColor = Color.Silver, Font = small, Margin = new Padding(14, 9, 3, 3) };
        bar.Controls.Add(status);
        UpdateReviveButton();
    }

    bool Ask(string q) { return MessageBox.Show(this, q, "Sacred Entrenamiento", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes; }

    static void Open(string exe, string arg) { try { Process.Start(exe, "\"" + arg + "\""); } catch { } }

    string ReviveOff { get { return Path.Combine(tools, "auto-revivir.off"); } }
    void ToggleRevive()
    {
        try { if (File.Exists(ReviveOff)) File.Delete(ReviveOff); else File.WriteAllText(ReviveOff, "off"); } catch { }
        UpdateReviveButton();
    }
    void UpdateReviveButton()
    {
        bool on = !File.Exists(ReviveOff);
        reviveButton.Text = "Revivir solo: " + (on ? "SÍ" : "NO");
        reviveButton.BackColor = on ? Color.FromArgb(35, 95, 55) : Color.FromArgb(90, 60, 30);
    }

    // ---- What the buttons do (only on SacredA1..A8) ----

    Process ProcessOf(int i)
    {
        string exe = Dir(i) + @"\Sacred.exe";
        foreach (var p in Process.GetProcessesByName("Sacred"))
            try { if (string.Equals(p.MainModule.FileName, exe, StringComparison.OrdinalIgnoreCase)) return p; } catch { }
        return null;
    }

    void SaveNow(int i)
    {
        if (ProcessOf(i) == null) return;
        try { File.WriteAllText(Dir(i) + @"\SacredBild-save-now", ""); saving[i] = DateTime.Now; } catch { }
    }

    // Saved, then closed 30 s later; agentes.sh starts it again.
    void Restart(int i)
    {
        if (ProcessOf(i) == null || restarting.ContainsKey(i)) return;
        SaveNow(i);
        restarting[i] = DateTime.Now;
    }

    void Kill(int i)
    {
        var p = ProcessOf(i);
        try { if (p != null) p.Kill(); } catch { }
    }

    bool Paused(int i) { return File.Exists(Dir(i) + @"\SacredBild-stop"); }
    void SetPaused(int i, bool paused)
    {
        string f = Dir(i) + @"\SacredBild-stop";
        try { if (paused) File.WriteAllText(f, "paused from the mosaic"); else if (File.Exists(f)) File.Delete(f); } catch { }
    }

    // The log's last 400 lines into atascos\, for the stuck solver.
    void KeepStuck(int i)
    {
        try
        {
            string log = Dir(i) + @"\SacredBild.log";
            string[] lines;
            using (var fs = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                long len = Math.Min(fs.Length, 400000);
                fs.Seek(-len, SeekOrigin.End);
                var buf = new byte[len];
                int n = fs.Read(buf, 0, buf.Length);
                lines = Encoding.UTF8.GetString(buf, 0, n).Split('\n');
            }
            string dir = Path.Combine(tools, "atascos");
            Directory.CreateDirectory(dir);
            string file = Path.Combine(dir, "A" + (i + 1) + "-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-mosaico.txt");
            File.WriteAllLines(file, lines.Skip(Math.Max(0, lines.Length - 400)).Select(l => l.TrimEnd('\r')));
            MessageBox.Show(this, "Guardado para el solucionador de atascos:\n" + file, "Sacred Entrenamiento");
        }
        catch (Exception ex) { MessageBox.Show(this, "No se pudo guardar: " + ex.Message, "Sacred Entrenamiento"); }
    }

    void RestartLauncher()
    {
        // Every agentes.sh stopped (two at once start each copy twice), then one started; it gives the release to A1
        // only and leaves the guinea pigs' builds alone.
        string script = Path.Combine(tools, "agentes.sh");
        // The dollar as (char)36: this C# sits in a PowerShell string that would expand a plain one.
        string it = (char)36 + "_";
        string ps = "Get-CimInstance Win32_Process -Filter \"Name='bash.exe'\" | Where-Object { " + it + ".CommandLine -like '*agentes.sh*' } | " +
                    "ForEach-Object { Stop-Process -Id " + it + ".ProcessId -Force -ErrorAction SilentlyContinue }; Start-Sleep 2; " +
                    "Start-Process 'C:\\Program Files\\Git\\bin\\bash.exe' -ArgumentList '\"" + script.Replace("'", "''") + "\" 8 4' -WindowStyle Hidden";
        try { Process.Start(new ProcessStartInfo("powershell.exe", "-NoProfile -WindowStyle Hidden -Command \"" + ps.Replace("\"", "\\\"") + "\"") { WindowStyle = ProcessWindowStyle.Hidden, UseShellExecute = false, CreateNoWindow = true }); } catch { }
    }

    List<int> StaleCopies()
    {
        var list = new List<int>();
        for (int i = 0; i < 8; i++) if (Stale(i)) list.Add(i);
        return list;
    }
    bool Stale(int i)
    {
        Hero h = heroes[i];
        return source[i] != IntPtr.Zero && !Paused(i) && h.LastProgress != DateTime.MinValue &&
               (DateTime.Now - h.LastProgress).TotalMinutes >= StaleMin;
    }

    ContextMenuStrip CellMenu(int i)
    {
        var m = new ContextMenuStrip();
        string name = "A" + (i + 1);
        m.Items.Add(name + (heroes[i].Class.Length > 0 ? " · " + heroes[i].Class : "")).Enabled = false;
        m.Items.Add(new ToolStripSeparator());
        m.Items.Add("Guardar ahora", null, (o, e) => SaveNow(i));
        m.Items.Add("Reiniciar (guarda y cierra; el lanzador la reabre)", null, (o, e) => Restart(i));
        m.Items.Add("Cerrar ya, sin guardar", null, (o, e) => { if (Ask("¿Cerrar " + name + " sin guardar?")) Kill(i); });
        m.Items.Add(Paused(i) ? "Reanudar el agente" : "Pausar el agente (el juego sigue abierto)", null, (o, e) => SetPaused(i, !Paused(i)));
        m.Items.Add(new ToolStripSeparator());
        m.Items.Add("Guardar atasco para el solucionador", null, (o, e) => KeepStuck(i));
        m.Items.Add("Abrir el registro", null, (o, e) => Open("notepad.exe", Dir(i) + @"\SacredBild.log"));
        m.Items.Add("Abrir la carpeta", null, (o, e) => Open("explorer.exe", Dir(i)));
        m.Items.Add(zoomed == i ? "Volver a la cuadrícula" : "Ampliar", null, (o, e) => { zoomed = zoomed == i ? -1 : i; PlaceThumbs(); Invalidate(); });
        return m;
    }

    // Each second: the restarts due, the save flags gone; every 10 s the launcher's and the vigilante's state.
    void Housekeeping()
    {
        var now = DateTime.Now;
        foreach (var kv in restarting.ToList())
            if ((now - kv.Value).TotalSeconds >= 30) { Kill(kv.Key); restarting.Remove(kv.Key); }
        foreach (var kv in saving.ToList())
            if ((now - kv.Value).TotalSeconds >= 30) { try { File.Delete(Dir(kv.Key) + @"\SacredBild-save-now"); } catch { } saving.Remove(kv.Key); }
        if (tick % 10 == 1)
        {
            launcherUp = watcherUp = false;
            try
            {
                using (var q = new ManagementObjectSearcher("SELECT CommandLine FROM Win32_Process WHERE Name='bash.exe' OR Name='powershell.exe'"))
                    foreach (ManagementObject o in q.Get())
                    {
                        string cl = (o["CommandLine"] ?? "").ToString();
                        // Another program's command naming these files (a shell of Claude's, a status query) is not them.
                        if (cl.Contains("shell-snapshots") || cl.Contains("Win32_Process") || cl.Contains("Get-CimInstance")) continue;
                        if (cl.Contains("agentes.sh")) launcherUp = true;
                        if (cl.Contains("vigilante.ps1")) watcherUp = true;
                    }
            }
            catch { }
            UpdateReviveButton();
        }
        int alive = 0, stale = 0, paused = 0;
        for (int i = 0; i < 8; i++) { if (source[i] != IntPtr.Zero) alive++; if (Stale(i)) stale++; if (Paused(i)) paused++; }
        status.Text = "Copias " + alive + "/8   ·   paradas " + stale + "   ·   en pausa " + paused +
                      "   ·   lanzador " + (launcherUp ? "✔" : "✘ (Relanzar)") + "   ·   vigilante " + (watcherUp ? "✔" : "✘") +
                      (restarting.Count > 0 ? "   ·   reiniciando " + string.Join(", ", restarting.Keys.Select(k => "A" + (k + 1))) : "");
        status.ForeColor = !launcherUp || !watcherUp || stale > 0 ? Color.Orange : Color.Silver;
    }
    int CellAt(Point p) { for (int i = 0; i < 8; i++) if (Cell(i).Contains(p)) return i; return -1; }

    // The cell's parts: the game (top left), the raster under it, the numbers and thoughts on the right.
    void Parts(Rectangle c, out Rectangle game, out Rectangle raster, out Rectangle side)
    {
        int sideW = Math.Max(260, c.Width / 4);
        int innerW = c.Width - sideW - 6, innerH = c.Height - Bar - 4;
        int rasterH = Math.Max(90, innerH / 5);
        int gameH = innerH - rasterH - 4;
        int gameW = innerW;
        game = new Rectangle(c.X + 2, c.Y + Bar + 2, gameW, gameH);
        raster = new Rectangle(c.X + 2, game.Bottom + 4, innerW, rasterH);
        side = new Rectangle(c.X + innerW + 6, c.Y + Bar + 2, sideW - 4, innerH);
    }

    void Tick()
    {
        tick++;
        var procs = Process.GetProcessesByName("Sacred");
        for (int i = 0; i < 8; i++)
        {
            string exe = @"D:\SteamLibrary\steamapps\common\SacredA" + (i + 1) + @"\Sacred.exe";
            IntPtr h = IntPtr.Zero; Process found = null;
            foreach (var p in procs)
            {
                try { if (string.Equals(p.MainModule.FileName, exe, StringComparison.OrdinalIgnoreCase)) { h = p.MainWindowHandle; found = p; break; } } catch { }
            }
            if (h != source[i] || (thumb[i] != IntPtr.Zero && !IsWindow(source[i])))
            {
                if (thumb[i] != IntPtr.Zero) { DwmUnregisterThumbnail(thumb[i]); thumb[i] = IntPtr.Zero; }
                source[i] = h;
                if (h != IntPtr.Zero) DwmRegisterThumbnail(Handle, h, out thumb[i]);
            }
            if (h != IntPtr.Zero) SetWindowPos(h, Handle, 0, 0, 0, 0, 0x13);
            Hero hero = heroes[i];
            if (found != null)
            {
                try
                {
                    var now = DateTime.Now; var cpu = found.TotalProcessorTime;
                    if (pid[i] == found.Id && hero.LastCpuAt != DateTime.MinValue)
                        hero.Cpu = (cpu - hero.LastCpu).TotalMilliseconds / (now - hero.LastCpuAt).TotalMilliseconds / Environment.ProcessorCount * 100.0;
                    hero.LastCpu = cpu; hero.LastCpuAt = now; pid[i] = found.Id;
                    hero.Ram = found.WorkingSet64 / (1024 * 1024);
                }
                catch { }
            }
            if (tick % 2 == 0 || hero.Offset < 0) Read(hero);
        }
        Housekeeping();
        PlaceThumbs();
        Invalidate();
    }

    void PlaceThumbs()
    {
        for (int i = 0; i < 8; i++)
        {
            if (thumb[i] == IntPtr.Zero) continue;
            Rectangle c = Cell(i);
            var props = new THUMBPROPS { dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_OPACITY, opacity = 255, fSourceClientAreaOnly = true };
            props.fVisible = !c.IsEmpty;
            if (!c.IsEmpty)
            {
                Rectangle game, raster, side; Parts(c, out game, out raster, out side);
                RECT src; GetClientRect(source[i], out src);
                int sw = Math.Max(1, src.Right), sh = Math.Max(1, src.Bottom);
                double k = Math.Min((double)game.Width / sw, (double)game.Height / sh);
                int w = (int)(sw * k), h = (int)(sh * k);
                int x = game.X + (game.Width - w) / 2, y = game.Y + (game.Height - h) / 2;
                props.rcDestination = new RECT(x, y, x + w, y + h);
            }
            DwmUpdateThumbnailProperties(thumb[i], ref props);
        }
    }

    // The log's new lines (the last 3 MB at the first read; from the start again when the game began a new log).
    void Read(Hero h)
    {
        try
        {
            using (var fs = new FileStream(h.Path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                if (h.Offset < 0) h.Offset = Math.Max(0, fs.Length - 3000000);
                if (fs.Length < h.Offset) { h.Offset = 0; h.LastProgress = DateTime.MinValue; }
                if (fs.Length == h.Offset) return;
                fs.Seek(h.Offset, SeekOrigin.Begin);
                var bytes = new byte[fs.Length - h.Offset];
                int n = fs.Read(bytes, 0, bytes.Length);
                int end = Array.LastIndexOf(bytes, (byte)'\n', n - 1);
                if (end < 0) return;
                h.Offset += end + 1;
                string text = Encoding.UTF8.GetString(bytes, 0, end + 1);
                foreach (var raw in text.Split('\n')) Parse(h, raw.TrimEnd('\r'));
            }
        }
        catch { }
        var cut = DateTime.Now.AddHours(-1);
        h.Events.RemoveAll(e => e.At < cut.AddMinutes(-30));
        h.Fights.RemoveAll(f => f.At < cut);
        h.Hitches.RemoveAll(t => t < cut);
        h.Potions.RemoveAll(t => t < cut);
        h.Deaths.RemoveAll(t => t < cut);
    }

    void Parse(Hero h, string line)
    {
        var m = LineRx.Match(line);
        if (!m.Success) return;
        var today = DateTime.Today;
        var at = today.AddHours(int.Parse(m.Groups[1].Value)).AddMinutes(int.Parse(m.Groups[2].Value)).AddSeconds(int.Parse(m.Groups[3].Value));
        if (at > DateTime.Now.AddMinutes(5)) at = at.AddDays(-1);
        h.LastLine = at;
        string msg = m.Groups[5].Value;
        Match x;
        if ((x = HeroRx.Match(msg)).Success) { h.Class = x.Groups[1].Value.Trim(); h.Level = int.Parse(x.Groups[2].Value); }
        if ((x = ModeRx.Match(msg)).Success) { h.Difficulty = x.Groups[1].Value; h.Campaign = x.Groups[2].Value; }
        if ((x = IntentRx.Match(msg)).Success) { string es; h.Intent = Intents.TryGetValue(x.Groups[1].Value, out es) ? es : x.Groups[1].Value; }
        if ((x = QuestRx.Match(msg)).Success) h.Quests = int.Parse(x.Groups[1].Value);
        if (msg.StartsWith("Hitch: the presenting thread froze")) { h.Hitches.Add(at); return; }
        if (msg.StartsWith("Hitch:")) return;
        if (msg.Contains("AgentSave: death") || msg.StartsWith("AutoAttack: died")) h.Deaths.Add(at);
        if (msg.StartsWith("AutoPotion: drinks")) h.Potions.Add(at);
        if (msg.StartsWith("AgentStuck: ")) h.StuckAt = at;
        if (ProgressRx.IsMatch(msg)) h.LastProgress = at;
        // A new log (the copy started again): its start counts as progress, so a fresh copy is not «stopped».
        if (h.LastProgress == DateTime.MinValue) h.LastProgress = at;
        if (msg.Contains("a new game with class")) { h.Deaths.Clear(); h.Fights.Clear(); h.Potions.Clear(); h.Quests = 0; }
        foreach (var r in Rules)
        {
            if (!r.Item1.IsMatch(msg)) continue;
            string text = r.Item3;
            if ((x = FightRx.Match(msg)).Success)
            {
                var f = new Hero.Fight { At = at, Seconds = int.Parse(x.Groups[1].Value), Killed = int.Parse(x.Groups[3].Value),
                    Before = int.Parse(x.Groups[4].Value), Lowest = int.Parse(x.Groups[6].Value), Potions = int.Parse(x.Groups[7].Value), Exp = int.Parse(x.Groups[8].Value) };
                h.Fights.Add(f);
                text = "Combate: " + f.Killed + " muertos, +" + f.Exp + " exp, vida mínima " + f.Lowest;
            }
            else if (text == null && (x = IntentRx.Match(msg)).Success) text = "Decide: " + h.Intent;
            if (text == null) break;
            // The same thought again within 3 s: once.
            if (h.Events.Count > 0) { var last = h.Events[h.Events.Count - 1]; if (last.Text == text && (at - last.At).TotalSeconds < 3) break; }
            h.Events.Add(new Hero.Ev { At = at, Kind = r.Item2, Text = text });
            break;
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        using (var barBrush = new SolidBrush(Color.FromArgb(30, 30, 38)))
        using (var frame = new Pen(Color.FromArgb(55, 55, 66)))
        {
            for (int i = 0; i < 8; i++)
            {
                Rectangle c = Cell(i);
                if (c.IsEmpty) continue;
                Hero h = heroes[i];
                g.FillRectangle(barBrush, c.X, c.Y, c.Width, Bar);
                g.DrawRectangle(frame, c.X, c.Y, c.Width - 1, c.Height - 1);
                // The wheel's group outlined, when it is one half.
                if (wheel != null && wheel.Visible && group != 2 && InGroup(i))
                    using (var sel = new Pen(Color.FromArgb(200, 60, 60), 3f)) g.DrawRectangle(sel, c.X + 1, c.Y + 1, c.Width - 3, c.Height - 3);
                bool alive = (DateTime.Now - h.LastLine).TotalSeconds < 30;
                string head = "A" + (i + 1) + (h.Class.Length > 0 ? "  ·  " + h.Class + "  ·  nivel " + h.Level : "") +
                              (h.Difficulty.Length > 0 ? "  ·  " + h.Difficulty + "  ·  " + h.Campaign : "") +
                              (h.Intent.Length > 0 ? "  ·  " + h.Intent : "") + (source[i] == IntPtr.Zero ? "  ·  arrancando…" : "");
                TextRenderer.DrawText(g, head, title, new Rectangle(c.X + 8, c.Y, c.Width - 16, Bar), Color.Gainsboro,
                                      TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
                using (var dot = new SolidBrush(alive ? Color.LimeGreen : Color.OrangeRed)) g.FillEllipse(dot, c.Right - 18, c.Y + 8, 9, 9);
                // One badge at most, the most telling first: paused on purpose, being restarted, no progress, stuck.
                string badge = null; Color badgeColor = Color.Empty;
                if (Paused(i)) { badge = "EN PAUSA"; badgeColor = Color.FromArgb(90, 90, 110); }
                else if (restarting.ContainsKey(i)) { badge = "REINICIANDO"; badgeColor = Color.FromArgb(40, 90, 160); }
                else if (Stale(i)) { badge = "PARADO " + (int)(DateTime.Now - h.LastProgress).TotalMinutes + " min"; badgeColor = Color.FromArgb(210, 110, 20); }
                else if ((DateTime.Now - h.StuckAt).TotalMinutes < 3) { badge = "ATASCADO"; badgeColor = Color.FromArgb(200, 30, 30); }
                if (badge != null)
                {
                    using (var b = new SolidBrush(badgeColor)) g.FillRectangle(b, c.Right - 150, c.Y + 3, 124, Bar - 6);
                    TextRenderer.DrawText(g, badge, title, new Rectangle(c.Right - 150, c.Y, 124, Bar), Color.White,
                                          TextFormatFlags.VerticalCenter | TextFormatFlags.HorizontalCenter);
                }
                Rectangle game, raster, side; Parts(c, out game, out raster, out side);
                DrawRaster(g, h, raster);
                DrawSide(g, h, side);
            }
        }
    }

    // The mind at work: each decision of the last 10 minutes a tick in its kind's row.
    void DrawRaster(Graphics g, Hero h, Rectangle r)
    {
        using (var bg = new SolidBrush(Color.FromArgb(20, 20, 26))) g.FillRectangle(bg, r);
        int label = 72;
        int rowH = Math.Max(8, (r.Height - 14) / Kinds.Length);
        var now = DateTime.Now; double span = 600;
        int plotW = r.Width - label - 4;
        using (var grid = new Pen(Color.FromArgb(40, 40, 50)))
        {
            for (int m = 0; m <= 10; m += 2)
            {
                int x = r.X + label + (int)(plotW * (1 - m * 60 / span));
                g.DrawLine(grid, x, r.Y, x, r.Y + rowH * Kinds.Length);
                TextRenderer.DrawText(g, m == 0 ? "ahora" : "-" + m + " min", tiny, new Point(x - 14, r.Y + rowH * Kinds.Length + 1), Color.Gray);
            }
        }
        for (int k = 0; k < Kinds.Length; k++)
        {
            TextRenderer.DrawText(g, Kinds[k], tiny, new Rectangle(r.X + 2, r.Y + k * rowH, label - 4, rowH), KindColor[k], TextFormatFlags.VerticalCenter);
        }
        foreach (var ev in h.Events)
        {
            double age = (now - ev.At).TotalSeconds;
            if (age < 0 || age > span) continue;
            int x = r.X + label + (int)(plotW * (1 - age / span));
            int alpha = (int)Math.Max(90, 255 - age / span * 160);
            using (var p = new Pen(Color.FromArgb(alpha, KindColor[ev.Kind]), age < 5 ? 3f : 2f))
                g.DrawLine(p, x, r.Y + ev.Kind * rowH + 1, x, r.Y + (ev.Kind + 1) * rowH - 1);
        }
    }

    void DrawSide(Graphics g, Hero h, Rectangle r)
    {
        using (var bg = new SolidBrush(Color.FromArgb(20, 20, 26))) g.FillRectangle(bg, r);
        var now = DateTime.Now;
        var recent = h.Fights.Where(f => (now - f.At).TotalMinutes <= 15).ToList();
        double expH = recent.Sum(f => f.Exp) * 4.0;
        int deathsH = h.Deaths.Count(t => (now - t).TotalMinutes <= 60);
        double hitchMin = h.Hitches.Count(t => (now - t).TotalMinutes <= 5) / 5.0;
        string[,] stats = {
            { "Exp/hora", expH.ToString("N0") }, { "Muertes/hora", deathsH.ToString() },
            { "Combates 15 min", recent.Count.ToString() }, { "Enemigos 15 min", recent.Sum(f => f.Killed).ToString() },
            { "Pociones/hora", h.Potions.Count(t => (now - t).TotalMinutes <= 60).ToString() }, { "Misiones hechas", h.Quests.ToString() },
            { "CPU", h.Cpu.ToString("0.0") + " %" }, { "RAM", h.Ram + " MB" },
            { "Tirones/min", hitchMin.ToString("0.0") }, { "Pensamientos/min", h.Events.Count(ev => (now - ev.At).TotalSeconds <= 60).ToString() } };
        int colW = r.Width / 2, cellH = 34, y = r.Y + 4;
        for (int s = 0; s < stats.GetLength(0); s++)
        {
            int x = r.X + 6 + (s % 2) * colW, yy = y + (s / 2) * cellH;
            TextRenderer.DrawText(g, stats[s, 0], tiny, new Point(x, yy), Color.Gray);
            TextRenderer.DrawText(g, stats[s, 1], big, new Point(x, yy + 10), s == 1 && deathsH > 0 ? Color.OrangeRed : Color.White);
        }
        y += cellH * 5 + 4;
        // The lowest health of each fight, as a share of its start (the last 20).
        var last = h.Fights.Skip(Math.Max(0, h.Fights.Count - 20)).ToList();
        Rectangle chart = new Rectangle(r.X + 6, y + 12, r.Width - 12, 46);
        TextRenderer.DrawText(g, "Vida mínima en cada combate", tiny, new Point(chart.X, y), Color.Gray);
        using (var frame = new Pen(Color.FromArgb(45, 45, 55))) g.DrawRectangle(frame, chart);
        if (last.Count > 1)
        {
            var pts = last.Select((f, j) => new PointF(chart.X + j * (chart.Width - 1f) / (last.Count - 1),
                chart.Bottom - (float)Math.Min(1.0, f.Before > 0 ? (double)f.Lowest / f.Before : 0) * (chart.Height - 2) - 1)).ToArray();
            using (var p = new Pen(Color.FromArgb(120, 220, 140), 2f)) g.DrawLines(p, pts);
            foreach (var pt in pts) using (var b = new SolidBrush(pt.Y > chart.Bottom - chart.Height * 0.3 ? Color.OrangeRed : Color.FromArgb(120, 220, 140))) g.FillEllipse(b, pt.X - 2, pt.Y - 2, 4, 4);
        }
        y = chart.Bottom + 6;
        // Experience each minute, the last 30.
        Rectangle bars = new Rectangle(r.X + 6, y + 12, r.Width - 12, 40);
        TextRenderer.DrawText(g, "Experiencia por minuto (30 min)", tiny, new Point(bars.X, y), Color.Gray);
        var perMin = new double[30];
        foreach (var f in h.Fights) { int ago = (int)(now - f.At).TotalMinutes; if (ago >= 0 && ago < 30) perMin[29 - ago] += f.Exp; }
        double max = Math.Max(1, perMin.Max());
        float bw = bars.Width / 30f;
        using (var b = new SolidBrush(Color.FromArgb(90, 170, 255)))
            for (int j = 0; j < 30; j++) { float hgt = (float)(perMin[j] / max) * bars.Height; g.FillRectangle(b, bars.X + j * bw + 1, bars.Bottom - hgt, bw - 2, hgt); }
        y = bars.Bottom + 8;
        // Its thoughts, newest first.
        TextRenderer.DrawText(g, "Pensamientos", tiny, new Point(r.X + 6, y), Color.Gray);
        y += 14;
        for (int j = h.Events.Count - 1; j >= 0 && y + 15 < r.Bottom; j--)
        {
            var ev = h.Events[j];
            using (var b = new SolidBrush(KindColor[ev.Kind])) g.FillRectangle(b, r.X + 6, y + 4, 6, 8);
            TextRenderer.DrawText(g, ev.At.ToString("HH:mm:ss") + "  " + ev.Text, small, new Rectangle(r.X + 16, y, r.Width - 20, 15),
                                  j == h.Events.Count - 1 ? Color.White : Color.Silver, TextFormatFlags.EndEllipsis);
            y += 15;
        }
    }

    // ---- The wheel (the player's sketch, 2026-11-16): a round panel over the middle of the mosaic. Its hub picks the
    // group (A1-A4, A5-A8 or all); its ring runs an action on that group only. A window of its own: the games'
    // thumbnails cover whatever the mosaic itself draws under them. ----

    int group = 2;                      // 0 A1-A4, 1 A5-A8, 2 all
    Rueda wheel;
    static readonly string[] GroupNames = { "A1 – A4", "A5 – A8", "Todas" };
    List<int> GroupCopies() { return (group == 0 ? Enumerable.Range(0, 4) : group == 1 ? Enumerable.Range(4, 4) : Enumerable.Range(0, 8)).ToList(); }
    bool InGroup(int i) { return group == 2 || (group == 0 ? i < 4 : i >= 4); }

    // The wheel folds (the player, 2026-11-16: "it takes much room, let it come and go"): a small knob in the middle
    // that opens into the wheel on a click and folds back after an action or once the mouse leaves it. The knob can be
    // dragged anywhere; the wheel opens round it.
    void ShowWheel(bool open)
    {
        if (wheel == null) wheel = new Rueda(this);
        wheel.SetOpen(open);
        if (!wheel.Visible) wheel.Show(this);
        Invalidate();
    }
    bool WheelOpen { get { return wheel != null && wheel.Visible && wheel.IsOpen; } }

    class Rueda : Form
    {
        const int R = 150, Hub = 58, K = 24;      // K: the folded knob's radius
        readonly Mosaico m;
        bool open = true;
        public bool IsOpen { get { return open; } }
        Point off = Point.Empty;                // the knob's centre from the mosaic's middle (dragged by the player)
        Point downAt; bool dragging, down;
        readonly Timer fold = new Timer { Interval = 1200 };
        int hover = -1;                 // 0..4 a sector of the ring, 10 + n the hub's group row n
        static readonly string[] Actions = { "Guardar", "Reiniciar", "Reiniciar parados", "Pausar agentes", "Reanudar agentes" };
        static readonly Color[] ActionColor = {
            Color.FromArgb(35, 95, 55), Color.FromArgb(120, 40, 40), Color.FromArgb(40, 70, 120), Color.FromArgb(70, 70, 86), Color.FromArgb(60, 80, 70) };

        public Rueda(Mosaico m)
        {
            this.m = m;
            FormBorderStyle = FormBorderStyle.None; ShowInTaskbar = false; StartPosition = FormStartPosition.Manual;
            BackColor = Color.FromArgb(18, 18, 24); DoubleBuffered = true; Opacity = 0.95;
            Cursor = Cursors.Hand;
            fold.Tick += (o, e) => { fold.Stop(); if (open && !Bounds.Contains(Control.MousePosition)) SetOpen(false); };
            MouseEnter += (o, e) => fold.Stop();
            MouseLeave += (o, e) => { hover = -1; Invalidate(); if (open) fold.Start(); };
            MouseDown += (o, e) => { if (e.Button == MouseButtons.Left && !open) { down = true; dragging = false; downAt = Control.MousePosition; } };
            MouseMove += (o, e) =>
            {
                if (!open)
                {
                    if (!down) return;
                    Point p = Control.MousePosition;
                    if (!dragging && Math.Abs(p.X - downAt.X) + Math.Abs(p.Y - downAt.Y) > 4) dragging = true;
                    if (dragging) { off.Offset(p.X - downAt.X, p.Y - downAt.Y); downAt = p; Place(); }
                    return;
                }
                int h = HitTest(e.Location); if (h != hover) { hover = h; Invalidate(); }
            };
            MouseUp += (o, e) =>
            {
                if (e.Button != MouseButtons.Left) return;
                if (!open) { bool was = dragging; down = dragging = false; if (!was) SetOpen(true); return; }
                RunAt(HitTest(e.Location));
            };
            SetOpen(false);
        }

        public void SetOpen(bool o)
        {
            open = o; hover = -1; fold.Stop();
            int r = open ? R : K;
            Size = new Size(2 * r, 2 * r);
            var path = new GraphicsPath(); path.AddEllipse(0, 0, 2 * r, 2 * r); Region = new Region(path);
            Opacity = open ? 0.95 : 0.85;
            Place(); Invalidate();
        }

        public void Place()
        {
            Rectangle b = m.Bounds;
            int r = open ? R : K;
            int cx = b.X + b.Width / 2 + off.X, cy = b.Y + TopH + (b.Height - TopH) / 2 + off.Y;
            // Kept whole on the mosaic.
            cx = Math.Max(b.X + r, Math.Min(b.Right - r, cx));
            cy = Math.Max(b.Y + TopH + r, Math.Min(b.Bottom - r, cy));
            Location = new Point(cx - r, cy - r);
        }

        int HitTest(Point p)
        {
            double dx = p.X - R, dy = p.Y - R, r = Math.Sqrt(dx * dx + dy * dy);
            if (r > R) return -1;
            if (r <= Hub) return 10 + Math.Max(0, Math.Min(2, (int)((dy + Hub) / (2.0 * Hub / 3))));
            double a = Math.Atan2(dy, dx) * 180 / Math.PI + 90;     // 0 at the top, clockwise
            if (a < 0) a += 360;
            return (int)(a / 72) % 5;
        }

        void RunAt(int h)
        {
            if (h >= 10) { m.group = h - 10; m.Invalidate(); Invalidate(); return; }
            if (h < 0) return;
            List<int> copies = m.GroupCopies();
            string name = GroupNames[m.group];
            switch (h)
            {
                case 0: foreach (int i in copies) m.SaveNow(i); break;
                case 1: if (m.Ask("¿Guardar y reiniciar " + name + "?")) foreach (int i in copies) m.Restart(i); break;
                case 2: foreach (int i in copies) if (m.Stale(i)) m.Restart(i); break;
                case 3: foreach (int i in copies) m.SetPaused(i, true); break;
                case 4: foreach (int i in copies) m.SetPaused(i, false); break;
            }
            m.Invalidate();
            SetOpen(false);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            if (!open)
            {
                var knob = new Rectangle(1, 1, 2 * K - 3, 2 * K - 3);
                using (var b = new SolidBrush(Color.FromArgb(150, 40, 40))) g.FillEllipse(b, knob);
                using (var p = new Pen(Color.FromArgb(230, 120, 120), 2f)) g.DrawEllipse(p, knob);
                string label = m.group == 0 ? "1-4" : m.group == 1 ? "5-8" : "1-8";
                TextRenderer.DrawText(g, label, m.small, new Rectangle(0, 0, 2 * K, 2 * K), Color.White,
                                      TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
                return;
            }
            var disc = new Rectangle(0, 0, 2 * R - 1, 2 * R - 1);
            var flags = TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.WordBreak;
            for (int s = 0; s < 5; s++)
            {
                float start = -90 + s * 72;
                Color col = hover == s ? ControlPaint.Light(ActionColor[s], 0.5f) : ActionColor[s];
                using (var b = new SolidBrush(col)) g.FillPie(b, disc, start, 72);
                using (var p = new Pen(BackColor, 3f)) g.DrawPie(p, disc, start, 72);
                double mid = (start + 36) * Math.PI / 180, rr = (R + Hub) / 2.0 + 3;
                int cx = (int)(R + Math.Cos(mid) * rr), cy = (int)(R + Math.Sin(mid) * rr);
                TextRenderer.DrawText(g, Actions[s], m.small, new Rectangle(cx - 46, cy - 20, 92, 40), Color.White, flags);
            }
            var hub = new Rectangle(R - Hub, R - Hub, 2 * Hub, 2 * Hub);
            using (var b = new SolidBrush(Color.FromArgb(26, 26, 34))) g.FillEllipse(b, hub);
            using (var p = new Pen(Color.FromArgb(200, 60, 60), 2f)) g.DrawEllipse(p, hub);
            float rowH = 2f * Hub / 3;
            for (int k = 0; k < 3; k++)
            {
                var row = new Rectangle(R - Hub + 12, (int)(R - Hub + k * rowH), 2 * Hub - 24, (int)rowH);
                bool selected = m.group == k;
                if (selected) using (var b = new SolidBrush(Color.FromArgb(160, 45, 45))) g.FillRectangle(b, row.X, row.Y + 5, row.Width, row.Height - 10);
                TextRenderer.DrawText(g, GroupNames[k], m.small, row, selected ? Color.White : hover == 10 + k ? Color.Gainsboro : Color.Gray,
                                      TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            }
        }
    }
}
"@

[System.Windows.Forms.Application]::EnableVisualStyles()
[System.Windows.Forms.Application]::Run((New-Object Mosaico $Cols, $Rows, $PSScriptRoot))
