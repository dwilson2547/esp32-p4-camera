# esp_cam_sensor_imx477 - Sony IMX477 (Raspberry Pi HQ Camera) for the ESP32-P4
# esp_cam_sensor framework. GPL-2.0: register tables ported from the Raspberry
# Pi Linux driver. Plugs into esp_cam_sensor's auto-detect array through the
# ".esp_cam_sensor_detect_fn" linker section.
#
# ISP tuning (cfg/imx477_default.json) must be registered by the APP, in its
# project CMakeLists between include(project.cmake) and project():
#   idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH <path> APPEND)
set(srcs)
if(CONFIG_CAMERA_IMX477)
    list(APPEND srcs "imx477.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_cam_sensor
    PRIV_REQUIRES esp_driver_gpio esp_sccb_intf esp_timer
)

# Keep the detect object: nothing references it directly.
if(CONFIG_CAMERA_IMX477)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u imx477_detect")
endif()
