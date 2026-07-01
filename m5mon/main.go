package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"time"
)

type Sensors struct {
	CPUPerc  []float64 `json:"cpu_perf_cores"`
	CPUEff   []float64 `json:"cpu_eff_cores"`
	GPUCores []float64 `json:"gpu_temps"`
	Fan0     float64   `json:"fan0_rpm"`
	Fan1     float64   `json:"fan1_rpm"`
}

type ClusterUtil struct {
	Perc float64 `json:"p_cluster_pct"` // AP2u / 256 * 100
	Eff  float64 `json:"e_cluster_pct"` // AP3u / 256 * 100
}

type MemInfo struct {
	UsedGB  float64 `json:"used_gb"`
	TotalGB float64 `json:"total_gb"`
	FreeGB  float64 `json:"free_gb"`
	WiredGB float64 `json:"wired_gb"`
	CompGB  float64 `json:"compressed_gb"`
}

type ProcInfo struct {
	PID    string  `json:"pid"`
	Comm   string  `json:"command"`
	CPU    float64 `json:"cpu_pct"`
	RSS_MB float64 `json:"rss_mb"`
}

type BatteryInfo struct {
	Percent  int     `json:"percent"`
	Charging bool    `json:"charging"`
	Cycles   int     `json:"cycles"`
}

type AnomalyInfo struct{ Zombies, Stuck int }

type SystemReport struct {
	Timestamp  string      `json:"timestamp"`
	Sensors    Sensors     `json:"sensors"`
	Cluster    ClusterUtil `json:"cluster_utilization"`
	GPUPct     int         `json:"gpu_pct"`
	Memory     MemInfo     `json:"memory"`
	LoadAvg    [3]float64  `json:"load_avg"`
	Uptime     string      `json:"uptime"`
	Battery    BatteryInfo `json:"battery"`
	TopProcs   []ProcInfo  `json:"top_processes"`
	Anomalies  AnomalyInfo `json:"anomalies"`
}

const MAX_RPM = 5269.0

func xc(name string, args ...string) string {
	cmd := exec.Command(name, args...)
	b, err := cmd.Output()
	if err != nil { return "" }
	return string(b)
}

// ── SMC ─────────────────────────────────────────────────────────────
// readSMCall returns float values from smc -l. TYPE is padded: [flt ], [si16], etc.
// For [si16]: displayed value is unsigned; sign-extend if >32767.
func readSMCall(smcPath string) map[string]float64 {
	raw := xc(smcPath, "-l")
	vals := map[string]float64{}
	re := regexp.MustCompile(`^\s+(\S{4})\s+\[([^\]]+)\]\s+(-?[\d.]+)`)
	for _, line := range strings.Split(raw, "\n") {
		m := re.FindStringSubmatch(line)
		if m == nil { continue }
		k, typ, sval := m[1], strings.TrimSpace(m[2]), m[3]
		f, err := strconv.ParseFloat(sval, 64)
		if err != nil { continue }
		if typ == "si16" {
			u := int32(f)
			if u > 32767 { u -= 65536 }
			f = float64(u)
		}
		vals[k] = f
	}
	return vals
}

// readSMCuint16 parses hex bytes from smc -l for a key as little-endian uint16.
func readSMCuint16(smcPath, key string) int {
	raw := xc(smcPath, "-l")
	for _, line := range strings.Split(raw, "\n") {
		if len(line) < 6 || strings.TrimSpace(line[2:6]) != key { continue }
		idx := strings.Index(line, "(bytes")
		if idx < 0 { continue }
		rest := strings.TrimRight(line[idx+6:], ") \t\r\n")
		parts := strings.Fields(rest)
		if len(parts) < 2 { continue }
		b1, e1 := strconv.ParseUint(parts[0], 16, 8)
		b2, e2 := strconv.ParseUint(parts[1], 16, 8)
		if e1 != nil || e2 != nil { continue }
		val := int(b1) | int(b2)<<8
		if val > 32767 { val -= 65536 }
		return val
	}
	return 0
}

func readSensors(smcPath string) Sensors {
	v := readSMCall(smcPath)
	return Sensors{
		CPUPerc:  []float64{v["TPD7"], v["TPD6"], v["TPD5"], v["TPD4"]},
		CPUEff:   []float64{v["TPD0"], v["TPD1"], v["TPD2"], v["TPD3"],
			v["TPD8"], v["TPD9"], v["TPDa"], v["TPDb"],
			v["TPDc"], v["TPDd"], v["TPDe"], v["TPDf"]},
		GPUCores: []float64{v["TVD0"], v["TVDM"], v["TVDA"]},
		Fan0:     v["F0Ac"],
		Fan1:     v["F1Ac"],
	}
}

