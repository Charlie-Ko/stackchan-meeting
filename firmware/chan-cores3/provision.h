#pragma once

// ============================================================
//  provision.h · 手机配网（AP + 网页）
//
//  流程：
//    1. 设备开热点 Chan-XXXX（密码 changeme88）
//    2. 手机连上 → 自动/手动打开 http://192.168.4.1
//    3. 填 WiFi + 网关 → 保存 → 写 NVS → 自动重启
//
//  只用 Arduino 自带的 WebServer / DNSServer / WiFi，
//  不引第三方配网库，行为可控。
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "config.h"
#include "settings.h"

namespace provision {

static WebServer server(80);
static DNSServer dns;
static bool      gRunning = false;
static bool      gSaved   = false;
static bool      gExitReq = false;
static uint32_t  gLastAct = 0;
static String    gApSsid;
static bool      gRoutesReady = false;   // 路由只注册一次（多次进出配网不重复挂）

// ---------------- 工具 ----------------
static String apSsid() {
  if (gApSsid.length()) return gApSsid;
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char b[32];
  snprintf(b, sizeof(b), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
  gApSsid = String(b);
  return gApSsid;
}

static String esc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

// ---------------- 页面模板 ----------------
static const char PAGE_TPL[] = R"HTML(<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Chan 会议记录 · 配网</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#0b1220;color:#e8eef7;padding:18px;
 font:15px/1.6 -apple-system,BlinkMacSystemFont,"PingFang SC","Microsoft YaHei",sans-serif}
.card{max-width:440px;margin:0 auto;background:#16233a;border:1px solid #2a3a55;
 border-radius:16px;padding:20px}
h1{margin:0 0 4px;font-size:20px;font-weight:700}
.sub{color:#8ca0bc;font-size:13px;margin:0 0 16px}
label{display:block;font-size:13px;color:#8ca0bc;margin:14px 0 6px}
input,select,textarea{width:100%;padding:12px;background:#0b1220;border:1px solid #2a3a55;
 border-radius:10px;color:#e8eef7;font-size:16px;   /* 必须 ≥16px：iOS 会为小于 16px 的输入框自动放大页面，放大后光标点不准 */
 -webkit-appearance:none;-webkit-user-select:text;user-select:text;caret-color:#22c55e}
textarea{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:16px;line-height:1.7;
 resize:vertical;min-height:110px}
input:focus,select:focus,textarea:focus{outline:none;border-color:#22c55e}
.row{display:flex;gap:8px}
.row input{flex:1}
button{padding:12px 14px;border:1px solid #2a3a55;background:#1e2e4a;color:#e8eef7;
 border-radius:10px;font-size:14px;white-space:nowrap}
button:active{background:#2a3a55}
button.primary{width:100%;margin-top:22px;background:#22c55e;border:none;color:#06170c;
 font-weight:700;font-size:16px;padding:14px}
details{margin-top:18px;border-top:1px solid #2a3a55;padding-top:12px}
summary{color:#8ca0bc;font-size:13px;cursor:pointer}
.tip{margin-top:16px;font-size:12px;color:#8ca0bc;line-height:1.7}
.badge{display:inline-block;background:#1e2e4a;border:1px solid #2a3a55;border-radius:6px;
 padding:2px 8px;font-size:12px;color:#8ca0bc;margin-right:6px}
</style></head><body>
<div class="card">
<h1>Chan 会议记录</h1>
<p class="sub">配置联网后自动重启，无需重新烧录</p>
<form method="post" action="/save">
<label>WiFi 名称</label>
<div class="row">
<input name="ssid" id="ssid" list="ssidlist" placeholder="点右侧扫描，或手动输入"
 required value="%SSID%" autocomplete="off">
<button type="button" id="scanbtn" onclick="scan()">扫描</button>
</div>
<datalist id="ssidlist"></datalist>
<label>WiFi 密码</label>
<input name="pass" id="pass" type="password" placeholder="开放网络留空" autocomplete="off">
<details>
<summary>高级：网关设置（一般不用改）</summary>
<label>网关地址</label><input name="host" value="%HOST%">
<label>端口</label><input name="port" type="number" value="%PORT%">
<label>路径</label><input name="path" value="%PATH%">
<label>设备令牌</label><input name="token" value="%TOKEN%">
</details>
<label>专有名词（一行一个，识别到就加粗显示）</label>
<textarea name="terms" rows="5" spellcheck="false"
 placeholder="张工&#10;王经理&#10;联调&#10;评审,评审会">%TERMS%</textarea>
<p class="tip">这一栏会存进设备，优先于 SD 卡和内置词表。<br>
同一个词的别名用逗号分隔，例如 <b>前端,前端开发</b>。留空 = 用 SD 卡 /terms.txt 或内置词表。</p>
<button class="primary" type="submit">保存并重启</button>
</form>
<p class="tip"><span class="badge">%APNAME%</span>热点密码 %APPASS%<br>
配网 10 分钟无操作会自动退出，设备回到录音模式。</p>
</div>
<script>
// 只手动扫描：不要在 focus 时自动扫。ESP32 的 web server 是单线程，
// scanNetworks() 会阻塞几秒，期间页面点哪儿都没反应（这就是当初"光标挪不动"的一半原因）。
async function scan(){
  const dl=document.getElementById('ssidlist');
  dl.innerHTML='';
  const btn=document.getElementById('scanbtn');
  btn.disabled=true; btn.textContent='扫描中…';
  try{
    const r=await fetch('/scan'); const a=await r.json();
    a.forEach(n=>{const o=document.createElement('option');o.value=n.s;
      o.label=n.s+'  ('+n.r+'dBm)';dl.appendChild(o);});
    if(a.length===0) alert('没扫到 WiFi，请靠近路由器再试');
  }catch(e){alert('扫描失败：'+e);}
  finally{btn.disabled=false; btn.textContent='扫描';}
}
// 注意：不要在这里自动 focus()。iOS 上页面加载即抢占焦点会导致键盘弹出、
// 视口被顶飞，之后点输入框时落点全错，看起来就是"光标没法移动"。
</script>
</body></html>)HTML";

static const char OK_TPL[] = R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>保存成功</title>
<style>body{margin:0;background:#0b1220;color:#e8eef7;padding:40px 20px;
 font:15px/1.8 -apple-system,"PingFang SC",sans-serif;text-align:center}
.box{max-width:400px;margin:0 auto;background:#16233a;border:1px solid #2a3a55;
 border-radius:16px;padding:28px}
h2{margin:0 0 10px;color:#22c55e;font-size:20px}
p{color:#8ca0bc;margin:8px 0}
b{color:#e8eef7}</style></head><body>
<div class="box"><h2>配置已保存</h2>
<p>设备正在重启并连接 <b>%SSID%</b></p>
<p>热点即将关闭，可断开手机 WiFi</p></div></body></html>)HTML";

// ---------------- 请求处理 ----------------
static void touch() { gLastAct = millis(); }

static void handleRoot() {
  touch();
  settings::Config &c = settings::get();
  String page = PAGE_TPL;
  page.replace("%SSID%", esc(c.wifiSsid));
  page.replace("%HOST%", esc(c.gwHost));
  page.replace("%PORT%", String(c.gwPort));
  page.replace("%PATH%", esc(c.gwPath));
  page.replace("%TOKEN%", esc(c.gwToken));
  page.replace("%TERMS%", esc(c.termsText));
  page.replace("%APNAME%", apSsid());
  page.replace("%APPASS%", AP_PASS);
  server.send(200, "text/html; charset=utf-8", page);
}

static void handleScan() {
  touch();
  int n = WiFi.scanNetworks();
  String j = "[";
  int emitted = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;          // 隐藏网络跳过
    if (emitted++) j += ",";
    j += "{\"s\":\"" + esc(s) + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
    if (emitted >= 15) break;
  }
  j += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

static void handleSave() {
  touch();
  String ssid  = server.arg("ssid");
  String pass  = server.arg("pass");
  String host  = server.arg("host");
  String portS = server.arg("port");
  String path  = server.arg("path");
  String token = server.arg("token");
  String termsTxt = server.arg("terms");

  ssid.trim();
  host.trim();
  path.trim();
  token.trim();
  termsTxt.trim();

  if (ssid.length() == 0) {
    server.send(400, "text/html; charset=utf-8",
                "<meta charset=utf-8><h3>WiFi 名称不能为空</h3>");
    return;
  }
  if (host.length() == 0)  host = DEFAULT_GW_HOST;
  if (path.length() == 0)  path = DEFAULT_GW_PATH;
  uint16_t port = (uint16_t)portS.toInt();
  if (port == 0) port = DEFAULT_GW_PORT;

  settings::saveAll(ssid, pass, host, port, path, token);
  settings::setTerms(termsTxt);
  gSaved = true;

  String ok = OK_TPL;
  ok.replace("%SSID%", esc(ssid));
  server.send(200, "text/html; charset=utf-8", ok);

  Serial.printf("[PROV] saved ssid=%s host=%s:%u%s\n",
                ssid.c_str(), host.c_str(), port, path.c_str());
  delay(1500);          // 让响应先发出去
  ESP.restart();
}

static void handleExit() {
  touch();
  gExitReq = true;
  server.send(200, "text/html; charset=utf-8",
              "<meta charset=utf-8><h3>已退出配网</h3>设备返回录音模式。");
}

static void handleInfo() {
  touch();
  settings::Config &c = settings::get();
  String j = "{";
  j += "\"ap\":\"" + apSsid() + "\",";
  j += "\"clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  j += "\"host\":\"" + esc(c.gwHost) + "\",";
  j += "\"port\":" + String(c.gwPort) + ",";
  j += "\"device\":\"" + esc(c.deviceName) + "\",";
  j += "\"version\":\"" FW_VERSION "\"";
  j += "}";
  server.send(200, "application/json", j);
}

// ---------------- 生命周期 ----------------
static void begin() {
  if (gRunning) return;
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);

  String ssid = apSsid();
  bool ok = WiFi.softAP(ssid.c_str(), AP_PASS, AP_CHANNEL, 0, 4);
  delay(150);
  Serial.printf("[PROV] AP %s (%s) %s\n", ssid.c_str(),
                WiFi.softAPIP().toString().c_str(), ok ? "ok" : "FAIL");

  dns.start(53, "*", WiFi.softAPIP());

  if (!gRoutesReady) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/info", HTTP_GET, handleInfo);
    server.on("/exit", HTTP_GET, handleExit);
    server.on("/save", HTTP_POST, handleSave);
    // 各家系统的联网探测路径，一律回配置页，触发"登录网络"提示
    server.on("/generate_204", HTTP_GET, handleRoot);
    server.on("/gen_204", HTTP_GET, handleRoot);
    server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
    server.on("/library/test/success.html", HTTP_GET, handleRoot);
    server.on("/ncsi.txt", HTTP_GET, handleRoot);
    server.on("/connecttest.txt", HTTP_GET, handleRoot);
    server.onNotFound(handleRoot);
    gRoutesReady = true;
  }

  server.begin();
  gRunning = true;
  gExitReq = false;
  gLastAct = millis();
}

static void stop() {
  if (!gRunning) return;
  dns.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  gRunning = false;
  Serial.println("[PROV] stopped");
}

static void loop() {
  if (!gRunning) return;
  dns.processNextRequest();
  server.handleClient();
  if (!gSaved && millis() - gLastAct > PROVISION_IDLE_MS) {
    Serial.println("[PROV] idle timeout");
    gExitReq = true;
  }
}

static inline bool running()       { return gRunning; }
static inline bool exitRequested() { return gExitReq; }
static inline bool saved()         { return gSaved; }
static inline String name()        { return apSsid(); }
static inline String ip()          { return WiFi.softAPIP().toString(); }
static inline int clients()        { return WiFi.softAPgetStationNum(); }

}  // namespace provision
