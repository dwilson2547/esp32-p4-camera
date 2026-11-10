idf_component_register(SRCS "imx477_preview.c" "sd_card.c" "capture.c" "ui.c" "fullres.c" "isp_wrap.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_video esp_cam_sensor_imx477 esp_lcd_rpi_dsi esp_driver_ppa esp_driver_i2c esp_mm esp_timer fatfs sdmmc esp_driver_sdmmc esp_driver_jpeg esp_driver_isp)

# isp_wrap.c: let a bypassed ISP take frames wider than 1920 px (full-res raw).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_isp_new_processor" "-u __wrap_esp_isp_new_processor")
