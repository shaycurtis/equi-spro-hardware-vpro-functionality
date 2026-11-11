idf_component_register(SRCS "gateway_main.c" "gw_wifi.c" "gw_ble.c" "gw_queue.c" "gw_cloud.c"
                            "gw_identity.c" "gw_led.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt esp_wifi esp_netif esp_event nvs_flash esp_timer mbedtls
                                     esp_driver_gpio esp_app_format)

# ---- Secrets from the git-ignored <repo>/private/ (never committed) ---------------------------
set(REPO_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
set(CLAIM_DIR ${REPO_DIR}/private/gateway-claim)

# AWS IoT fleet provisioning claim certificate (shared by all gateways, only allowed to ask for
# its own certificate). See the README, "AWS set-up".
if(EXISTS ${CLAIM_DIR}/claim.pem.crt AND EXISTS ${CLAIM_DIR}/claim.private.pem.key)
    target_add_binary_data(${COMPONENT_LIB} ${CLAIM_DIR}/claim.pem.crt TEXT)
    target_add_binary_data(${COMPONENT_LIB} ${CLAIM_DIR}/claim.private.pem.key TEXT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GW_HAVE_CLAIM=1)
else()
    message(WARNING "No claim certificate in ${CLAIM_DIR} (claim.pem.crt + claim.private.pem.key): "
                    "the gateway builds but cannot get its own AWS identity. Run 'idf.py reconfigure' after adding it.")
endif()

# IoT Core endpoint: CONFIG_GW_IOT_ENDPOINT, else private/gateway-claim/endpoint.txt, else the PC
# gateway's private/iot/endpoint.txt (same account).
foreach(f ${CLAIM_DIR}/endpoint.txt ${REPO_DIR}/private/iot/endpoint.txt)
    if(EXISTS ${f})
        file(READ ${f} GW_ENDPOINT)
        string(STRIP "${GW_ENDPOINT}" GW_ENDPOINT)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE GW_ENDPOINT_FILE="${GW_ENDPOINT}")
        break()
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CLAIM_DIR})
