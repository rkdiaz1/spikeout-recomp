# A problem report's network timeline (net-events.txt, src/net/event_log.hpp) read: when each other player's buttons
# came, against when this PC made its own for the same frames. Late by more than rollback's window (12 frames), the
# room waited for that player or slowed down for them; this PC itself standing still shows as a gap in its own frames.
# Prints, for each player, how late their buttons were (the median, the 95th percentile, the worst) and each spell they
# held the room; each spell this PC stood still; and the session's own log lines (joins, take-overs, drops) beside them.
#   tools/net-timeline.ps1 -File net-events.txt [-Window 12] [-Gap 0.2] [-Context 3]
# -Window: rollback's frames (0: a game without rollback: any lateness held it). -Gap: seconds without a frame of its
# own taken as this PC standing still. -Context: seconds of log lines shown before each spell. -MinSpell: a held spell
# shorter than this (seconds) not listed (a frame or two late now and then is normal).
param([Parameter(Mandatory)][string]$File, [int]$Window = 12, [double]$Gap = 0.2, [double]$Context = 3, [double]$MinSpell = 0.2)
$ErrorActionPreference = 'Stop'
$fps = 57.524

$src = @'
using System; using System.Collections.Generic; using System.Globalization; using System.IO; using System.Linq; using System.Text;
public static class NetTimeline {
  public static string Run(string path, int window, double gap, double context, double fps, double minSpell) {
    var inv = CultureInfo.InvariantCulture;
    var local = new SortedDictionary<long, double>();               // frame -> when its own buttons were made
    var arrived = new Dictionary<int, Dictionary<long, double>>();  // seat -> frame -> first came
    var from = new Dictionary<int, HashSet<int>>();                 // seat -> who sent them (itself, or the host for an empty cabinet)
    var notes = new List<Tuple<double, string>>();
    var ownFrames = new List<Tuple<double, long>>();
    int me = -1;
    foreach (var raw in File.ReadLines(path)) {
      if (raw.StartsWith("#") || raw.Length == 0) continue;
      var p = raw.Split(new[] {' '}, 3);
      double t; if (!double.TryParse(p[0], NumberStyles.Float, inv, out t) || p.Length < 2) continue;
      var rest = p.Length > 2 ? p[2] : "";
      var f = rest.Split(' ');
      switch (p[1]) {
        case "in": {
          int sender = int.Parse(f[0]), seat = int.Parse(f[1]); long first = long.Parse(f[2]); int count = int.Parse(f[3]);
          Dictionary<long, double> a; if (!arrived.TryGetValue(seat, out a)) arrived[seat] = a = new Dictionary<long, double>();
          for (long g = first; g < first + count; ++g) if (!a.ContainsKey(g)) a[g] = t;
          HashSet<int> s; if (!from.TryGetValue(seat, out s)) from[seat] = s = new HashSet<int>(); s.Add(sender);
          break; }
        case "local": { me = int.Parse(f[0]); long g = long.Parse(f[1]); if (!local.ContainsKey(g)) local[g] = t; ownFrames.Add(Tuple.Create(t, g)); break; }
        case "taken": notes.Add(Tuple.Create(t, "seat " + f[0] + " taken from frame " + f[1])); break;
        case "idle": notes.Add(Tuple.Create(t, "seat " + f[0] + " dropped: idle from frame " + f[1])); break;
        case "started": notes.Add(Tuple.Create(t, "the game started here: " + f[0] + " seats, this PC's seat " + f[1] + (f[2] != "0" ? ", from frame " + f[2] : ""))); break;
        case "note": notes.Add(Tuple.Create(t, rest)); break;
        case "delay": notes.Add(Tuple.Create(t, "input delay " + f[0] + " from frame " + f[2])); break;
        case "sync": // (time sync, once a second: only when it slowed the game or ran it unpaced)
          if (f.Length >= 10 && (f[5] != "0" || f[9] != "0"))
            notes.Add(Tuple.Create(t, "time sync: ahead " + f[1] + ", behind " + f[3] + "; slowed " + f[5] + " frames, sooner " + f[7] + ", unpaced " + f[9]));
          break;
        case "msg": case "sent": { // (the session's exchanges; the frequent routine ones left out)
          int type = int.Parse(f[1]);
          if (type == 7 || type == 19 || type == 21 || type == 22 || type == 25) break;
          string[] names = {"?", "welcome", "roster", "start", "suspect", "have", "idle", "hash", "bye", "cabinet", "state",
                            "want state", "take", "taken", "want board", "board", "parting", "backup", "want backup", "status",
                            "watch welcome", "watch info", "watchers", "watch mode", "state size", "node"};
          string name = type < names.Length ? names[type] : "message " + type;
          notes.Add(Tuple.Create(t, (p[1] == "msg" ? "received " + name + " from seat " : "sent " + name + " to seat ") + f[0] + " (" + f[2] + " bytes)"));
          break; }
      }
    }
    var o = new StringBuilder();
    if (local.Count == 0) return "no frames of this PC's own in the file (a spectator's, or an empty timeline)\n";
    double t0 = ownFrames[0].Item1, t1 = ownFrames[ownFrames.Count - 1].Item1;
    o.AppendFormat(inv, "this PC: seat {0}, frames {1}-{2}, {3:F1} s ({4:F1} to {5:F1})\n", me, local.Keys.First(), local.Keys.Last(), t1 - t0, t0, t1);
    Action<double, double> say = (from_t, to_t) => {
      foreach (var n in notes) if (n.Item1 >= from_t && n.Item1 <= to_t) o.AppendFormat(inv, "      {0,9:F3}  {1}\n", n.Item1, n.Item2);
    };
    // Each other player: how late, in frames, against this PC's own for the same frame.
    foreach (var kv in arrived.OrderBy(k => k.Key)) {
      int seat = kv.Key; if (seat == me) continue;
      var late = new List<Tuple<long, double, double>>(); // frame, when it came, how late (frames)
      foreach (var g in kv.Value) { double own; if (local.TryGetValue(g.Key, out own)) late.Add(Tuple.Create(g.Key, g.Value, (g.Value - own) * fps)); }
      if (late.Count == 0) continue;
      late.Sort((x, y) => x.Item1.CompareTo(y.Item1));
      var sorted = late.Select(x => x.Item3).OrderBy(x => x).ToList();
      Func<double, double> pct = q => sorted[Math.Min(sorted.Count - 1, (int)(q * sorted.Count))];
      o.AppendFormat(inv, "\nseat {0} (sent by {1}): {2} frames; late by {3:F1} frames at the median, {4:F1} at 95%, {5:F1} at worst\n",
                     seat, string.Join(",", from[seat].OrderBy(x => x)), late.Count, pct(0.5), pct(0.95), sorted[sorted.Count - 1]);
      // Spells over the window: the room held for this player.
      int i = 0;
      while (i < late.Count) {
        if (late[i].Item3 <= window) { ++i; continue; }
        int j = i; double worst = 0;
        while (j < late.Count && late[j].Item3 > window) { worst = Math.Max(worst, late[j].Item3); ++j; }
        double start = local[late[i].Item1], end = late[j - 1].Item2;
        if (end - start < minSpell) { i = j; continue; }
        o.AppendFormat(inv, "  held by seat {0}: frames {1}-{2}, from {3:F3} s for {4:F2} s, the worst {5:F0} frames late\n",
                       seat, late[i].Item1, late[j - 1].Item1, start, end - start, worst);
        say(start - context, end);
        i = j;
      }
    }
    // This PC standing still: its own frames with a gap.
    o.Append("\nthis PC's own frames:\n");
    int stills = 0;
    for (int k = 1; k < ownFrames.Count; ++k) {
      double d = ownFrames[k].Item1 - ownFrames[k - 1].Item1;
      if (d < gap) continue;
      ++stills;
      o.AppendFormat(inv, "  stood still {0:F2} s before frame {1}, at {2:F3} s\n", d, ownFrames[k].Item2, ownFrames[k - 1].Item1);
      say(ownFrames[k - 1].Item1 - context, ownFrames[k].Item1);
    }
    if (stills == 0) o.AppendFormat(inv, "  never stood still {0} s or more\n", gap);
    return o.ToString();
  }
}
'@
Add-Type -TypeDefinition $src -Language CSharp
[NetTimeline]::Run((Resolve-Path $File).Path, $Window, $Gap, $Context, $fps, $MinSpell)
