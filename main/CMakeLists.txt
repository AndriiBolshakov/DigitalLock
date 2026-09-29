idf_component_register(
    SRCS "DigitalLock.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio
    REQUIRES esp_timer
    REQUIRES esp_adc
    REQUIRES esp_driver_ledc
)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error" APPEND)