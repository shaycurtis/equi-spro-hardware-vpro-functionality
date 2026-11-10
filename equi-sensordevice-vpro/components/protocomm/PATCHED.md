# protocomm: patched copy of ESP-IDF's component

This folder is ESP-IDF **v6.1** `components/protocomm` (commit `fff9895c82d744c7237be8847347bdd1b07c6643`,
copied from `C:\esp\v6.1\esp-idf\components\protocomm`), unchanged except for
`src/transports/protocomm_nimble.c`. Because a project component with the same name as an IDF
component wins, the build uses this copy instead of IDF's (check: `build/project_description.json`,
`build_component_info.protocomm.dir`).

## Why

`protocomm_nimble.c` advertises with NimBLE's legacy API (`ble_gap_adv_set_fields`,
`ble_gap_adv_rsp_set_fields`, `ble_gap_adv_start`, `ble_gap_adv_stop`). With
`CONFIG_BT_NIMBLE_EXT_ADV=y` NimBLE compiles those out and they return `BLE_HS_ENOTSUP`
(`bt/host/nimble/nimble/nimble/host/src/ble_gap.c`), so BLE provisioning
(`network_provisioning` BLE scheme) starts but never advertises. This firmware needs extended
advertising for its two telemetry advertising sets, so it can't turn it off.

## What changed (only when `MYNEWT_VAL(BLE_EXT_ADV)`; the legacy code is kept in `#else`)

1. `simple_ble_advertise()`: the same advertising data and scan response are sent through
   extended-advertising instance 0 with legacy PDUs (connectable, scannable, interval 0x100, 1M PHY,
   same own address type): `ble_gap_ext_adv_configure`, `ble_hs_adv_set_fields_mbuf`,
   `ble_gap_ext_adv_set_data`, `ble_gap_ext_adv_rsp_set_data`, `ble_gap_ext_adv_start(0, 0, 0)`.
   An active instance is stopped first, so re-advertising after a disconnect works.
2. `simple_ble_gap_event()`, `BLE_GAP_EVENT_ADV_COMPLETE`: an extended advertising set also
   completes (reason 0) when a central connects through it, which the legacy API never reports.
   Advertising is then not restarted, so the device stops advertising while connected, as with
   the legacy API (`BLE_GAP_EVENT_DISCONNECT` restarts it, unchanged).
3. `protocomm_ble_stop()`: `ble_gap_ext_adv_stop(0)` instead of `ble_gap_adv_stop()`.

Nothing else (GATT, security, NimBLE init) is touched.

## When upgrading ESP-IDF

Re-copy `components/protocomm` from the new IDF and re-apply the diff below, or delete this folder
if the new IDF's protocomm already supports extended advertising. Upstream issue: drafted, to be
filed on espressif/esp-idf (not yet filed).

## Diff against IDF v6.1

