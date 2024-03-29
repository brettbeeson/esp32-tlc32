#include "BBEsp32Lib.h"
#include "TimeLapseCamera.h"
#include "TimeLapseWebServer.h"
#include "TimeLapseWebSocket.h"
#include "WiFiAngel.h"
// Arduino
#include "Arduino.h"
#include "FS.h"
#include "SD_MMC.h"
// Time
#include "apps/sntp/sntp.h"
#include "lwip/apps/sntp.h"
// ESP
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "esp_camera.h"
#include "esp_event.h" // default event loop
#include "esp_pm.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "rom/rtc.h"
#include "sdmmc_cmd.h"

// System
#include <sys/time.h>
#include <time.h>
#include <vector>

#include "camera.h" // testing only

//#ifndef FATFS_LONG_FILENAMES
//#error Enable FATFS_LONG_FILENAMES in menuconfig
//#endif

using namespace std;
using namespace bbesp32lib;

TimeLapseCamera cam(SD_MMC);
//TimeLapseWebServer webServer(80, cam);
//TimeLapseWebSocket socketServer("/ws", cam);

RemoteDebug Debug;

const char *NTP_SERVER = "pool.ntp.org";

const char *TAG = "tlc32";
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE
#include "esp_log.h"

#define BASIC_CAM_ONLY 0

RTC_DATA_ATTR static int boot_count = 0;

bool SDMMC_init() {

  ESP_LOGI(TAG, "Starting SDMMC peripheral");

  if (!SD_MMC.begin("")) // mount at root to match FILE* and fs::FS
  {
    Serial.println("Card Mount Failed");
    return false;
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  ESP_LOGI(TAG, "SD_MMC Card Size: %lluMB", cardSize);

  return true;
}

void setSystemTime() {
  time_t now;
  struct tm timeinfo;
  char strftime_buf[128];

  time(&now);
  localtime_r(&now, &timeinfo);

  if (bbesp32lib::timeIsValid()) {
    ESP_LOGI(TAG, "System time is not set.");
  } else {
    ESP_LOGI(TAG, "System time is %s", bbesp32lib::timestamp().c_str());
  }

  //    initialize_sntp - 1 hour check is default
  ESP_LOGI(TAG, "Initializing SNTP");
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, (char *)NTP_SERVER);
  sntp_init();

  // timezone
  // todo: config via timelapse camera
  ESP_LOGI(TAG, "Setting timezone");
  setenv("TZ", "AEST-10", 1);
  tzset();

  localtime_r(&now, &timeinfo);
  strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);

  // wait for time to be set
  int retry = 0;
  const int retry_count = 10;
  while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
    // ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry,
    // retry_count);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    time(&now);
    localtime_r(&now, &timeinfo);
  }
  if (bbesp32lib::timeIsValid()) {
    ESP_LOGI(TAG, "System time set from NTP");
  } else {
    ESP_LOGW(TAG, "System time could not be set");
  }
  ESP_LOGI(TAG, "System time is %s", bbesp32lib::timestamp().c_str());
}

void init_power_save() {

  // Configure dynamic frequency scaling:
  // maximum and minimum frequencies are set in sdkconfig,
  // automatic light sleep is enabled if tickless idle support is enabled.
  esp_pm_config_esp32_t pm_config;
  pm_config.max_freq_mhz = CONFIG_EXAMPLE_MAX_CPU_FREQ_MHZ;
  pm_config.min_freq_mhz = CONFIG_EXAMPLE_MIN_CPU_FREQ_MHZ;
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
  pm_config.light_sleep_enable = true;
  ESP_LOGE(TAG, "Using light_sleep_enable: camera won't work. Try alternative powersave method.");
#else
  pm_config.light_sleep_enable = false;
  ESP_LOGI(TAG, "Not using light_sleep_enable");
#endif
  ESP_LOGV(TAG, "Initializing DFS: %d to %d Mhz", pm_config.min_freq_mhz,
           pm_config.max_freq_mhz);

  ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
}

