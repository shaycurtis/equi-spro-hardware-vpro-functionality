/*
 * The gateway's own AWS IoT identity (from fleet provisioning): thing name, certificate and private
 * key, kept in NVS namespace "gw_id". The key pair is made here (PSA crypto, P-256) and only its
 * CSR leaves the device (CreateCertificateFromCsr), so the private key never crosses the network.
 *
 * Phase 1 stores the key in plain NVS (dev boards have no flash encryption). For production, enable
 * NVS encryption (HMAC-based on the S3, or flash encryption) - see the README.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_csr.h"
#include "gw.h"

static const char *TAG = "identity";
#define NS "gw_id"

void gw_serial(char out[13])
{
    uint8_t m[6];
    esp_efuse_mac_get_default(m);
    snprintf(out, 13, "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static char *get_str(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(h, key, NULL, &len) != ESP_OK || len == 0) {
        return NULL;
    }
    char *s = malloc(len);
    if (s && nvs_get_str(h, key, s, &len) != ESP_OK) {
        free(s);
        s = NULL;
    }
    return s;
}

bool gw_identity_load(gw_identity_t *id)
{
    memset(id, 0, sizeof(*id));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t tl = sizeof(id->thing);
    bool ok = nvs_get_str(h, "thing", id->thing, &tl) == ESP_OK;
    id->cert_pem = get_str(h, "cert");
    id->key_pem = get_str(h, "key");
    nvs_close(h);
    if (!ok || !id->cert_pem || !id->key_pem) {
        gw_identity_free(id);
        return false;
    }
    return true;
}

esp_err_t gw_identity_save(const char *thing, const char *cert_pem, const char *key_pem)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "cert", cert_pem);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "key", key_pem);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "thing", thing);   /* written last: marks the identity complete */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "saved identity \"%s\": %s", thing, esp_err_to_name(err));
    return err;
}

void gw_identity_free(gw_identity_t *id)
{
    free(id->cert_pem);
    free(id->key_pem);
    id->cert_pem = id->key_pem = NULL;
}

esp_err_t gw_identity_make_csr(const char *cn, char **csr_pem, char **key_pem)
{
    *csr_pem = *key_pem = NULL;
    esp_err_t result = ESP_FAIL;
    psa_key_id_t kid = 0;
    mbedtls_pk_context pk;
    mbedtls_x509write_csr csr;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_csr_init(&csr);
    char *csr_buf = calloc(1, 1024), *key_buf = calloc(1, 512);
    char subject[160];
    int rc;

    if (!csr_buf || !key_buf || psa_crypto_init() != PSA_SUCCESS) {
        goto out;
    }
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&a, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    psa_status_t st = psa_generate_key(&a, &kid);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_key %d", (int)st);
        goto out;
    }
    if ((rc = mbedtls_pk_copy_from_psa(kid, &pk)) != 0) {
        ESP_LOGE(TAG, "pk_copy_from_psa -0x%04x", -rc);
        goto out;
    }
    snprintf(subject, sizeof(subject), "CN=%s,O=Equimetrics", cn);
    mbedtls_x509write_csr_set_key(&csr, &pk);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    if ((rc = mbedtls_x509write_csr_set_subject_name(&csr, subject)) != 0 ||
        (rc = mbedtls_x509write_csr_pem(&csr, (unsigned char *)csr_buf, 1024)) != 0 ||
        (rc = mbedtls_pk_write_key_pem(&pk, (unsigned char *)key_buf, 512)) != 0) {
        ESP_LOGE(TAG, "CSR / key PEM -0x%04x", -rc);
        goto out;
    }
    *csr_pem = csr_buf;
    *key_pem = key_buf;
    csr_buf = key_buf = NULL;
    result = ESP_OK;
out:
    if (kid) {
        psa_destroy_key(kid);
    }
    mbedtls_x509write_csr_free(&csr);
    mbedtls_pk_free(&pk);
    free(csr_buf);
    free(key_buf);
    return result;
}