```diff
--- a/components/protocomm/src/transports/protocomm_nimble.c
+++ b/components/protocomm/src/transports/protocomm_nimble.c
@@ -211,6 +211,71 @@
     adv_data.num_uuids128 = 1;
     adv_data.uuids128_is_complete = 1;
 
+#if MYNEWT_VAL(BLE_EXT_ADV)
+    /* PATCH (see components/protocomm/PATCHED.md): with CONFIG_BT_NIMBLE_EXT_ADV=y the legacy
+     * ble_gap_adv_* calls return BLE_HS_ENOTSUP. Advertise the same data through extended
+     * advertising instance 0 with legacy PDUs (connectable, scannable), so phones see the same
+     * advert and scan response as with the legacy API. */
+    struct ble_gap_ext_adv_params ext_params = {
+        .connectable = 1,
+        .scannable = 1,
+        .legacy_pdu = 1,
+        .itvl_min = 0x100,
+        .itvl_max = 0x100,
+        .own_addr_type = own_addr_type,
+        .primary_phy = BLE_HCI_LE_PHY_1M,
+        .secondary_phy = BLE_HCI_LE_PHY_1M,
+        .tx_power = 127,
+    };
+    struct os_mbuf *adv_m, *rsp_m;
+
+    if (ble_gap_ext_adv_active(0)) {
+        ble_gap_ext_adv_stop(0);
+    }
+    rc = ble_gap_ext_adv_configure(0, &ext_params, NULL, simple_ble_gap_event, NULL);
+    if (rc != 0) {
+        if (rc == BLE_HS_EDISABLED) {
+            ESP_LOGD(TAG, "BLE Host is disabled !!");
+        } else {
+            ESP_LOGE(TAG, "Error configuring advertisement; rc = %d", rc);
+        }
+        return;
+    }
+
+    adv_m = os_msys_get_pkthdr(BLE_HS_ADV_MAX_SZ, 0);
+    if (adv_m == NULL) {
+        ESP_LOGE(TAG, "Error allocating advertisement data");
+        return;
+    }
+    rc = ble_hs_adv_set_fields_mbuf(&adv_data, adv_m);
+    if (rc == 0) {
+        rc = ble_gap_ext_adv_set_data(0, adv_m);   /* takes ownership of adv_m */
+    } else {
+        os_mbuf_free_chain(adv_m);
+    }
+    if (rc != 0) {
+        ESP_LOGE(TAG, "Error setting advertisement data; rc = %d", rc);
+        return;
+    }
+
+    rsp_m = os_msys_get_pkthdr(BLE_HS_ADV_MAX_SZ, 0);
+    if (rsp_m == NULL) {
+        ESP_LOGE(TAG, "Error allocating scan response");
+        return;
+    }
+    rc = ble_hs_adv_set_fields_mbuf(&resp_data, rsp_m);
+    if (rc == 0) {
+        rc = ble_gap_ext_adv_rsp_set_data(0, rsp_m);   /* takes ownership of rsp_m */
+    } else {
+        os_mbuf_free_chain(rsp_m);
+    }
+    if (rc != 0) {
+        ESP_LOGE(TAG, "Error in setting scan response; rc = %d", rc);
+        return;
+    }
+
+    rc = ble_gap_ext_adv_start(0, 0, 0);   /* forever, as BLE_HS_FOREVER */
+#else
     rc = ble_gap_adv_set_fields(&adv_data);
     if (rc != 0) {
         ESP_LOGE(TAG, "Error setting advertisement data; rc = %d", rc);
@@ -230,6 +295,7 @@
 
     rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                            &adv_params, simple_ble_gap_event, NULL);
+#endif /* MYNEWT_VAL(BLE_EXT_ADV) */
     if (rc != 0) {
         /* If BLE Host is disabled, it probably means device is already
          * provisioned in previous session. Avoid error prints for this case.*/
@@ -278,6 +344,14 @@
         return 0;
 
     case BLE_GAP_EVENT_ADV_COMPLETE:
+#if MYNEWT_VAL(BLE_EXT_ADV)
+        /* PATCH: an extended advertising set also completes (reason 0) when a central connects
+         * through it, which the legacy API does not report. Don't re-advertise then: as with
+         * legacy advertising, BLE_GAP_EVENT_DISCONNECT resumes it. */
+        if (event->adv_complete.reason == 0) {
+            return 0;
+        }
+#endif
         simple_ble_advertise();
         return 0;
 
@@ -1240,7 +1314,11 @@
 
     esp_err_t ret = ESP_OK;
     bool ble_callbacks_active = true;
+#if MYNEWT_VAL(BLE_EXT_ADV)
+    esp_err_t rc = ble_gap_ext_adv_stop(0);   /* PATCH: see simple_ble_advertise() */
+#else
     esp_err_t rc = ble_gap_adv_stop();
+#endif
     if (rc) {
         ESP_LOGD(TAG, "Error in stopping advertisement with err code = %d",
                  rc);
```