func readCluster(smcPath string) ClusterUtil {
	p := float64(readSMCuint16(smcPath, "AP2u"))
	e := float64(readSMCuint16(smcPath, "AP3u"))
	if p > 256 { p = 0 }
	if e > 256 { e = 0 }
	return ClusterUtil{Perc: p / 256.0 * 100.0, Eff: e / 256.0 * 100.0}
}

// ── Memory ──────────────────────────────────────────────────────────
func readMem(totalGB float64) MemInfo {
	raw := xc("vm_stat")
	fm := map[string]float64{}
	re := regexp.MustCompile(`^([^:]+):\s+([\d.]+)`)
	for _, line := range strings.Split(raw, "\n") {
		m := re.FindStringSubmatch(line)
		if m != nil {
			v, _ := strconv.ParseFloat(m[2], 64)
			fm[strings.TrimSpace(m[1])] = v * 16384.0 / 1e9
		}
	}
	used := fm["Pages active"] + fm["Pages wired down"] + fm["Pages stored in compressor"]
	return MemInfo{
		UsedGB: used, TotalGB: totalGB, FreeGB: totalGB - used,
		WiredGB: fm["Pages wired down"], CompGB: fm["Pages stored in compressor"],
	}
}

// ── Load / Uptime ───────────────────────────────────────────────────
func readLoad() [3]float64 {
	m := regexp.MustCompile(`\{?([\d.]+)\s+([\d.]+)\s+([\d.]+)`).FindStringSubmatch(xc("sysctl", "vm.loadavg"))
	if m != nil {
		a, _ := strconv.ParseFloat(m[1], 64)
		b, _ := strconv.ParseFloat(m[2], 64)
		c, _ := strconv.ParseFloat(m[3], 64)
		return [3]float64{a, b, c}
	}
	return [3]float64{}
}

func readUptime() string {
	m := regexp.MustCompile(`sec\s*=\s*(\d+)`).FindStringSubmatch(xc("sysctl", "-n", "kern.boottime"))
	if m != nil {
		boot, _ := strconv.ParseInt(m[1], 10, 64)
		up := time.Since(time.Unix(boot, 0))
		d := int(up.Hours()) / 24
		h := int(up.Hours()) % 24
		mi := int(up.Minutes()) % 60
		if d > 0 { return fmt.Sprintf("%dd %dh %dm", d, h, mi) }
		return fmt.Sprintf("%dh %dm", h, mi)
	}
	return "?"
}

// ── Processes ───────────────────────────────────────────────────────
func readTopProcs() []ProcInfo {
	raw := xc("ps", "axo", "pid,pcpu,pmem,rss,comm")
	lines := strings.Split(raw, "\n")
	type psort struct { pi ProcInfo; cpu float64 }
	var ps []psort
	for _, line := range lines[1:] {
		f := strings.Fields(line)
		if len(f) < 5 { continue }
		cpu, _ := strconv.ParseFloat(f[1], 64)
		if cpu < 0.5 { continue }
		rss, _ := strconv.ParseFloat(f[3], 64)
		comm := f[4]
		if len(comm) > 30 { comm = comm[:30] }
		ps = append(ps, psort{ProcInfo{PID: f[0], Comm: comm, CPU: cpu, RSS_MB: rss / 1024}, cpu})
	}
	for i := 1; i < len(ps); i++ {
		for j := i; j > 0 && ps[j].cpu > ps[j-1].cpu; j-- {
			ps[j], ps[j-1] = ps[j-1], ps[j]
		}
	}
	out := make([]ProcInfo, 0, 12)
	for i, p := range ps {
		if i >= 12 { break }
		out = append(out, p.pi)
	}
	return out
}

func readAnomalies() AnomalyInfo {
	raw := xc("ps", "axo", "state,pid,ppid,comm")
	z, s := 0, 0
	for _, line := range strings.Split(raw, "\n")[1:] {
		f := strings.Fields(line)
		if len(f) < 1 { continue }
		st := f[0]
		if strings.Contains(st, "Z") { z++ }
		if strings.Contains(st, "T") { s++ }
	}
	return AnomalyInfo{z, s}
}

func readGPUPct() int {
	cmd := exec.Command("sh", "-c", "ioreg -r -c IOAccelerator 2>/dev/null | grep -oE '\"Device Utilization %\"=[0-9]+'")
	b, err := cmd.Output()
	if err != nil || len(b) == 0 { return -1 }
	idx := strings.Index(string(b), "=")
	if idx < 0 { return -1 }
	v, err := strconv.Atoi(strings.TrimSpace(string(b)[idx+1:]))
	if err != nil { return -1 }
	return v
}

