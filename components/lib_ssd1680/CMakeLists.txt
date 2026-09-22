idf_component_register(SRCS "lib_ssd1680.c"
                    INCLUDE_DIRS "include" "include/fonts" "include/bitmaps" "."
                    REQUIRES "driver"
                    REQUIRES esp_driver_spi  esp_driver_gpio)