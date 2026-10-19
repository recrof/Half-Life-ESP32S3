idf_component_register(
    SRCS
        esp_main.c
        esp_video.c
        esp_display.c
        esp_audio.c
        esp_input.c
        esp_touch.c
        esp_hid_parser.c
        esp_ble_hid.c
        esp_board_l2.c
        esp_board_tdeck.c
        esp_gt911.c
        esp_console.c
        font8x8.c
    INCLUDE_DIRS
        .
    REQUIRES
        xash
        esp_driver_spi
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_usb_serial_jtag
        esp_driver_i2s
        esp_driver_sdmmc
        esp_driver_sdspi
        sdmmc
        fatfs
        esp_psram
        esp_partition
        esp_timer
        nvs_flash
        bt
        esp_hid
        esp_netif
)
# Board selection from the command line: -DBOARD=BOARD_LILYGO_TDECK
if(DEFINED BOARD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD=${BOARD})
endif()
# Xash key codes (keydefs.h) for the input devices
idf_build_get_property(project_dir PROJECT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${project_dir}/xash3d-fwgs/engine")
# Diagnostic build: the callers of the software floating point functions ("dbl" console command)
option(HL_DBLPROF "count the callers of the soft-float functions" OFF)
if(HL_DBLPROF)
    target_sources(${COMPONENT_LIB} PRIVATE esp_dblprof.c)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HL_DBLPROF=1)
    foreach(f __muldf3 __adddf3 __subdf3 __divdf3 __divsf3 __ltdf2 __gtdf2 __ledf2 __gedf2 __extendsfdf2 __truncdfsf2 __floatsidf __fixdfsi sin cos)
        idf_build_set_property(LINK_OPTIONS "-Wl,--wrap=${f}" APPEND)
    endforeach()
endif()
