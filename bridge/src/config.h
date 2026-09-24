#pragma once

// Defaults for the private Wi-Fi network the bridge creates. WLED joins this network
// like any home Wi-Fi. These can also be changed later without reflashing:
//   python pc\wledlink.py bridge-config --ssid ... --password ...
#define WL_DEFAULT_SSID          "WLEDLink"
#define WL_DEFAULT_PASS          "quartz-basil-1769"
#define WL_DEFAULT_CHANNEL       0     // 0 = pick the least crowded of 1/6/11 at every boot
#define WL_DEFAULT_HIDDEN        1     // don't show up in phones' Wi-Fi lists; WLED must be given the name directly
#define WL_DEFAULT_TXPOWER_QDBM  34    // 8.5 dBm (units of 0.25 dBm): enough for one room, little spill-over
#define WL_BEACON_INTERVAL_TU    300   // ~0.3 s between beacons instead of the usual 0.1 s: less airtime used
#define WL_DEFAULT_WIFI_WITH_PC  0     // 0 = Wi-Fi on whenever the bridge has power (phone control with the PC off)
                                       // 1 = only while the PC program runs (off a minute after it stops)

#define WL_COUNTRY               "US"  // limits channels to 1-11
#define WL_AP_IP                 192, 168, 77, 1
#define WL_MAX_STATIONS          4

#define WL_BLE_NAME              "Lamp"  // Bluetooth name the phone sees; kept plain on purpose

#define WL_SERIAL_BAUD           921600
#define WL_STATUS_LED_PIN        2     // blue LED on most ESP32 dev boards, -1 to disable
