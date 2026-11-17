idf_component_register(SRCS "iot.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash mbedtls esp_event esp_timer app_update esp_https_ota esp_http_client
                                     esp_app_format)

# ---- Secrets from the git-ignored <repo>/private/ (never committed) ---------------------------
set(REPO_DIR ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(CLAIM_DIR ${REPO_DIR}/private/sensor-claim)

# AWS IoT fleet provisioning claim certificate for sensors (shared by all sensors, only allowed to
# ask for a certificate of its own). See cloud/README.md, "Sensor identity". The app image is
# flash-encrypted on the SPRO boards, which covers the claim key.
if(EXISTS ${CLAIM_DIR}/claim.pem.crt AND EXISTS ${CLAIM_DIR}/claim.private.pem.key)
    target_add_binary_data(${COMPONENT_LIB} ${CLAIM_DIR}/claim.pem.crt TEXT)
    target_add_binary_data(${COMPONENT_LIB} ${CLAIM_DIR}/claim.private.pem.key TEXT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE IOT_HAVE_CLAIM=1)
else()
    message(WARNING "No sensor claim certificate in ${CLAIM_DIR} (claim.pem.crt + claim.private.pem.key): "
                    "the firmware builds but a device cannot get its AWS identity. "
                    "Run 'idf.py reconfigure' after adding it.")
endif()

# IoT Core endpoint: private/sensor-claim/endpoint.txt, else the PC gateway's private/iot/endpoint.txt
# (same account).
foreach(f ${CLAIM_DIR}/endpoint.txt ${REPO_DIR}/private/iot/endpoint.txt)
    if(EXISTS ${f})
        file(READ ${f} IOT_ENDPOINT)
        string(STRIP "${IOT_ENDPOINT}" IOT_ENDPOINT)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE IOT_ENDPOINT="${IOT_ENDPOINT}")
        break()
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CLAIM_DIR})
