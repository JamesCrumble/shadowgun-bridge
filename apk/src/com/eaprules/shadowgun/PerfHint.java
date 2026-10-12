package com.eaprules.shadowgun;

import android.content.Context;
import android.os.Build;
import android.os.PerformanceHintManager;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Tells Android how long the game's frames take, so it clocks up the cores they run on.
 *
 * The emulated game is one busy thread in the qemu child process, and nothing about a child process
 * reads as "game" to the scheduler: on the phone it was left on a mid core with the prime cluster capped
 * at a quarter of its clock. A PerformanceHintManager session over qemu's threads, fed the real frame
 * time against the frame limit's period, is the documented way to ask for more. The target has to be the
 * rate the game is aiming for: with a fixed 60 fps target and a 90 fps limit the system saw 11 ms frames
 * against 16.7 ms, took that as headroom and clocked the cores down until the game ran at ~60-65 fps. Sessions over another process's
 * threads are accepted for the same uid on recent Android; where they are not, this logs and does nothing.
 */
final class PerfHint {
    interface Log { void log(String s); }

    private long targetNs = 16_666_667L;

    private final Context ctx;
    private final Log log;
    private volatile Object session;   // PerformanceHintManager.Session, kept untyped for API < 31
    private volatile int pid;
    private long lastFrame;
    private int lastThreadCount;

    PerfHint(Context ctx, Log log) {
        this.ctx = ctx;
        this.log = log;
    }

    /** The pid of a java.lang.Process: Android's toString() names it ("Process[pid=123, ...]"). */
    static int pidOf(Process p) {
        Matcher m = Pattern.compile("pid=(\\d+)").matcher(String.valueOf(p));
        if (m.find()) return Integer.parseInt(m.group(1));
        try {
            java.lang.reflect.Field f = p.getClass().getDeclaredField("pid");
            f.setAccessible(true);
            return f.getInt(p);
        } catch (Exception e) {
            return -1;
        }
    }

    /**
     * CPU_SPREAD=1: qemu is started on the two prime cores, and with the engine, its job worker, the gfx thread,
     * audio and the rest all there the two that matter get preempted. Everything except the engine and the job
     * worker is moved to cores 0-5.
     */
    boolean spread;
    private final java.util.Set<Integer> moved = new java.util.HashSet<>();

    private void spreadLoop() {
        try {
            int n = 0, failed = 0;
            while (new File("/proc/" + pid).exists()) {
                for (int tid : threads()) {
                    if (tid == pid || moved.contains(tid)) continue;
                    String comm;
                    try { comm = readFirstLine("/proc/" + pid + "/task/" + tid + "/comm").trim(); } catch (java.io.IOException e) { continue; }
                    // qemu names guest threads only once the game does; until then they carry qemu's own name.
                    if (comm.startsWith("UnityMain") || comm.startsWith("Worker Thread") || comm.startsWith("libqemu")) continue;
                    moved.add(tid);
                    try {
                        Process t = new ProcessBuilder("/system/bin/taskset", "-p", "3f", String.valueOf(tid))
                                .redirectErrorStream(true).start();
                        while (t.getInputStream().read() >= 0) { }
                        if (t.waitFor() == 0) n++; else failed++;
                    } catch (java.io.IOException e) {
                        failed++;
                    }
                }
                if (n + failed != lastSpread) {
                    lastSpread = n + failed;
                    log.log("perf hint: CPU_SPREAD: " + n + " threads moved to cores 0-5, " + failed + " failed");
                }
                Thread.sleep(3000);
            }
        } catch (InterruptedException ignored) {
        }
    }

    private int lastSpread;

    void start(Process p, boolean adpf, int fpsLimit) {
        if (Build.VERSION.SDK_INT < 31) return;
        if (fpsLimit >= 15 && fpsLimit <= 240) targetNs = 1_000_000_000L / fpsLimit;
        pid = pidOf(p);
        if (pid <= 0) {
            log.log("perf hint: no pid for " + p);
            return;
        }
        if (spread) {
            Thread sp = new Thread(this::spreadLoop, "me-spread");
            sp.setDaemon(true);
            sp.start();
        }
        if (!adpf) {
            log.log("perf hint: ADPF session off (settings)");
            return;
        }
        Thread t = new Thread(this::watch, "me-perfhint");
        t.setDaemon(true);
        t.start();
    }

