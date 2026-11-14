/*
 * ĐỀ 2 - HỆ THỐNG GIÁM SÁT MÔI TRƯỜNG VƯỜN CÂY CÔNG NGHIỆP (CÀ PHÊ, HỒ TIÊU)
 * ---------------------------------------------------------------------------
 * Phần cứng : ESP32 + DHT22 + Máy phun sương (PWM) + Máy bơm + Quạt
 * Giao tiếp : Serial, WiFi, MQTT (HiveMQ public broker)
 * Hệ điều hành: FreeRTOS (3 task + 2 queue)
 *
 *   Core 0 : Task truyền data  (WiFi + MQTT)
 *   Core 1 : Task đọc data     (DHT22 -> Serial -> data Queue / data Alert)
 *   Core 1 : Task cảnh báo     (data Alert -> điều khiển máy phun sương)
 *
 * Thư viện : DHT sensor library, Adafruit Unified Sensor, PubSubClient
 * Cây cần độ ẩm ổn định 40% - 60%.
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <stdarg.h>

// =============================================================
// 1. CẤU HÌNH
// =============================================================

// ----- Chân kết nối -----
constexpr uint8_t DHT_PIN  = 15;   // DHT22
constexpr uint8_t MIST_PIN = 26;   // Máy phun sương (PWM)
constexpr uint8_t PUMP_PIN = 27;   // Máy bơm   (manual)
constexpr uint8_t FAN_PIN  = 25;   // Quạt      (manual)

// ----- PWM cho máy phun sương -----
constexpr uint32_t PWM_FREQ = 1000;   // Hz
constexpr uint8_t  PWM_BITS = 8;      // 0..255

// ----- WiFi -----
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
constexpr uint32_t WIFI_RETRY_MS = 10000;

// ----- MQTT -----
const char* MQTT_HOST = "broker.hivemq.com";
constexpr uint16_t MQTT_PORT = 1883;
#define TOPIC_DATA   "vuon_cay_cn/data"       // ESP32 -> Web : JSON số liệu
#define TOPIC_STATUS "vuon_cay_cn/status"     // ESP32 -> Web : online/offline (retained, LWT)
#define TOPIC_PUMP   "vuon_cay_cn/cmd/pump"   // Web -> ESP32 : ON/OFF
#define TOPIC_FAN    "vuon_cay_cn/cmd/fan"    // Web -> ESP32 : ON/OFF

// ----- Cảm biến -----
constexpr uint32_t READ_PERIOD_MS = 2000;   // DHT22 tối thiểu 2 s / lần đọc
constexpr uint8_t  FILTER_SIZE    = 3;      // Số mẫu lọc trung bình trượt
constexpr uint8_t  MAX_READ_FAIL  = 5;      // Số lần lỗi liên tiếp -> báo hỏng cảm biến

// ----- Thuật toán phun sương (theo đề) -----
//   Mức 3: H < 30%        -> 100% công suất, 30 s
//   Mức 2: 30% <= H < 35% ->  50% công suất, 20 s
//   Mức 1: 35% <= H < 40% ->  25% công suất, 10 s
//   H >= 40%              -> tắt
constexpr float HUMI_LEVEL1 = 40.0f;
constexpr float HUMI_LEVEL2 = 35.0f;
constexpr float HUMI_LEVEL3 = 30.0f;

constexpr uint32_t MIST_REST_MS      = 5000;    // Nghỉ giữa 2 đợt phun (bảo vệ máy)
constexpr uint32_t SENSOR_TIMEOUT_MS = 10000;   // Mất dữ liệu độ ẩm quá lâu -> tắt phun sương (fail-safe)

// ----- Hàng đợi -----
constexpr uint8_t DATA_QUEUE_LEN  = 10;
constexpr uint8_t ALERT_QUEUE_LEN = 5;

// =============================================================
// 2. KIỂU DỮ LIỆU & BIẾN TOÀN CỤC
// =============================================================

// Dữ liệu đầy đủ gửi lên MQTT
struct SensorData {
  float    temp;
  float    humi;
  uint32_t ts;      // millis() lúc đo
};

// Dữ liệu cảnh báo: chỉ độ ẩm
struct AlertData {
  float    humi;
  uint32_t ts;
};

// Mô tả một mức phun sương
struct MistLevel {
  uint8_t  level;    // 0 = không phun
  uint8_t  percent;  // % công suất
  uint32_t durMs;    // thời gian phun
};

// Trạng thái hệ thống dùng chung giữa các task (bảo vệ bằng mutex)
struct SharedState {
  int  mistPercent;
  bool pumpOn;
  bool fanOn;
  bool sensorOk;
  bool wifiOk;
  bool mqttOk;
};

static_assert(sizeof(SensorData) <= 32, "SensorData qua lon cho bo dem sendLatest");
static_assert(sizeof(AlertData)  <= 32, "AlertData qua lon cho bo dem sendLatest");

QueueHandle_t     dataQueue;     // "data Queue"
QueueHandle_t     alertQueue;    // "data Alert"
SemaphoreHandle_t stateMutex;
SharedState       gState = {0, false, false, true, false, false};

DHT          dht(DHT_PIN, DHT22);
WiFiClient   espClient;
PubSubClient mqtt(espClient);

// =============================================================
// 3. HÀM TIỆN ÍCH
// =============================================================

// In log Serial có thời gian, core, nhãn
void logf(const char* tag, const char* fmt, ...) {
  char msg[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  Serial.printf("[%7.1fs][C%d][%s] %s\n", millis() / 1000.0, xPortGetCoreID(), tag, msg);
}

// ----- Truy cập trạng thái dùng chung an toàn -----
SharedState getState() {
  SharedState s;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  s = gState;
  xSemaphoreGive(stateMutex);
  return s;
}

void setSensorOk(bool ok) { xSemaphoreTake(stateMutex, portMAX_DELAY); gState.sensorOk = ok; xSemaphoreGive(stateMutex); }
void setWifiOk(bool ok)   { xSemaphoreTake(stateMutex, portMAX_DELAY); gState.wifiOk   = ok; xSemaphoreGive(stateMutex); }
void setMqttOk(bool ok)   { xSemaphoreTake(stateMutex, portMAX_DELAY); gState.mqttOk   = ok; xSemaphoreGive(stateMutex); }

// ----- Điều khiển cơ cấu chấp hành -----
void setMist(int percent) {
  percent = constrain(percent, 0, 100);
  ledcWrite(MIST_PIN, percent * ((1 << PWM_BITS) - 1) / 100);
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  gState.mistPercent = percent;
  xSemaphoreGive(stateMutex);
  if (percent > 0) logf("PHUN SUONG", "BAT %d%% cong suat", percent);
  else             logf("PHUN SUONG", "TAT");
}

void setPump(bool on) {
  digitalWrite(PUMP_PIN, on ? HIGH : LOW);
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  gState.pumpOn = on;
  xSemaphoreGive(stateMutex);
  logf("MAY BOM", "%s", on ? "BAT" : "TAT");
}

void setFan(bool on) {
  digitalWrite(FAN_PIN, on ? HIGH : LOW);
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  gState.fanOn = on;
  xSemaphoreGive(stateMutex);
  logf("QUAT", "%s", on ? "BAT" : "TAT");
}

// Gửi vào queue; nếu đầy thì bỏ phần tử cũ nhất để luôn giữ dữ liệu mới nhất
void sendLatest(QueueHandle_t q, const void* item) {
  if (xQueueSend(q, item, 0) != pdTRUE) {
    uint8_t trash[32];
    xQueueReceive(q, trash, 0);
    xQueueSend(q, item, 0);
  }
}

// Xác định mức phun sương theo độ ẩm
MistLevel evaluateMist(float humi) {
  if (humi < HUMI_LEVEL3) return {3, 100, 30000};
  if (humi < HUMI_LEVEL2) return {2,  50, 20000};
  if (humi < HUMI_LEVEL1) return {1,  25, 10000};
  return {0, 0, 0};
}

// =============================================================
// 4. WIFI & MQTT
// =============================================================

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      setWifiOk(true);
      logf("WIFI", "Da ket noi, IP: %s, RSSI: %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (getState().wifiOk) logf("WIFI", "Mat ket noi WiFi");
      setWifiOk(false);
      setMqttOk(false);
      break;
    default:
      break;
  }
}

// Nhận lệnh điều khiển manual từ Web: payload "ON" hoặc "OFF"
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  msg.toUpperCase();
  bool on = (msg == "ON" || msg == "1" || msg == "TRUE");

  logf("MQTT", "Nhan lenh %s = %s", topic, msg.c_str());
  if (strcmp(topic, TOPIC_PUMP) == 0)      setPump(on);
  else if (strcmp(topic, TOPIC_FAN) == 0)  setFan(on);
}

// Kết nối MQTT (có LWT để Web biết khi ESP32 mất điện/mạng)
bool connectMqtt() {
  String clientId = "esp32-vuon-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  logf("MQTT", "Dang ket noi %s:%d ...", MQTT_HOST, MQTT_PORT);
  if (mqtt.connect(clientId.c_str(), NULL, NULL, TOPIC_STATUS, 1, true, "offline")) {
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_PUMP);
    mqtt.subscribe(TOPIC_FAN);
    setMqttOk(true);
    logf("MQTT", "Da ket noi broker");
    return true;
  }
  logf("MQTT", "That bai, ma loi = %d", mqtt.state());
  setMqttOk(false);
  return false;
}

// Đóng gói và gửi dữ liệu lên broker
void publishData(const SensorData& d) {
  SharedState s = getState();
  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"t\":%.1f,\"h\":%.1f,\"mist\":%d,\"pump\":%d,\"fan\":%d,\"rssi\":%d,\"up\":%lu}",
           d.temp, d.humi, s.mistPercent, s.pumpOn, s.fanOn, WiFi.RSSI(), millis() / 1000);
  if (mqtt.publish(TOPIC_DATA, buf)) logf("MQTT", "Gui: %s", buf);
  else                               logf("MQTT", "Gui that bai");
}

// =============================================================
// 5. CÁC TASK FREERTOS
// =============================================================

// ---------------- TASK TRUYỀN DATA - Core 0 ----------------
// Kết nối WiFi; nếu thành công thì đọc "data Queue" và gửi lên MQTT broker.
void taskTruyenData(void* pv) {
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  logf("WIFI", "Dang ket noi %s ...", WIFI_SSID);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(30);
  mqtt.setBufferSize(256);

  uint32_t lastWifiTry = millis();
  uint32_t lastMqttTry = 0;

  for (;;) {
    // --- 1. Kiểm tra WiFi ---
    if (WiFi.status() != WL_CONNECTED) {
      if (millis() - lastWifiTry > WIFI_RETRY_MS) {   // thử nối lại định kỳ
        logf("WIFI", "Thu ket noi lai ...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        lastWifiTry = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    // --- 2. Kiểm tra MQTT ---
    if (!mqtt.connected()) {
      setMqttOk(false);
      if (millis() - lastMqttTry > 3000 || lastMqttTry == 0) {
        lastMqttTry = millis();
        connectMqtt();
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // --- 3. WiFi + MQTT đều tốt: xử lý gói tin và gửi dữ liệu ---
    mqtt.loop();

    SensorData d;
    if (xQueueReceive(dataQueue, &d, pdMS_TO_TICKS(100)) == pdTRUE) {
      publishData(d);
    }
  }
}

// ---------------- TASK ĐỌC DATA - Core 1 ----------------
// Đọc DHT22, hiển thị Serial, đẩy dữ liệu vào "data Queue" và "data Alert".
void taskDocData(void* pv) {
  dht.begin();

  float bufT[FILTER_SIZE], bufH[FILTER_SIZE];
  uint8_t count = 0, idx = 0, failCount = 0;
  uint32_t lastStackLog = 0;

  for (;;) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (isnan(h) || isnan(t)) {
      failCount++;
      logf("DHT22", "Loi doc cam bien (%u/%u)", failCount, MAX_READ_FAIL);
      if (failCount >= MAX_READ_FAIL && getState().sensorOk) {
        setSensorOk(false);
        logf("DHT22", "CANH BAO: cam bien khong phan hoi, kiem tra day noi!");
      }
    } else {
      if (failCount > 0 || !getState().sensorOk) logf("DHT22", "Cam bien hoat dong tro lai");
      failCount = 0;
      setSensorOk(true);

      // Lọc trung bình trượt để giảm nhiễu
      bufT[idx] = t;
      bufH[idx] = h;
      idx = (idx + 1) % FILTER_SIZE;
      if (count < FILTER_SIZE) count++;

      float sumT = 0, sumH = 0;
      for (uint8_t i = 0; i < count; i++) { sumT += bufT[i]; sumH += bufH[i]; }
      float avgT = sumT / count;
      float avgH = sumH / count;

      // Hiển thị Serial
      Serial.println("------------------------------------");
      Serial.printf("Nhiet do : %.1f C\n", avgT);
      Serial.printf("Do am    : %.1f %%  %s\n", avgH,
                    avgH < HUMI_LEVEL1 ? "(THAP - can phun suong)" :
                    avgH > 60.0f       ? "(CAO)" : "(on dinh)");

      // Gửi vào các hàng đợi
      uint32_t now = millis();
      SensorData d = {avgT, avgH, now};
      AlertData  a = {avgH, now};
      sendLatest(dataQueue, &d);    // dữ liệu đầy đủ -> task truyền data
      sendLatest(alertQueue, &a);   // dữ liệu cảnh báo (độ ẩm) -> task cảnh báo
    }

    // In mức stack còn trống mỗi 30 s (giúp kiểm tra cấp phát stack hợp lý)
    if (millis() - lastStackLog > 30000) {
      lastStackLog = millis();
      logf("RTOS", "Stack con trong cua DocData: %u bytes", uxTaskGetStackHighWaterMark(NULL) * 4);
    }

    vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
  }
}

// ---------------- TASK CẢNH BÁO - Core 1 ----------------
// Đọc "data Alert", điều khiển máy phun sương theo thuật toán 3 mức.
// Máy trạng thái: IDLE -> RUNNING -> REST -> IDLE
enum MistState { MIST_IDLE, MIST_RUNNING, MIST_REST };

void taskCanhBao(void* pv) {
  MistState state = MIST_IDLE;
  MistLevel cur = {0, 0, 0};
  uint32_t stateStart = 0;
  uint32_t lastAlertMs = millis();

  for (;;) {
    AlertData a;
    if (xQueueReceive(alertQueue, &a, pdMS_TO_TICKS(100)) == pdTRUE) {
      lastAlertMs = millis();
      MistLevel lv = evaluateMist(a.humi);
      logf("CANH BAO", "Do am %.1f%% -> muc %u", a.humi, lv.level);

      switch (state) {
        case MIST_IDLE:
          if (lv.level > 0) {                       // bắt đầu đợt phun mới
            cur = lv;
            setMist(cur.percent);
            logf("CANH BAO", "Phun %u%% trong %lu s", cur.percent, cur.durMs / 1000);
            stateStart = millis();
            state = MIST_RUNNING;
          }
          break;

        case MIST_RUNNING:
          if (lv.level == 0) {                      // đã đủ ẩm -> dừng ngay
            setMist(0);
            logf("CANH BAO", "Do am dat nguong, dung phun");
            state = MIST_IDLE;
          } else if (lv.level > cur.level) {        // độ ẩm giảm sâu hơn -> nâng mức
            cur = lv;
            setMist(cur.percent);
            logf("CANH BAO", "Nang muc: %u%% trong %lu s", cur.percent, cur.durMs / 1000);
            stateStart = millis();
          }
          break;

        case MIST_REST:
          if (lv.level == 0) state = MIST_IDLE;     // đủ ẩm trong lúc nghỉ
          break;
      }
    }

    uint32_t now = millis();

    // Hết thời gian phun -> tắt, chuyển sang nghỉ (không chặn các task khác)
    if (state == MIST_RUNNING && now - stateStart >= cur.durMs) {
      setMist(0);
      logf("CANH BAO", "Het %lu s, tat phun suong, nghi %lu s", cur.durMs / 1000, MIST_REST_MS / 1000);
      stateStart = now;
      state = MIST_REST;
    }

    // Hết thời gian nghỉ -> sẵn sàng cho đợt phun tiếp theo
    if (state == MIST_REST && now - stateStart >= MIST_REST_MS) {
      state = MIST_IDLE;
    }

    // Fail-safe: mất dữ liệu độ ẩm quá lâu khi đang phun -> tắt để tránh phun mù quáng
    if (state == MIST_RUNNING && now - lastAlertMs > SENSOR_TIMEOUT_MS) {
      setMist(0);
      logf("CANH BAO", "Mat du lieu do am > %lu s, TAT phun suong (fail-safe)", SENSOR_TIMEOUT_MS / 1000);
      state = MIST_IDLE;
    }
  }
}

// =============================================================
// 6. SETUP / LOOP
// =============================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("====================================================");
  Serial.println(" GIAM SAT VUON CAY CONG NGHIEP (CA PHE, HO TIEU)");
  Serial.println(" ESP32 + DHT22 + MQTT + FreeRTOS");
  Serial.println(" Nguong do am on dinh: 40% - 60%");
  Serial.println("====================================================");

  // Ngõ ra
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);

  // PWM cho máy phun sương (arduino-esp32 v3.x)
  // Với v2.x: ledcSetup(0, PWM_FREQ, PWM_BITS); ledcAttachPin(MIST_PIN, 0); và ledcWrite(0, duty)
  ledcAttach(MIST_PIN, PWM_FREQ, PWM_BITS);
  ledcWrite(MIST_PIN, 0);

  // Mutex + hàng đợi
  stateMutex = xSemaphoreCreateMutex();
  dataQueue  = xQueueCreate(DATA_QUEUE_LEN,  sizeof(SensorData));   // "data Queue"
  alertQueue = xQueueCreate(ALERT_QUEUE_LEN, sizeof(AlertData));    // "data Alert"

  if (!stateMutex || !dataQueue || !alertQueue) {
    Serial.println("LOI: khong tao duoc mutex/queue, dung chuong trinh!");
    while (true) delay(1000);
  }

  // Tạo task và ghim vào core
  xTaskCreatePinnedToCore(taskTruyenData, "TruyenData", 8192, NULL, 1, NULL, 0);  // Core 0
  xTaskCreatePinnedToCore(taskDocData,    "DocData",    4096, NULL, 2, NULL, 1);  // Core 1
  xTaskCreatePinnedToCore(taskCanhBao,    "CanhBao",    4096, NULL, 2, NULL, 1);  // Core 1

  logf("RTOS", "Da tao 3 task: TruyenData(C0), DocData(C1), CanhBao(C1)");
}

void loop() {
  vTaskDelete(NULL);   // mọi việc do các task đảm nhận
}
