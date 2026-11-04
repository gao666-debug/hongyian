# 颐鸿安 ESP32-CAM v0.3

支持 OV3660，保留局域网 Basic 鉴权观看；可选 HTTPS 上传到家庭后端供异地 App 查看，无需家中电脑。

1. 保留本机 HomeSafetyCamera/secrets.h 的 Wi-Fi 与本地账号，未配置时参考 secrets.example.h。
2. 服务器部署 backend 1.5.0 后，使用后端 tools_camera_provision.py 生成 cloud_private.h（仅本机保存）。
3. 使用原 ESP32 Arduino 2.0.17 兼容工具链编译：`arduino-cli compile --fqbn esp32:esp32:esp32cam HomeSafetyCamera`；本机已安装环境可用 `bash scripts/build.sh`。
4. 按原 ESP32-CAM 烧录座方式烧录。供电后自动连 Wi-Fi，NTP 校时后开始校验证书并上传。

缺少 cloud_private.h 时只启用局域网。默认不会自动获得服务器凭据。重新签发令牌后需重新配置、编译、烧录。

公网以最多约 2 fps 的 JPEG 查看为目标，实际帧率需实测。锁只保护采集与复制，不在网络请求期间占用。最大上传 512 KiB，失败退避 1~30 秒；TLS 仍需内存，实板稳定性尚待验证。

后端部署、权限和验收见仓库 backend/docs/三设备与远程摄像头_20261104.md。不要上传 secrets.h / cloud_private.h / 带私有配置的镜像。
