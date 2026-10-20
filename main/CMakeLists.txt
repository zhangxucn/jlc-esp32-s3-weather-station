idf_component_register(SRCS "main.c" "ui_display.c" "weather_ui.c" "font_data.c" "region_data.c" "touch.c" "settings.c"
                       INCLUDE_DIRS "."
                       REQUIRES weather esp_wifi esp_netif esp_event nvs_flash
                                 esp_lcd esp_driver_i2c esp_driver_ledc esp_driver_spi esp_driver_gpio)
