cmake_minimum_required(VERSION 3.16)
# IMX477 ISP tuning (black level, AWB, CCM and AE limits from the Raspberry Pi
# libcamera IMX477 tuning; see the comments inside the JSON).
# Must be set between project.cmake and project(): esp_ipa reads it while configuring.
set(imx477_ipa_json "${CMAKE_CURRENT_LIST_DIR}/../components/esp_cam_sensor_imx477/cfg/imx477_default.json")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${imx477_ipa_json}" APPEND)
set(COMPONENTS main)
project(esp32_p4_camera)