    /** qemu starts its guest threads over the first seconds; the session follows the thread list. */
    private void watch() {
        try {
            while (new File("/proc/" + pid).exists()) {
                int[] tids = threads();
                if (tids.length > 0 && tids.length != lastThreadCount) {
                    lastThreadCount = tids.length;
                    attach(tids);
                }
                Thread.sleep(session == null ? 1000 : 5000);
            }
        } catch (InterruptedException ignored) {
        }
    }

    private int[] threads() {
        String[] names = new File("/proc/" + pid + "/task").list();
        if (names == null) return new int[0];
        List<Integer> l = new ArrayList<>();
        for (String n : names) {
            try { l.add(Integer.parseInt(n)); } catch (NumberFormatException ignored) { }
        }
        int[] a = new int[l.size()];
        for (int i = 0; i < a.length; i++) a[i] = l.get(i);
        return a;
    }

    private void attach(int[] tids) {
        if (Build.VERSION.SDK_INT < 31) return;
        try {
            PerformanceHintManager m = ctx.getSystemService(PerformanceHintManager.class);
            if (m == null) {
                log.log("perf hint: no PerformanceHintManager");
                return;
            }
            Object s = session;
            if (s != null && Build.VERSION.SDK_INT >= 34) {
                ((PerformanceHintManager.Session) s).setThreads(tids);
            } else {
                if (s != null) ((PerformanceHintManager.Session) s).close();
                session = m.createHintSession(tids, targetNs);
                // Android 15: do not trade this session's speed for power (API 35, so by reflection).
                if (session != null && Build.VERSION.SDK_INT >= 35) {
                    try {
                        session.getClass().getMethod("setPreferPowerEfficiency", boolean.class).invoke(session, false);
                    } catch (Exception e) {
                        log.log("perf hint: setPreferPowerEfficiency: " + e);
                    }
                }
            }
            log.log("perf hint: session over " + tids.length + " qemu threads: " + (session != null ? "ok" : "refused")
                    + ", target " + targetNs + " ns, preferred rate " + m.getPreferredUpdateRateNanos() + " ns");
        } catch (RuntimeException e) {
            log.log("perf hint: " + e);
        }
    }

    /**
     * One log line of telemetry: qemu's main thread (as before), then the busiest qemu threads with their CPU share
     * over the interval since the last call, the core they last ran on and that core's clock, then the thermal state.
     * The main thread is NOT the engine (that runs on a thread of its own), so the per-thread part is what says
     * which thread is the bottleneck and whether it sits on a throttled core.
     */
    String cpuLine() {
        return mainCpu() + threadLine() + thermalLine();
    }

    private final java.util.Map<Integer, Long> prevTicks = new java.util.HashMap<>();
    private long prevThreadsAt;

    private static final class Row {
        String name;
        int tid, cpu;
        double pct;
    }

    private String threadLine() {
        int p = pid;
        if (p <= 0) return "";
        try {
            String[] names = new File("/proc/" + p + "/task").list();
            if (names == null) return "";
            long now = System.nanoTime();
            double dt = prevThreadsAt == 0 ? 0 : (now - prevThreadsAt) / 1e9;
            List<Row> rows = new ArrayList<>();
            double total = 0;
            for (String n : names) {
                int tid;
                try { tid = Integer.parseInt(n); } catch (NumberFormatException e) { continue; }
                String stat;
                try { stat = readFirstLine("/proc/" + p + "/task/" + tid + "/stat"); } catch (java.io.IOException e) { continue; }
                int open = stat.indexOf('('), close = stat.lastIndexOf(')');
                if (open < 0 || close < open) continue;
                String[] f = stat.substring(close + 2).split(" ");
                long ticks = Long.parseLong(f[11]) + Long.parseLong(f[12]);    // utime + stime (fields 14, 15)
                Long before = prevTicks.put(tid, ticks);
                if (before == null || dt <= 0) continue;
                Row r = new Row();
                r.name = stat.substring(open + 1, close);
                r.tid = tid;
                r.cpu = Integer.parseInt(f[36]);
                r.pct = (ticks - before) * 10.0 / (dt * 1000.0) * 100.0;      // CLK_TCK is 100: one tick = 10 ms
                total += r.pct;
                rows.add(r);
            }
            prevThreadsAt = now;
            java.util.Collections.sort(rows, (a, b) -> Double.compare(b.pct, a.pct));
            StringBuilder sb = new StringBuilder(String.format(java.util.Locale.US, " | thr total %.0f%%:", total));
            int shown = 0;
            for (Row r : rows) {
                if (r.pct < 3 || shown++ >= 7) break;
                long cur = -1, max = -1;
                try {
                    String base = "/sys/devices/system/cpu/cpu" + r.cpu + "/cpufreq/";
                    cur = Long.parseLong(readFirstLine(base + "scaling_cur_freq").trim()) / 1000;
                    max = Long.parseLong(readFirstLine(base + "scaling_max_freq").trim()) / 1000;
                } catch (Exception ignored) { }
                sb.append(String.format(java.util.Locale.US, " %s/%d %.0f%%@cpu%d %d/%dMHz uclamp %s", r.name, r.tid, r.pct,
                        r.cpu, cur, max, uclampMax(p, r.tid)));
            }
            return sb.toString();
        } catch (Exception e) {
            return " | thr ?";
        }
    }

