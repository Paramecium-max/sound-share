package com.mishare.miplay;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.projection.MediaProjectionManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import org.json.JSONObject;

/**
 * 独立 Android 入口：
 * - 所有网络与引擎初始化 100% 在后台线程执行，彻底消除主线程卡顿与 NetworkOnMainThreadException
 * - 通过原生同步 JavascriptInterface 暴露本地 WebSocket 配置，零竞态
 */
public class MainActivity extends Activity {
    private static final int REQ_MEDIA_PROJECTION = 2001;
    private static final int REQ_PICK_WAV = 2002;
    private static final int REQ_PERMISSIONS = 2003;

    private WebView webView;

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        requestRuntimePermissions();

        // 异步启动核心，绝不在 UI 主线程做任何网络 IO
        MultiRoomManager mgr = MultiRoomManager.getInstance();
        mgr.setUiActionDelegate(() -> runOnUiThread(this::requestMediaProjection));
        mgr.ensureStartedAsync();

        try {
            Intent svc = new Intent(this, MiPlayForegroundService.class);
            svc.setAction(MiPlayForegroundService.ACTION_START);
            startForegroundService(svc);
        } catch (Exception ignored) {}

        webView = new WebView(this);
        webView.setBackgroundColor(0xFF0B0F17);
        WebSettings ws = webView.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setAllowFileAccess(true);
        ws.setAllowFileAccessFromFileURLs(true);
        ws.setAllowUniversalAccessFromFileURLs(true);

        webView.addJavascriptInterface(new AndroidBridge(), "_androidMiPlayBridge");
        webView.setWebViewClient(new WebViewClient());

        setContentView(webView);
        webView.loadUrl("file:///android_asset/www/index.html");
    }

    private void requestRuntimePermissions() {
        java.util.ArrayList<String> need = new java.util.ArrayList<>();
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            need.add(Manifest.permission.RECORD_AUDIO);
        }
        if (Build.VERSION.SDK_INT >= 33 &&
                checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            need.add(Manifest.permission.POST_NOTIFICATIONS);
        }
        if (!need.isEmpty()) {
            requestPermissions(need.toArray(new String[0]), REQ_PERMISSIONS);
        }
    }

    private void requestMediaProjection() {
        MediaProjectionManager mpm = (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        if (mpm != null) {
            startActivityForResult(mpm.createScreenCaptureIntent(), REQ_MEDIA_PROJECTION);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_MEDIA_PROJECTION) {
            if (resultCode == RESULT_OK && data != null) {
                Intent svc = new Intent(this, MiPlayForegroundService.class);
                svc.setAction(MiPlayForegroundService.ACTION_ATTACH_PROJECTION);
                svc.putExtra(MiPlayForegroundService.EXTRA_RESULT_CODE, resultCode);
                svc.putExtra(MiPlayForegroundService.EXTRA_RESULT_DATA, data);
                startForegroundService(svc);
            } else {
                // 用户取消录屏抓音权限，退回同步校准音避免空等
                MultiRoomManager.getInstance().onMediaProjectionDenied();
            }
        } else if (requestCode == REQ_PICK_WAV && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) {
                new Thread(() -> MultiRoomManager.getInstance()
                        .onWavFileSelected(getApplicationContext(), uri, uri.getLastPathSegment()),
                        "MiPlay-LoadWav").start();
            }
        }
    }

    private class AndroidBridge {
        @JavascriptInterface
        public String getSidecarConfigJson() {
            try {
                MultiRoomManager mgr = MultiRoomManager.getInstance();
                mgr.ensureStartedAsync();
                int port = mgr.awaitWsPort(1500);
                JSONObject o = new JSONObject();
                o.put("port", port);
                o.put("token", mgr.getWsToken());
                return o.toString();
            } catch (Exception e) {
                return "{}";
            }
        }

        @JavascriptInterface
        public void openWavPicker() {
            runOnUiThread(() -> {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("audio/*");
                startActivityForResult(i, REQ_PICK_WAV);
            });
        }
    }
}
