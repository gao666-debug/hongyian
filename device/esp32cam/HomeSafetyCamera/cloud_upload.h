#pragma once
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <esp_heap_caps.h>
#include "cloud_config.h"

volatile bool cloudOnline = false;
volatile int cloudLastCode = 0;
volatile bool cloudViewing = false;

void cloudUploadTask(void *) {
  unsigned long backoff = 1000;
  WiFiClientSecure transport;
  transport.setCACert(CLOUD_CA_PEM); // Never disable peer certificate verification.
  transport.setTimeout(5);
  transport.setHandshakeTimeout(8);
  HTTPClient client;
  client.setConnectTimeout(5000);
  client.setTimeout(5000);
  client.setReuse(true);
  unsigned long waitLog = 0;
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
      if (millis() - waitLog > 15000) {
        waitLog = millis();
        Serial.printf("[cloud-camera] waiting wifi=%d utc_ready=%d camera=%d\n", WiFi.status() == WL_CONNECTED, time(nullptr) >= 1700000000, cameraReady);
      }
      delay(1000);
      continue;
    }
    // Idle requests carry only control data. Never capture/upload without current demand.
    if (demandAt == 0 || millis() - demandAt >= (cloudViewing ? 2000UL : 5000UL)) {
      int code = 0;
      if (client.begin(transport, demandUrl)) {
        client.addHeader("X-Camera-Token", CLOUD_CAMERA_TOKEN);
        code = client.GET();
        cloudViewing = code == 200 && client.header("X-Camera-Active") == "1";
        if (code > 0) client.getString();
      }
      cloudLastCode = code;
      cloudOnline = code == 200;
      demandAt = millis();
      if (!cloudOnline) {
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