func readBattery(smcPath string) BatteryInfo {
	bat := BatteryInfo{}
	if r := xc("pmset", "-g", "batt"); r != "" {
		if m := regexp.MustCompile(`(\d+)%`).FindStringSubmatch(r); m != nil {
			bat.Percent, _ = strconv.Atoi(m[1])
		}
		bat.Charging = strings.Contains(r, "charging") && !strings.Contains(r, "discharging")
	}
	if r := xc("system_profiler", "SPPowerDataType"); r != "" {
		if m := regexp.MustCompile(`Cycle Count:\s+(\d+)`).FindStringSubmatch(r); m != nil {
			bat.Cycles, _ = strconv.Atoi(m[1])
		}
	}
	return bat
}

// ── TUI ─────────────────────────────────────────────────────────────
const (
	C    = "\033["
	CLR  = "\033[2J\033[H"
	HIDE = "\033[?25l"
	SHOW = "\033[?25h"
)

func tc(t float64) string {
	switch {
	case t >= 75: return "196"
	case t >= 60: return "214"
	case t >= 50: return "226"
	default: return "47"
	}
}

func pc(p float64) string {
	switch {
	case p >= 80: return "196"
	case p >= 50: return "214"
	default: return "47"
	}
}

func bar(v, mx float64, w int) string {
	n := int(v / mx * float64(w))
	if n > w { n = w }
	if n < 0 { n = 0 }
	return strings.Repeat("█", n) + strings.Repeat("░", w-n)
}

func render(s Sensors, cl ClusterUtil, gpuPct int, m MemInfo, ld [3]float64, up string,
	bat BatteryInfo, procs []ProcInfo, anom AnomalyInfo) {

	cpuM, gpuM := 0.0, 0.0
	for _, t := range s.CPUPerc { if t > cpuM { cpuM = t } }
	for _, t := range s.CPUEff  { if t > cpuM { cpuM = t } }
	for _, t := range s.GPUCores { if t > gpuM { gpuM = t } }

	fmt.Print(CLR + C + "1m")
	fmt.Print("  ┌─────────────────────────────────────────────────────────────────────┐\n")
	fmt.Printf("  │  %-67s │\n", "M5 Pro Monitor (m5mon)")
	fmt.Print("  └─────────────────────────────────────────────────────────────────────┘\n" + C + "0m")

	fmt.Printf("\n  %s●%s  CPU  %5.0f°C  %s  max\n",
		C+"1m"+C+"38;5;"+tc(cpuM)+"m", C+"0m", cpuM, bar(cpuM, 100, 15))
	gpuLine := fmt.Sprintf("  %s●%s  GPU  %5.0f°C  %s",
		C+"1m"+C+"38;5;"+tc(gpuM)+"m", C+"0m", gpuM, bar(gpuM, 100, 15))
	if gpuPct >= 0 {
		gpuLine += fmt.Sprintf("  "+C+"38;5;%sm%3d%%"+C+"0m", pc(float64(gpuPct)), gpuPct)
	}
	fmt.Println(gpuLine)

	fmt.Printf("\n  %s  CPU Perf Cores%s", C+"1m", C+"0m")
	for _, t := range s.CPUPerc { fmt.Printf(C+"38;5;%sm %4.0f"+C+"0m", tc(t), t) }
	fmt.Printf("\n  %s  CPU Eff Cores%s", C+"1m", C+"0m")
	for _, t := range s.CPUEff { fmt.Printf(C+"38;5;%sm %4.0f"+C+"0m", tc(t), t) }
	fmt.Printf("\n  %s  GPU Hot  Mem  Avg%s", C+"1m", C+"0m")
	for _, t := range s.GPUCores { if t > 0 { fmt.Printf(C+"38;5;%sm %4.0f"+C+"0m", tc(t), t) } }
	fmt.Printf("\n")

	f0 := s.Fan0 / MAX_RPM * 100
	f1 := s.Fan1 / MAX_RPM * 100
	fmt.Printf("\n  %s  Fans%s", C+"1m", C+"0m")
	fmt.Printf(C+"38;5;%sm %5.0f"+C+"0m /"+C+"38;5;%sm %5.0f"+C+"0m RPM  (%3.0f%%) %s\n",
		pc(f0), s.Fan0, pc(f1), s.Fan1, (f0+f1)/2, bar((f0+f1)/2, 100, 20))

	pk := cpuM; if gpuM > pk { pk = gpuM }
	cr, cc2 := "silent", "47"
	if pk >= 75 { cr, cc2 = "aggressive", "196" } else if pk >= 60 { cr, cc2 = "active", "214" } else if pk >= 50 { cr, cc2 = "balanced", "226" }
	fmt.Printf("  Curve: " + C + "38;5;" + cc2 + "m" + cr + C + "0m sigmoid\n")

	mp := m.UsedGB / m.TotalGB * 100
	fmt.Printf("\n  %s  MEM%s  ", C+"1m", C+"0m")
	fmt.Printf(C+"38;5;%sm"+C+"0m", pc(mp))
	fmt.Printf("%5.1fG / %4.0fG  %s  wired %.1fG  comp %.1fG\n",
		m.UsedGB, m.TotalGB, bar(m.UsedGB, m.TotalGB, 15), m.WiredGB, m.CompGB)

	fmt.Printf("\n  %s  LOAD%s  %.2f / %.2f / %.2f\n", C+"1m", C+"0m", ld[0], ld[1], ld[2])
	fmt.Printf("  %s  UP%s   %s\n", C+"1m", C+"0m", up)

	if cl.Perc > 0 || cl.Eff > 0 {
		fmt.Printf("\n  %s  UTIL%s  P-cluster:", C+"1m", C+"0m")
		fmt.Printf(C+"38;5;%sm %5.1f%%"+C+"0m", pc(cl.Perc), cl.Perc)
		fmt.Printf("  E-cluster:")
		fmt.Printf(C+"38;5;%sm %5.1f%%"+C+"0m", pc(cl.Eff), cl.Eff)
		fmt.Printf("  %s  %s\n", bar(cl.Perc, 100, 10), bar(cl.Eff, 100, 10))
	}

	if bat.Percent > 0 {
		ic := "⬆"; if !bat.Charging { ic = "⬇" }
		fmt.Printf("  %s  BATT%s %s %3d%%", C+"1m", C+"0m", ic, bat.Percent)
		if bat.Cycles > 0 { fmt.Printf("  Cycles: %d", bat.Cycles) }
		fmt.Printf("\n")
	}

	fmt.Printf("\n  %s  TOP CPU%s\n", C+"1m", C+"0m")
	for i, p := range procs {
		if i >= 8 { break }
		fmt.Printf("  %2d. ", i+1)
		fmt.Printf(C+"38;5;%sm%5.1f%%"+C+"0m", pc(p.CPU), p.CPU)
		fmt.Printf("  %4.0fM  %s\n", p.RSS_MB, p.Comm)
	}
	if anom.Zombies > 0 { fmt.Printf("\n  ⚠  %d zombie(s)\n", anom.Zombies) }
	if anom.Stuck > 0 { fmt.Printf("\n  ⚠  %d stuck\n", anom.Stuck) }
	fmt.Print("\n  Ctrl+C to exit\n")
}

