# Default to an isolated panel test until the screen has been verified on hardware.
if(NOT DEFINED HONGYIAN_LCD_ONLY)
    set(HONGYIAN_LCD_ONLY ON)
endif()

if(HONGYIAN_LCD_ONLY)
    message(STATUS "Hongyian: HY-LCD-ONLY-1 (no I2C, no GPIO scan)")
    set(SOURCES "${SOURCES}"
        "${CMAKE_CURRENT_SOURCE_DIR}/lcd_only_main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/lcd_bringup.c"
        PARENT_SCOPE)
else()
    message(STATUS "Hongyian: watch application (board_check disabled)")
    set(SOURCES "${SOURCES}"
        "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/watch_ui.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/watch_sensor.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/lcd.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/lcd_init.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/bsp_iic.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/algorithm.c"
        PARENT_SCOPE)
endif()
