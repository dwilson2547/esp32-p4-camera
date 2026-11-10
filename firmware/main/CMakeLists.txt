idf_component_register(SRCS "imx477_preview.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_video esp_cam_sensor_imx477 esp_lcd_rpi_dsi esp_driver_ppa esp_driver_i2c esp_mm esp_timer)
