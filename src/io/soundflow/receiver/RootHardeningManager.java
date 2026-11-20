package io.soundflow.receiver;

import android.content.Context;
import android.content.pm.ApplicationInfo;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

/**
 * Inspects and applies Root/Magisk hardening so SoundFlow can be promoted into
 * a privileged system service with HyperOS PowerKeeper & Doze immunity.
 */
public final class RootHardeningManager {
    public static final String MODULE_DIR = "/data/adb/modules/soundflow_receiver";
    public static final String GUARD_SCRIPT = "/data/adb/service.d/soundflow_receiver_guard.sh";

    public static final class Status {
        public boolean rootAvailable;
        public String magiskVersion = "";
        public boolean isPrivilegedSystemApp;
        public boolean inDozeWhitelist;
        public boolean inPerfProtectList;
        public boolean moduleInstalled;
        public boolean guardInstalled;
        public String summary = "";
    }

    public static final class CommandResult {
        public final boolean ok;
        public final String output;

        public CommandResult(boolean ok, String output) {
            this.ok = ok;
            this.output = output;
        }
    }

    private RootHardeningManager() {}

    public static Status inspect(Context context) {
        Status st = new Status();
        ApplicationInfo ai = context.getApplicationInfo();
        st.isPrivilegedSystemApp = ai != null
                && ((ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0)
                && ai.sourceDir != null
                && ai.sourceDir.contains("/priv-app/");

        CommandResult suProbe = runSu("id -u && (magisk -v 2>/dev/null || echo none)");
        if (!suProbe.ok || !suProbe.output.contains("0")) {
            st.rootAvailable = false;
            st.summary = "未检测到 Root (su) 权限，当前使用标准 App 前台服务+防杀保护";
            return st;
        }
        st.rootAvailable = true;
        String[] lines = suProbe.output.trim().split("\\r?\\n");
        if (lines.length > 1 && !"none".equals(lines[1].trim())) {
            st.magiskVersion = lines[1].trim();
        }

        String pkg = context.getPackageName();
        String script = ""
                + "echo \"WL=$(dumpsys deviceidle whitelist 2>/dev/null | grep -c " + pkg + ")\"; "
                + "echo \"PP=$(settings get system perf_proc_protect_list 2>/dev/null | grep -c " + pkg + ")\"; "
                + "if [ -d " + MODULE_DIR + " ] && [ ! -f " + MODULE_DIR + "/remove ]; then echo MOD=1; else echo MOD=0; fi; "
                + "if [ -x " + GUARD_SCRIPT + " ]; then echo GRD=1; else echo GRD=0; fi";
        CommandResult details = runSu(script);
        for (String line : details.output.split("\\r?\\n")) {
            if (line.startsWith("WL=") && !"WL=0".equals(line.trim())) st.inDozeWhitelist = true;
            if (line.startsWith("PP=") && !"PP=0".equals(line.trim())) st.inPerfProtectList = true;
            if ("MOD=1".equals(line.trim())) st.moduleInstalled = true;
            if ("GRD=1".equals(line.trim())) st.guardInstalled = true;
        }

        StringBuilder sb = new StringBuilder();
        sb.append("Root 已就绪").append(st.magiskVersion.isEmpty() ? "" : " (" + st.magiskVersion + ")");
        sb.append(" · 系统固化模块: ").append(st.moduleInstalled ? "已部署" : "未部署");
        sb.append(" · 电池白名单: ").append(st.inDozeWhitelist ? "已豁免" : "未豁免");
        sb.append(" · HyperOS 保护列表: ").append(st.inPerfProtectList ? "已加入" : "未加入");
        st.summary = sb.toString();
        return st;
    }

    public static CommandResult applyHotProtection(Context context) {
        String pkg = context.getPackageName();
        String procSvc = pkg + ":receiver";
        String cmd = ""
                + "dumpsys deviceidle whitelist +" + pkg + " >/dev/null 2>&1; "
                + "cmd appops set " + pkg + " RUN_IN_BACKGROUND allow >/dev/null 2>&1; "
                + "cmd appops set " + pkg + " RUN_ANY_IN_BACKGROUND allow >/dev/null 2>&1; "
                + "cmd appops set " + pkg + " START_FOREGROUND allow >/dev/null 2>&1; "
                + "CUR=$(settings get system perf_proc_protect_list 2>/dev/null); "
                + "[ \"$CUR\" = \"null\" ] && CUR=''; "
                + "NEW=\"$CUR\"; "
                + "case \",$NEW,\" in *,\"" + pkg + "\",*) ;; *) NEW=\"${NEW:+$NEW,}" + pkg + "\";; esac; "
                + "case \",$NEW,\" in *,\"" + procSvc + "\",*) ;; *) NEW=\"${NEW:+$NEW,}" + procSvc + "\";; esac; "
                + "settings put system perf_proc_protect_list \"$NEW\"; "
                + "am set-standby-bucket " + pkg + " active >/dev/null 2>&1 || true; "
                + "echo OK";
        return runSu(cmd);
    }

    public static CommandResult installMagiskModule(Context context) {
        try {
            File stage = new File(context.getCacheDir(), "magisk_stage");
            deleteRecursive(stage);
            File privDir = new File(stage, "system/product/priv-app/SoundFlowReceiver");
            File permDir = new File(stage, "system/product/etc/permissions");
            File commonDir = new File(stage, "common");
            privDir.mkdirs();
            permDir.mkdirs();
            commonDir.mkdirs();

            copyAsset(context, "magisk/module.prop", new File(stage, "module.prop"));
            copyAsset(context, "magisk/system/product/etc/permissions/privapp-permissions-io.soundflow.receiver.xml",
                    new File(permDir, "privapp-permissions-io.soundflow.receiver.xml"));
            copyAsset(context, "magisk/common/service.sh", new File(commonDir, "service.sh"));

            String srcApk = context.getApplicationInfo().sourceDir;
            String cmd = ""
                    + "set -e; "
                    + "rm -rf " + MODULE_DIR + "; "
                    + "mkdir -p " + MODULE_DIR + "/system/product/priv-app/SoundFlowReceiver "
                    + MODULE_DIR + "/system/product/etc/permissions /data/adb/service.d; "
                    + "cp -f " + shellQuote(new File(stage, "module.prop").getAbsolutePath()) + " " + MODULE_DIR + "/module.prop; "
                    + "cp -f " + shellQuote(new File(permDir, "privapp-permissions-io.soundflow.receiver.xml").getAbsolutePath()) + " "
                    + MODULE_DIR + "/system/product/etc/permissions/privapp-permissions-io.soundflow.receiver.xml; "
                    + "cp -f " + shellQuote(srcApk) + " " + MODULE_DIR + "/system/product/priv-app/SoundFlowReceiver/SoundFlowReceiver.apk; "
                    + "cp -f " + shellQuote(new File(commonDir, "service.sh").getAbsolutePath()) + " " + MODULE_DIR + "/service.sh; "
                    + "cp -f " + shellQuote(new File(commonDir, "service.sh").getAbsolutePath()) + " " + GUARD_SCRIPT + "; "
                    + "chmod 0755 " + MODULE_DIR + " " + MODULE_DIR + "/system " + MODULE_DIR + "/system/product "
                    + MODULE_DIR + "/system/product/priv-app " + MODULE_DIR + "/system/product/priv-app/SoundFlowReceiver "
                    + MODULE_DIR + "/system/product/etc " + MODULE_DIR + "/system/product/etc/permissions "
                    + MODULE_DIR + "/service.sh " + GUARD_SCRIPT + "; "
                    + "chmod 0644 " + MODULE_DIR + "/module.prop "
                    + MODULE_DIR + "/system/product/priv-app/SoundFlowReceiver/SoundFlowReceiver.apk "
                    + MODULE_DIR + "/system/product/etc/permissions/privapp-permissions-io.soundflow.receiver.xml; "
                    + "echo INSTALLED";
            CommandResult res = runSu(cmd);
            if (res.ok) {
                applyHotProtection(context);
            }
            return res;
        } catch (Throwable t) {
            return new CommandResult(false, t.toString());
        }
    }

    public static CommandResult uninstallMagiskModule() {
        return runSu("rm -rf " + MODULE_DIR + " " + GUARD_SCRIPT + " && echo REMOVED");
    }

    public static CommandResult stopPortConflictReceiver() {
        return runSu("am force-stop cn.padreceiver.audio >/dev/null 2>&1 || true; echo STOPPED");
    }

    private static void copyAsset(Context ctx, String assetPath, File dst) throws Exception {
        try (InputStream in = ctx.getAssets().open(assetPath);
             FileOutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
        }
    }

    private static void deleteRecursive(File f) {
        if (f == null || !f.exists()) return;
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) deleteRecursive(c);
        }
        f.delete();
    }

    private static String shellQuote(String s) {
        return "'" + s.replace("'", "'\"'\"'") + "'";
    }

    private static CommandResult runSu(String command) {
        Process p = null;
        try {
            p = new ProcessBuilder("su", "-c", command).redirectErrorStream(true).start();
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            InputStream is = p.getInputStream();
            long deadline = System.currentTimeMillis() + 8000;
            while (System.currentTimeMillis() < deadline) {
                while (is.available() > 0) {
                    int n = is.read(buf);
                    if (n > 0) out.write(buf, 0, n);
                }
                try {
                    int exit = p.exitValue();
                    while (is.available() > 0) {
                        int n = is.read(buf);
                        if (n > 0) out.write(buf, 0, n);
                    }
                    return new CommandResult(exit == 0, out.toString(StandardCharsets.UTF_8.name()));
                } catch (IllegalThreadStateException running) {
                    Thread.sleep(40);
                }
            }
            p.destroyForcibly();
            return new CommandResult(false, "su timeout");
        } catch (Throwable t) {
            return new CommandResult(false, t.getMessage() == null ? "su unavailable" : t.getMessage());
        } finally {
            if (p != null) {
                try { p.waitFor(100, TimeUnit.MILLISECONDS); } catch (Throwable ignored) {}
            }
        }
    }
}

