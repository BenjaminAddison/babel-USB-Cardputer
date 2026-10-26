FILE(GLOB_RECURSE app_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.c)
idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "." "screen"
    REQUIRES espressif-tinyusb-component esp_lcd driver esp_driver_ledc esp_driver_gpio esp_driver_spi
             esp_driver_i2c esp_adc esp_timer nvs_flash
)