void setup() {
  try {
      ++boot_count;

    //
    // Logging
    //
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("tlc32", ESP_LOG_VERBOSE);
    esp_log_level_set("wifiangel", ESP_LOG_VERBOSE);
    esp_log_level_set("bbesp32lib", ESP_LOG_VERBOSE);
    esp_log_level_set("camera", ESP_LOG_INFO);
    esp_log_level_set("gpio", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("TimeLapseCamera", ESP_LOG_INFO);
    ESP_LOGI(TAG, "tlc32 boot:%d", boot_count);

    //
    // Miscellaneous
    //
    ESP_ERROR_CHECK(nvs_flash_init());
    
    //Serial.begin(115200);
    //Serial.flush();

    //
    //  Sleep to prevent rapid boot-crash-reboot on brownout
    //
    if (rtc_get_reset_reason(0) == 12 /* CPU_SW_RESET */ ||
        rtc_get_reset_reason(0) == 15 /* brownout */) {
      //
      ESP_LOGW(TAG, "Back to sleep... I'm out of it\n");
      ESP.deepSleep(10 /*minutes*/ * 60 * 1000000);
    }

    //
    // Storage
    //
    if (SDMMC_init()) {
      LogFile.print(String("Starting. Reset reason: ") + resetReason(0));
      if (0 && LOG_LOCAL_LEVEL >= ESP_LOG_VERBOSE) {
        vector<String> rootFiles = ls("/", 20);
        for (size_t i = 0; i < rootFiles.size(); ++i) {
          printf("%s\n", rootFiles[i].c_str());
        }
        ESP_LOGV(TAG, "--- SD LOG TAIL: ---\n%s------------\n", LogFile.tail(10).c_str());
      }
    } else {
      ESP_LOGE(TAG, "Failed to start SD_MMC filesystem. Dat bad.");
      throw std::runtime_error("Failed to start SD_MMC filesystem. Dat bad.");
    }

    if (BASIC_CAM_ONLY) {
      ESP_LOGI(TAG, "BASIC_CAM_ONLY active");
      ESP_ERROR_CHECK(my_camera_init());
        while (1) {
      //cam.takeSinglePhoto();
      ESP_ERROR_CHECK(camera_capture());
      vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    //
    // Configure cam
    //
    
    if (cam.load()) {
      ESP_LOGI(TAG, "Loaded from config file");
    } else {
      ESP_LOGI(TAG, "No config file - using defaults");
      // cam._wifiPassword = "wimepuderi";
      cam.configFTP("monitor.phisaver.com", "timelapse", "U88magine!");
      cam.startTakingPhotos();
      // cam.configFTP("10.1.1.15", "bbeeson", "imagine");
    }
    
    ESP_LOGE(TAG, "Temp Override Settings");
    //cam.configFTP("monitor.phisaver.com", "timelapse", "U88magine!");
    cam.configFTP("13.236.175.255", "timelapse", "U88magine!");
    cam._wifiSSID = "NetComm 0405";
    cam._wifiPassword = "wimepuderi";
    cam._uploadMode = TimeLapseCamera::On;
    cam.startTakingPhotos();
      

    //
    // Back to sleep (must be after cam start)
    //
    if (rtc_get_reset_reason(0) == 5 /* DEEP_SLEEP */ && cam.sleepy()) {
      cam.sleep();
    }

    //
    // Wifi
    //
    ESP_LOGI(TAG, "Connecting to WiFi...");
    WiFiAngel.configSTA(cam._wifiSSID.c_str(), cam._wifiPassword.c_str());

    if (WiFiAngel.begin(60000)) {
      ESP_LOGI(TAG, "Connected to WiFi as STA");
      setSystemTime();
    } else {
      ESP_LOGI(TAG, "Failed to connect to WiFi as STA. Opening AP.");
      WiFiAngel.configAP("tlc32", "");
      if (WiFiAngel.begin(10000)) {
        ESP_LOGI(TAG, "Connected to WiFi as AP");
      } else {
        throw std::runtime_error("Failed to connected to WiFi as AP. Dat bad.");
      }
    }
    //
    // Begin Cam
    //
    ESP_LOGV(TAG, "%s", cam.toString().c_str());
    LogFile.print(cam.toString());
    cam.taskify();

    //
    // Webserver. FreeRTOS priority 3.
    //
    ESP_LOGE(TAG, "Test: no server");
    //webServer.addHandler(&socketServer);
    //webServer.begin();
    init_power_save();
    ESP_LOGI(TAG, "Setup complete.");
    // LogFile.print(String("Setup complete. WiFi: ") + String(WiFi.status() ==
    // WL_CONNECTED));
  } catch (std::exception &e) {
    ESP_LOGE(TAG, "Setup Exception: %s", e.what());
    LogFile.print(e.what());
    ESP.deepSleep(10 /*minutes*/ * 60 * 1000000);
  }
}

void loop() {
  try {
    vTaskDelay(pdMS_TO_TICKS(60000));
    //vTaskPrintRunTimeStats();
  } catch (std::exception &e) {
    ESP_LOGE(TAG, "Loop Exception: %s", e.what());
    LogFile.print(e.what());
    ESP.deepSleep(10 /*minutes*/ * 60 * 1000000);
  }
}

extern "C" {
void app_main() {
  setup();
  while (1) {
    loop();
  }
}
}