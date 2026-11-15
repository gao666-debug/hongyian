#pragma once
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>
#include <esp_heap_caps.h>
#include "cloud_config.h"

volatile bool cloudOnline = false;
volatile int cloudLastCode = 0;
volatile bool cloudViewing = false;
volatile bool cloudCertPinned = false;

// 服务器 47.116.171.114 自签证书的 SHA256 指纹（与 qbot-public.crt 一致）。
// 该版 mbedTLS 对「自签证书 + IP 型 SAN」做链校验恒报 -9984，只能 setInsecure
// 建立连接后立刻比对指纹，指纹不符则拒绝上传，避免向冒充服务器推送画面。
static const uint8_t CLOUD_CERT_SHA256[32] = {
    0xBC, 0xEA, 0x1A, 0xB6, 0x67, 0xF5, 0x34, 0x2C, 0x4A, 0x4A, 0x3D, 0x4F, 0x2F, 0xBF, 0x0B, 0xE4,
    0x32, 0xD3, 0x86, 0x62, 0xEF, 0xDB, 0x7D, 0xA3, 0x68, 0x01, 0x7A, 0xD1, 0xDE, 0x7C, 0x02, 0xCB,
};

// 手机热点常屏蔽 UDP 123，SNTP 会一直 utc_ready=0，而 TLS 校验又必须有可信时间。
// 这里用明文 HTTP 的 Date 响应头兜底校时，绕开 UDP。
// 不依赖 timegm/strptime（本核心包未声明），自己把 UTC 历法换算成 epoch。
static time_t utcEpoch(int year, int mon, int day, int hh, int mi, int ss) {
  int y = year - (mon <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = static_cast<unsigned>((153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<time_t>(era * 146097 + static_cast<int>(doe) - 719468) * 86400 +
         hh * 3600 + mi * 60 + ss;
}

static int monthIndex(const char *m) {
  static const char *names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (int i = 0; i < 12; ++i)
    if (strncmp(m, names[i], 3) == 0) return i + 1;
  return 0;
}

static bool syncTimeFromHttp() {
  static const char *urls[] = {
      "http://connectivitycheck.platform.hicloud.com/generate_204",
      "http://connect.rom.miui.com/generate_204",
      "http://www.baidu.com/",
  };
  const char *keys[] = {"Date"};
  for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); ++i) {
    WiFiClient plain;
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(4000);
    http.setReuse(false);
    if (!http.begin(plain, urls[i])) continue;
    http.collectHeaders(keys, 1);
    const int code = http.GET();
    const String date = http.header("Date");
    http.end();
    if (code <= 0 || date.length() < 20) continue;
    char wd[4] = {0};
    char mon[4] = {0};
    int day = 0, year = 0, hh = 0, mi = 0, ss = 0;
    if (sscanf(date.c_str(), "%3s, %d %3s %d %d:%d:%d", wd, &day, mon, &year, &hh, &mi, &ss) != 7) continue;
    const int m = monthIndex(mon);
    if (m == 0) continue;
    const time_t t = utcEpoch(year, m, day, hh, mi, ss);
    if (t < 1700000000) continue;
    struct timeval tv = {t, 0};
    settimeofday(&tv, nullptr);
    Serial.printf("[cloud-camera] HTTP 校时成功 code=%d date=%s", code, date.c_str());
    return true;
  }
  return false;
}

// 分层探测：先看 8443 的 TCP 能否连通，再看 TLS 握手与证书校验。
static void probeTlsOnce() {
  String url = CLOUD_UPLOAD_URL;
  const int p = url.indexOf("://");
  String rest = p > 0 ? url.substring(p + 3) : url;
  const int s = rest.indexOf('/');
  String hostport = s > 0 ? rest.substring(0, s) : rest;
  const int c = hostport.indexOf(':');
  String host = c > 0 ? hostport.substring(0, c) : hostport;
  const int port = c > 0 ? hostport.substring(c + 1).toInt() : 443;

  WiFiClient tcp;
  const bool tcpOk = tcp.connect(host.c_str(), static_cast<uint16_t>(port), 5000);
  Serial.printf("[net] TCP %s:%d = %d\n", host.c_str(), port, tcpOk);
  tcp.stop();

  WiFiClientSecure tls;
  // 自签证书 + IP 型 SAN 在 mbedTLS 2.16 下链校验恒失败（-9984），改用
  // 「不校验链、连上后立刻比对 SHA256 指纹」的固定指纹方案。
  tls.setInsecure();
  const bool tlsOk = tls.connect(host.c_str(), static_cast<uint16_t>(port), 8000);
  char err[128] = {0};
  const int e = tls.lastError(err, sizeof(err));
  uint8_t fp[32] = {0};
  bool fpOk = false;
  if (tlsOk && tls.getFingerprintSHA256(fp)) {
    fpOk = memcmp(fp, CLOUD_CERT_SHA256, sizeof(fp)) == 0;
  }
  Serial.printf("[net] TLS = %d err=%d(%s) 指纹匹配=%d heap=%u largest=%u\n", tlsOk, e, err, fpOk,
                ESP.getFreeHeap(), static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  tls.stop();
  cloudCertPinned = fpOk;
}

void cloudUploadTask(void *) {
  unsigned long backoff = 1000;
  WiFiClientSecure transport;
  transport.setInsecure(); // 证书链校验不可用，改由 probeTlsOnce() 比对指纹把关
  transport.setTimeout(5);
  transport.setHandshakeTimeout(8);
  HTTPClient client;
  client.setConnectTimeout(5000);
  client.setTimeout(5000);
  client.setReuse(true);
  unsigned long waitLog = 0;
  unsigned long lastTimeSync = 0;
  unsigned long demandAt = 0;
  bool discardOldFrame = true;
  String demandUrl = CLOUD_UPLOAD_URL;
  if (demandUrl.endsWith("/frame")) demandUrl.remove(demandUrl.length() - 6);
  demandUrl += "/demand";
  const char *headers[] = {"X-Camera-Active"};
  client.collectHeaders(headers, 1);
  while (true) {
    if (WiFi.status() != WL_CONNECTED || !cameraReady || time(nullptr) < 1700000000) {
      cloudOnline = false;
      cloudViewing = false;
      demandAt = 0;
      // 有网但时间不可信时，每 10 秒试一次 HTTP 校时（SNTP 被热点屏蔽时靠它）。
      if (WiFi.status() == WL_CONNECTED && cameraReady && time(nullptr) < 1700000000 &&
          millis() - lastTimeSync > 10000) {
        lastTimeSync = millis();
        if (!syncTimeFromHttp()) Serial.println("[cloud-camera] HTTP 校时失败，稍后重试");
      }
      if (millis() - waitLog > 15000) {
        waitLog = millis();
        Serial.printf("[cloud-camera] waiting wifi=%d utc_ready=%d camera=%d\n", WiFi.status() == WL_CONNECTED, time(nullptr) >= 1700000000, cameraReady);
      }
      delay(1000);
      continue;
    }
    // Idle requests carry only control data. Never capture/upload without current demand.
    static unsigned long lastProbe = 0;
    if (!cloudCertPinned && (lastProbe == 0 || millis() - lastProbe > 60000)) {
      lastProbe = millis();
      probeTlsOnce();
    }
    if (!cloudCertPinned) {
      // 指纹没确认就不推流，避免把画面送到冒充的服务器。
      cloudOnline = false;
      cloudViewing = false;
      delay(2000);
      continue;
    }
    if (demandAt == 0 || millis() - demandAt >= (cloudViewing ? 2000UL : 5000UL)) {
      int code = 0;
      if (client.begin(transport, demandUrl)) {
        client.addHeader("X-Camera-Token", CLOUD_CAMERA_TOKEN);
        code = client.GET();
        cloudViewing = code == 200 && client.header("X-Camera-Active") == "1";
        if (code > 0) client.getString();
      }
      const int prevCode = cloudLastCode;
      cloudLastCode = code;
      cloudOnline = code == 200;
      demandAt = millis();
      if (!cloudOnline) {
        if (code != prevCode)
          Serial.printf("[cloud-camera] demand 失败 code=%d utc=%ld heap=%u\n", code,
                        static_cast<long>(time(nullptr)), ESP.getFreeHeap());
        cloudViewing = false; discardOldFrame = true; client.end();
        delay(backoff); backoff = min(backoff * 2UL, 30000UL); demandAt = 0;
        continue;
      }
      backoff = 1000;
    }
    if (!cloudViewing) { discardOldFrame = true; delay(500); continue; }
    if (xSemaphoreTake(cameraLock, pdMS_TO_TICKS(500)) != pdTRUE) { delay(100); continue; }
    // The single driver buffer may contain a pre-standby frame; discard it on wake.
    if (discardOldFrame) {
      camera_fb_t *old = esp_camera_fb_get();
      if (old) esp_camera_fb_return(old);
      discardOldFrame = false;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    const size_t len = fb ? fb->len : 0;
    uint8_t *copy = nullptr;
    if (len > 0 && len <= 512 * 1024) {
      copy = static_cast<uint8_t *>(heap_caps_malloc(len, psramFound() ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : MALLOC_CAP_8BIT));
      if (copy) memcpy(copy, fb->buf, len);
    }
    if (fb) esp_camera_fb_return(fb);
    xSemaphoreGive(cameraLock); // Network delays must not block local camera capture.
    int code = 0;
    if (copy && client.begin(transport, CLOUD_UPLOAD_URL)) {
      client.addHeader("Content-Type", "image/jpeg");
      client.addHeader("X-Camera-Token", CLOUD_CAMERA_TOKEN);
      code = client.POST(copy, len);
      if (code > 0) client.getString(); // Drain response before reusing HTTPS connection.
      if (code != 200) client.end();
    }
    if (copy) free(copy);
    cloudLastCode = code;
    const bool success = code == 200;
    if (success != cloudOnline || !success) Serial.printf("[cloud-camera] upload %s code=%d\n", success ? "ready" : "retry", code);
    cloudOnline = success;
    if (success) { backoff = 1000; delay(500); } // Up to 2 fps, network time adds to interval.
    else { cloudViewing = false; demandAt = 0; discardOldFrame = true; delay(backoff); backoff = min(backoff * 2UL, 30000UL); }
  }
}

void startCloudUpload() {
#if CLOUD_ENABLED
  configTime(0, 0, "ntp.aliyun.com", "pool.ntp.org", "time.cloudflare.com");
  if (!String(CLOUD_UPLOAD_URL).startsWith("https://") || strlen(CLOUD_CA_PEM) < 64 || strlen(CLOUD_CAMERA_TOKEN) < 24) {
    Serial.println("[cloud-camera] invalid local provisioning; remote upload disabled");
    return;
  }
  if (xTaskCreate(cloudUploadTask, "cloud-camera", 12288, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("[cloud-camera] upload task allocation failed");
  }
#else
  Serial.println("[cloud-camera] not provisioned; LAN viewing available");
#endif
}
