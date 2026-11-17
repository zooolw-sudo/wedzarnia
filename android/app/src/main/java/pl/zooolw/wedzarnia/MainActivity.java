package pl.zooolw.wedzarnia;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.net.Uri;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import java.net.InetAddress;

/**
 * Ekran aplikacji Wędzarnia: interfejs z assets (index.html) w WebView.
 * Dane i sterowanie idą bezpośrednio do ESP32 w sieci lokalnej (HTTP).
 * Aplikacja dodaje to, czego sama strona nie umie: wyszukanie ESP32 w sieci (mDNS/NSD),
 * tryb wyświetlacza ściennego (poziomo, pełny ekran, ekran nie gaśnie) i dźwięk bez dotknięcia.
 */
public class MainActivity extends Activity {

    private WebView web;
    private volatile boolean fullscreen = true;
    private volatile boolean keepOn = true;

    private NsdManager nsd;
    private NsdManager.DiscoveryListener discovery;
    private WifiManager.MulticastLock multicastLock;
    private volatile boolean resolving = false;
    private volatile String foundHost = "";

    @SuppressLint({"SetJavaScriptEnabled", "AddJavascriptInterface"})
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(0xFF151C2B);
        getWindow().setNavigationBarColor(0xFF0D121B);
        fullscreen = getSharedPreferences("ui", MODE_PRIVATE).getBoolean("fullscreen", true);
        keepOn = getSharedPreferences("ui", MODE_PRIVATE).getBoolean("keepOn", true);
        foundHost = getSharedPreferences("ui", MODE_PRIVATE).getString("foundHost", "");
        // tablet (najmniejszy bok co najmniej 600 dp) trzymamy poziomo; telefon obraca się swobodnie
        if (getResources().getConfiguration().smallestScreenWidthDp >= 600) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        }
        applyFullscreen();
        applyKeepOn();

        web = new WebView(this);
        web.setBackgroundColor(0xFF0D121B);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(true);
        // strona z assets odpytuje ESP32 po HTTP w sieci lokalnej
        s.setAllowUniversalAccessFromFileURLs(true);
        // alarm końca wędzenia ma zabrzmieć bez dotykania ekranu
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setLoadsImagesAutomatically(true);
        web.addJavascriptInterface(new Bridge(), "AndroidWedzarnia");
        web.setWebViewClient(new WebViewClient() {
            @SuppressWarnings("deprecation")
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, String url) {
                if (url.startsWith("file:")) return false;
                try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))); } catch (Exception ignored) { }
                return true;
            }
        });
        setContentView(web);
        web.loadUrl("file:///android_asset/index.html");
    }

    /** Most między stroną a aplikacją. */
    private class Bridge {
        @JavascriptInterface
        public boolean isNight() { return isNightMode(); }

        @JavascriptInterface
        public String getFoundHost() { return foundHost; }

        @JavascriptInterface
        public void discover() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() { startDiscovery(); }
            });
        }

        @JavascriptInterface
        public boolean getFullscreen() { return fullscreen; }

        @JavascriptInterface
        public void setFullscreen(final boolean on) {
            fullscreen = on;
            getSharedPreferences("ui", MODE_PRIVATE).edit().putBoolean("fullscreen", on).apply();
            runOnUiThread(new Runnable() {
                @Override
                public void run() { applyFullscreen(); }
            });
        }

        @JavascriptInterface
        public boolean getKeepOn() { return keepOn; }

        @JavascriptInterface
        public void setKeepOn(final boolean on) {
            keepOn = on;
            getSharedPreferences("ui", MODE_PRIVATE).edit().putBoolean("keepOn", on).apply();
            runOnUiThread(new Runnable() {
                @Override
                public void run() { applyKeepOn(); }
            });
        }
    }

    // ---------- wyszukiwanie ESP32 w sieci (usługa _http._tcp o nazwie „wedzarnia”) ----------

    private void startDiscovery() {
        stopDiscovery();
        try {
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wm != null) {
                multicastLock = wm.createMulticastLock("wedzarnia");
                multicastLock.setReferenceCounted(false);
                multicastLock.acquire();
            }
        } catch (Exception ignored) { }
        try {
            nsd = (NsdManager) getSystemService(Context.NSD_SERVICE);
            discovery = new NsdManager.DiscoveryListener() {
                @Override public void onStartDiscoveryFailed(String type, int code) { }
                @Override public void onStopDiscoveryFailed(String type, int code) { }
                @Override public void onDiscoveryStarted(String type) { }
                @Override public void onDiscoveryStopped(String type) { }
                @Override public void onServiceLost(NsdServiceInfo info) { }
                @Override
                public void onServiceFound(NsdServiceInfo info) {
                    String name = info.getServiceName();
                    if (name != null && name.toLowerCase().startsWith("wedzarnia")) resolve(info);
                }
            };
            nsd.discoverServices("_http._tcp.", NsdManager.PROTOCOL_DNS_SD, discovery);
        } catch (Exception e) {
            discovery = null;
        }
    }

    @SuppressWarnings("deprecation")
    private void resolve(NsdServiceInfo info) {
        if (resolving || nsd == null) return;
        resolving = true;
        try {
            nsd.resolveService(info, new NsdManager.ResolveListener() {
                @Override
                public void onResolveFailed(NsdServiceInfo i, int code) { resolving = false; }

                @Override
                public void onServiceResolved(NsdServiceInfo i) {
                    resolving = false;
                    InetAddress h = i.getHost();
                    if (h == null) return;
                    String ip = h.getHostAddress();
                    if (ip == null || ip.contains(":")) return;   // tylko IPv4
                    final String host = i.getPort() == 80 || i.getPort() <= 0 ? ip : ip + ":" + i.getPort();
                    foundHost = host;
                    getSharedPreferences("ui", MODE_PRIVATE).edit().putString("foundHost", host).apply();
                    runOnUiThread(new Runnable() {
                        @Override
                        public void run() {
                            if (web != null) web.evaluateJavascript("window.__found&&window.__found('" + host + "')", null);
                        }
                    });
                }
            });
        } catch (Exception e) {
            resolving = false;
        }
    }

    private void stopDiscovery() {
        try { if (nsd != null && discovery != null) nsd.stopServiceDiscovery(discovery); } catch (Exception ignored) { }
        discovery = null;
        resolving = false;
        try { if (multicastLock != null && multicastLock.isHeld()) multicastLock.release(); } catch (Exception ignored) { }
        multicastLock = null;
    }

    // ---------- ekran ----------

    private boolean isNightMode() {
        return (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (web != null) web.evaluateJavascript("window.__nightMode&&window.__nightMode()", null);
    }

    @Override
    protected void onResume() {
        super.onResume();
        startDiscovery();
        if (web != null) {
            web.onResume();
            web.evaluateJavascript("window.__nightMode&&window.__nightMode()", null);
        }
    }

    @Override
    protected void onPause() {
        // WebView działa dalej (odpytuje ESP32 i może dać alarm), zatrzymujemy tylko wyszukiwanie
        stopDiscovery();
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyFullscreen();
    }

    private void applyKeepOn() {
        if (keepOn) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @SuppressWarnings("deprecation")
    private void applyFullscreen() {
        Window w = getWindow();
        boolean on = fullscreen;
        if (on) w.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        else w.clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = w.getAttributes();
            lp.layoutInDisplayCutoutMode = on
                    ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT;
            w.setAttributes(lp);
        }
        if (Build.VERSION.SDK_INT >= 30) {
            w.setDecorFitsSystemWindows(!on);
            WindowInsetsController c = w.getInsetsController();
            if (c != null) {
                if (on) {
                    c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                    c.hide(WindowInsets.Type.systemBars());
                } else {
                    c.show(WindowInsets.Type.systemBars());
                }
            }
        } else {
            View d = w.getDecorView();
            d.setSystemUiVisibility(on
                    ? View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                      | View.SYSTEM_UI_FLAG_FULLSCREEN
                      | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                      | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                      | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                      | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    : View.SYSTEM_UI_FLAG_VISIBLE);
        }
    }

    @Override
    protected void onDestroy() {
        stopDiscovery();
        if (web != null) {
            web.destroy();
            web = null;
        }
        super.onDestroy();
    }
}