// ── JSON ────────────────────────────────────────────────────────────
func renderJSON(s Sensors, cl ClusterUtil, gpuPct int, m MemInfo, ld [3]float64, up string,
	bat BatteryInfo, procs []ProcInfo, anom AnomalyInfo) {
	json.NewEncoder(os.Stdout).Encode(SystemReport{
		Timestamp: time.Now().Format(time.RFC3339),
		Sensors: s, Cluster: cl, GPUPct: gpuPct, Memory: m, LoadAvg: ld,
		Uptime: up, Battery: bat, TopProcs: procs, Anomalies: anom,
	})
}

// ── Main ────────────────────────────────────────────────────────────
func main() {
	jsonMode := flag.Bool("json", false, "one-shot JSON report")
	flag.Parse()

	smcPath := os.Getenv("HOME") + "/.local/bin/smc"
	if _, err := os.Stat(smcPath); os.IsNotExist(err) {
		if p, err := exec.LookPath("smc"); err == nil { smcPath = p }
	}
	totalMem, _ := strconv.ParseFloat(strings.TrimSpace(xc("sysctl", "-n", "hw.memsize")), 64)
	totalMemGB := totalMem / 1e9

	if *jsonMode {
		renderJSON(readSensors(smcPath), readCluster(smcPath), readGPUPct(), readMem(totalMemGB),
			readLoad(), readUptime(), readBattery(smcPath),
			readTopProcs(), readAnomalies())
		return
	}

	fmt.Print(HIDE)
	defer fmt.Print(SHOW)
	for {
		render(readSensors(smcPath), readCluster(smcPath), readGPUPct(), readMem(totalMemGB),
			readLoad(), readUptime(), readBattery(smcPath),
			readTopProcs(), readAnomalies())
		time.Sleep(2 * time.Second)
	}
}
