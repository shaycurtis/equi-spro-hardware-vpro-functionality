# Backend URL + key live in <repo>/private/api_env.h (git-ignored), like VPro's env.xh.
set(API_ENV_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../private")
if(NOT EXISTS "${API_ENV_DIR}/api_env.h")
    message(FATAL_ERROR "Backend settings missing: ${API_ENV_DIR}/api_env.h "
                        "(defines API_DATA_URL and API_DATA_KEY; kept out of git)")
endif()

idf_component_register(SRCS "cloud.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "${API_ENV_DIR}"
                       PRIV_REQUIRES provisioning esp_wifi esp_netif esp_event esp_http_client esp-tls
                                     mbedtls esp_timer storage iot)
