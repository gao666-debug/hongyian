# Push Kit 后续配置（暂未接通）

1. 在华为开发者平台创建/选择 HarmonyOS 应用，包名与本项目 com.zhian.eldercare 一致，完成平台要求的应用与签名配置。
2. 按应用当前权益开通推送服务，确定安全报警通知使用的消息场景；不要套用营销通知类别。
3. 客户端接入 Push Kit，获取 Push Token；登录后绑定到当前家庭账号，Token更新后重新上传，退出或切换账号解除旧绑定。
4. 后端安全配置服务账号凭据，保存用户-终端Token绑定，按家庭成员权限发送告警通知；增加发送失败重试、去重、无效Token清理。凭据不能进入App或GitHub。
5. 配置通知点击路由至对应家庭设备、报警详情；显示数据前重新校验登录与家庭权限。
6. 用签名真机分别验证前台、退后台、锁屏、进程结束、退出账号、账号切换、断网恢复。接收能力以实际平台权限与系统状态为准。

官方开通指南：https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/push-config-setting
官方接入流程：https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/push-gettingstart

未完成上述接入与真机验证前，不宣称“App关闭也能推送”。