    /** The thread's effective uclamp.max (0..1024: the ceiling the scheduler puts on the frequency it may pick). */
    private static String uclampMax(int pid, int tid) {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader("/proc/" + pid + "/task/" + tid + "/sched"))) {
            String l;
            while ((l = r.readLine()) != null) {
                if (l.contains("uclamp.max") && (l.contains("effective") || !l.contains("requested"))) {
                    return l.substring(l.indexOf(':') + 1).trim();
                }
            }
        } catch (Exception ignored) { }
        return "?";
    }

    /** Thermal status (0 none .. 6 shutdown) and the headroom forecast (1.0 = throttling begins), both from the OS. */
    private String thermalLine() {
        try {
            android.os.PowerManager pm = ctx.getSystemService(android.os.PowerManager.class);
            if (pm == null) return "";
            String s = " | thermal " + pm.getCurrentThermalStatus();
            if (Build.VERSION.SDK_INT >= 30) {
                float h = pm.getThermalHeadroom(10);
                s += Float.isNaN(h) ? " headroom ?" : String.format(java.util.Locale.US, " headroom %.2f", h);
            }
            return s;
        } catch (Exception e) {
            return "";
        }
    }

    /**
     * Where qemu's main thread (tid == pid) last ran and at what clock: "cpu6 2649/3072 MHz".
     * The frequency limiter, not the game, is what most often decides the frame rate, so the log says it.
     */
    private String mainCpu() {
        int p = pid;
        if (p <= 0) return "";
        try {
            String stat = readFirstLine("/proc/" + p + "/task/" + p + "/stat");
            // Field 39 (processor) counted after the ")" that closes the command name.
            String[] f = stat.substring(stat.lastIndexOf(')') + 2).split(" ");
            int cpu = Integer.parseInt(f[36]);
            String base = "/sys/devices/system/cpu/cpu" + cpu + "/cpufreq/";
            long cur = Long.parseLong(readFirstLine(base + "scaling_cur_freq").trim()) / 1000;
            long max = Long.parseLong(readFirstLine(base + "scaling_max_freq").trim()) / 1000;
            return "cpu" + cpu + " " + cur + "/" + max + " MHz";
        } catch (Exception e) {
            return "cpu ?";
        }
    }

    private static String readFirstLine(String path) throws java.io.IOException {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(path))) {
            String l = r.readLine();
            return l == null ? "" : l;
        }
    }

    /** Called once per presented frame. */
    void frame() {
        long now = System.nanoTime();
        long d = now - lastFrame;
        lastFrame = now;
        Object s = session;
        if (s == null || Build.VERSION.SDK_INT < 31 || d <= 0 || d > 1_000_000_000L) return;
        try {
            PerformanceHintManager.Session ss = (PerformanceHintManager.Session) s;
            ss.reportActualWorkDuration(d);
            // Frames well over target: say the load just went up (API 33), at most twice a second.
            if (Build.VERSION.SDK_INT >= 33 && d > targetNs + targetNs / 4 && now - lastLoadUp > 500_000_000L) {
                lastLoadUp = now;
                loadUp(ss);
            }
        } catch (RuntimeException ignored) {
        }
    }

    private long lastLoadUp;
    private java.lang.reflect.Method sendHint;
    private boolean sendHintMissing;

    /** Session.sendHint(CPU_LOAD_UP): not in the SDK stubs this is built against, so by reflection. */
    private void loadUp(PerformanceHintManager.Session s) {
        if (sendHintMissing) return;
        try {
            if (sendHint == null) sendHint = s.getClass().getMethod("sendHint", int.class);
            sendHint.invoke(s, 0 /* Session.CPU_LOAD_UP */);
        } catch (Exception e) {
            sendHintMissing = true;
            log.log("perf hint: sendHint: " + e);
        }
    }
}
