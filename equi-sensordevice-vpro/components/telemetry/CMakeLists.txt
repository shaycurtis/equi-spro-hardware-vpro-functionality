set(FLEET_KEY "${CMAKE_CURRENT_LIST_DIR}/../../../private/fleet_key.bin")
if(NOT EXISTS "${FLEET_KEY}")
    message(FATAL_ERROR "Fleet key missing: ${FLEET_KEY} (16 random bytes, kept out of git; "
                        "the gateway reads the same file)")
endif()

idf_component_register(SRCS "telemetry.c" "ecg_service.c" "live_service.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bt ecg sensors nvs_flash esp_timer mbedtls
                       EMBED_FILES "${FLEET_KEY}")
