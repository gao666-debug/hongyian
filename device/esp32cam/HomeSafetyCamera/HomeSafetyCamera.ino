#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "secrets.h"

// AI Thinker ESP32-CAM pin map; sensor is probed by esp_camera (OV3660 supported).
WebServer server(80);
WebServer streamServer(81);
SemaphoreHandle_t cameraLock;
String streamTicket;
bool streamAvailable = false;
bool cameraReady = false;
esp_err_t cameraError = ESP_OK;
uint32_t reconnectAt = 0;
#include "cloud_upload.h"

bool authorized() {
  if (server.authenticate(CAMERA_USER, CAMERA_PASSWORD)) return true;
  server.requestAuthentication();
  return false;
}

void snapshot() {
  if (!authorized()) return;
  if (!cameraReady) {
    server.send(503, "application/json", "{\"error\":\"camera_not_ready\"}");
    return;
  }
  if (xSemaphoreTake(cameraLock, pdMS_TO_TICKS(2000)) != pdTRUE) {
    server.send(503, "application/json", "{\"error\":\"camera_busy\"}");
    return;
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(cameraLock);
    server.send(503, "application/json", "{\"error\":\"capture_failed\"}");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", reinterpret_cast<const char *>(fb->buf), fb->len);
  esp_camera_fb_return(fb);
  xSemaphoreGive(cameraLock);
}

// Separate HTTP task keeps the controls responsive during a long-lived video stream.
void liveStream() {
  if (streamServer.arg("ticket") != streamTicket || streamTicket.isEmpty()) {
    streamServer.send(401, "text/plain", "Unauthorized");
    return;
  }
  if (!cameraReady) {
    streamServer.send(503, "text/plain", "Camera not ready");
    return;
  }
  WiFiClient client = streamServer.client();
  client.setTimeout(3);
  client.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n"
               "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
  while (client.connected() && WiFi.status() == WL_CONNECTED) {
    if (xSemaphoreTake(cameraLock, pdMS_TO_TICKS(500)) != pdTRUE) {
      delay(10);
      continue;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    bool sent = false;
    if (fb) {
      client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
      sent = client.write(fb->buf, fb->len) == fb->len;
      client.print("\r\n");
      esp_camera_fb_return(fb);
    }
    xSemaphoreGive(cameraLock);
    if (!sent) break;
    delay(100);
  }
  client.stop();
}

void streamTask(void *) {
  for (;;) {
    streamServer.handleClient();
    delay(2);
  }
}

void statusPage() {
  if (!authorized()) return;
  String json = "{\"firmware\":\"camera-v0.3\",\"camera_ready\":";
  json += cameraReady ? "true" : "false";
  json += ",\"camera_error\":" + String(static_cast<int>(cameraError));
  json += ",\"wifi_connected\":";
  json += WiFi.status() == WL_CONNECTED ? "true" : "false";
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":";
  json += String(WiFi.RSSI());
  json += ",\"psram\":" + String(ESP.getPsramSize());
  json += ",\"uptime_s\":" + String(millis() / 1000);
  if (cameraReady) json += ",\"sensor_pid\":" + String(esp_camera_sensor_get()->id.PID);
  json += ",\"cloud_online\":" + String(cloudOnline ? "true" : "false");
  json += ",\"cloud_code\":" + String(cloudLastCode);
  json += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void initCamera() {
  pinMode(4, OUTPUT); // Flash LED off; no microSD used in this baseline.
  digitalWrite(4, LOW);
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = 5; c.pin_d1 = 18; c.pin_d2 = 19; c.pin_d3 = 21;
  c.pin_d4 = 36; c.pin_d5 = 39; c.pin_d6 = 34; c.pin_d7 = 35;
  c.pin_xclk = 0; c.pin_pclk = 22; c.pin_vsync = 25; c.pin_href = 23;
  c.pin_sccb_sda = 26; c.pin_sccb_scl = 27;
  c.pin_pwdn = 32; c.pin_reset = -1;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = psramFound() ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  c.jpeg_quality = 12;
  c.fb_count = 1;
  c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  cameraError = esp_camera_init(&c);
  cameraReady = cameraError == ESP_OK;
  if (!cameraReady) {
    Serial.printf("[camera] init failed: 0x%x; status page remains available\n", cameraError);
    return;
  }
  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("[camera] ready PID=0x%x PSRAM=%u bytes\n", s->id.PID, ESP.getPsramSize());
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  } else {
    Serial.println("[camera] detected sensor is not OV3660; verify actual module");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[boot] YiHongAn Camera v0.3");
  cameraLock = xSemaphoreCreateMutex();
  if (!cameraLock) {
    Serial.println("[camera] mutex allocation failed");
    return;
  }
  for (int i = 0; i < 4; ++i) {
    char part[9];
    snprintf(part, sizeof(part), "%08x", esp_random());
    streamTicket += part;
  }
  initCamera();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  startCloudUpload();
  server.on("/", HTTP_GET, []() {
    if (!authorized()) return;
    server.send(200, "text/html; charset=utf-8", R"HTML(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>颐鸿安 · 家中画面</title><style>body{font-family:system-ui;background:#f2f6f8;color:#183042;margin:24px auto;padding:20px;max-width:900px}button{padding:12px 20px;margin:6px 8px 6px 0;border:0;border-radius:12px;background:#087f8c;color:white;font-size:16px}button:disabled{opacity:.5}img{display:block;width:100%;max-width:800px;margin-top:20px;border-radius:16px;background:#dce5e9}pre{white-space:pre-wrap}</style>
<h1>颐鸿安 · 家中画面</h1><p>局域网实时画面 · 截图保存到当前手机或电脑</p>
<button id="start">开始观看</button><button id="stop">停止观看</button><button id="capture">保存当前画面</button><p id="message">正在连接…</p><img id="photo" crossorigin="anonymous" alt="实时画面"><pre id="status"></pre>
<script>
const msg=document.querySelector('#message'),img=document.querySelector('#photo'),start=document.querySelector('#start'),save=document.querySelector('#capture');
let active=false,sequence=0,retry=0;
async function connect(){const id=++sequence;clearTimeout(retry);active=true;start.disabled=true;msg.textContent='正在连接实时画面…';try{const r=await fetch('/stream-ticket',{cache:'no-store'});if(!r.ok)throw Error('连接失败：'+r.status);const s=await r.json();if(id!==sequence)return;img.src=location.protocol+'//'+location.hostname+':81/stream?ticket='+encodeURIComponent(s.ticket)+'&t='+Date.now();msg.textContent='实时观看中；可点击保存当前画面'}catch(e){if(id!==sequence)return;active=false;start.disabled=false;msg.textContent=e.message}}
function stop(){active=false;++sequence;clearTimeout(retry);img.removeAttribute('src');start.disabled=false;msg.textContent='已停止观看'}
start.onclick=connect;document.querySelector('#stop').onclick=stop;
img.onerror=()=>{if(active){msg.textContent='画面中断，3 秒后重试…';retry=setTimeout(connect,3000)}};
save.onclick=()=>{if(!active||!img.naturalWidth){msg.textContent='请先等待实时画面出现';return}save.disabled=true;try{const c=document.createElement('canvas');c.width=img.naturalWidth;c.height=img.naturalHeight;c.getContext('2d').drawImage(img,0,0);c.toBlob(b=>{save.disabled=false;if(!b){msg.textContent='截图失败，请重试';return}const u=URL.createObjectURL(b),a=document.createElement('a');a.href=u;a.download='居家监控-'+new Date().toISOString().replace(/[:.]/g,'-')+'.jpg';a.click();setTimeout(()=>URL.revokeObjectURL(u),30000);msg.textContent='已发起截图下载，请查看浏览器下载或保存提示'},'image/jpeg',.92)}catch(e){save.disabled=false;msg.textContent='截图失败：'+e.message}};
fetch('/status',{cache:'no-store'}).then(r=>r.json()).then(s=>{document.querySelector('#status').textContent='摄像头：'+(s.camera_ready?'就绪':'初始化失败')+' · 地址：'+s.ip;if(s.camera_ready)connect();else msg.textContent='摄像头未就绪'}).catch(e=>msg.textContent='无法读取摄像头状态');
window.addEventListener('pagehide',stop);
</script></html>)HTML");
  });
  server.on("/stream-ticket", HTTP_GET, []() {
    if (!authorized()) return;
    server.sendHeader("Cache-Control", "no-store");
    if (!streamAvailable || !cameraReady) {
      server.send(503, "application/json", "{\"error\":\"stream_unavailable\"}");
      return;
    }
    server.send(200, "application/json", "{\"ticket\":\"" + streamTicket + "\"}");
  });
  server.on("/capture", HTTP_GET, snapshot);
  server.on("/status", HTTP_GET, statusPage);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  streamServer.on("/stream", HTTP_GET, liveStream);
  streamServer.begin();
  streamAvailable = xTaskCreate(streamTask, "camera-stream", 8192, nullptr, 1, nullptr) == pdPASS;
  if (!streamAvailable) Serial.println("[camera] stream task allocation failed");
}

void loop() {
  static bool wasConnected = false;
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.printf("[wifi] connected; open http://%s/\n", WiFi.localIP().toString().c_str());
  }
  if (!connected && millis() - reconnectAt >= 15000) {
    reconnectAt = millis();
    Serial.println("[wifi] disconnected; retrying configured hotspot");
    WiFi.reconnect();
  }
  wasConnected = connected;
  server.handleClient();
  delay(2);
}
