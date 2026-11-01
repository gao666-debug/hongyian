# 手表应用：传感器 + 摔倒检测 + 上云（默认），或单独的圆屏点亮测试。
#
# HONGYIAN_LCD_ONLY=ON  -> 只初始化屏、不碰 I2C，用于排查面板/引脚
# HONGYIAN_LCD_ONLY=OFF -> hy_sensor_main.c 作为唯一 app 入口
if(NOT DEFINED HONGYIAN_LCD_ONLY)
    set(HONGYIAN_LCD_ONLY OFF)
endif()

if(HONGYIAN_LCD_ONLY)
    message(STATUS "Hongyian: HY-LCD-ONLY (no I2C, no GPIO scan)")
    set(SOURCES "${SOURCES}"
        "${CMAKE_CURRENT_SOURCE_DIR}/lcd_only_main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/lcd_bringup.c"
        PARENT_SCOPE)
else()
    # 注意：hy_cloud_cfg.h 不在仓库里，需从 hy_cloud_cfg.example.h 复制一份。
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hy_cloud_cfg.h")
        message(FATAL_ERROR "缺少 src/hy_cloud_cfg.h：请先复制 src/hy_cloud_cfg.example.h 并填写 WiFi / 设备三元组")
    endif()
    message(STATUS "Hongyian: HY-SENSOR (MPU6050 + MAX3010x + fall detect + IoTDA)")
    set(SOURCES "${SOURCES}"
        "${CMAKE_CURRENT_SOURCE_DIR}/hy_sensor_main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/hy_sensor.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/hy_net.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/hy_mqtt.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/hy_sha256.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/hy_cloud.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/lcd_bringup.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/bsp_iic.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/algorithm.c"
        PARENT_SCOPE)
endif()
