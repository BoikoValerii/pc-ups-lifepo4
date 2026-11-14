// Монитор ИБП ПК (LiFePO4 12,8 В 100 А·ч + JK BMS) → Telegram + сигнал выключения ПК
// ESP32 DevKit V1. Данные батареи — из JK BMS по Bluetooth (протокол JK02), сеть — делитель на D13
// от +24 В блока зарядки PROLUM. ПК опрашивает http://pc-ups.local/data и выключается по флагу shutdown.
// Настройки — в secrets.h (шаблон: secrets.example.h).

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <NimBLEDevice.h>   // не встроенный BLE: тот съедает память, и на HTTPS к Telegram не остаётся

// Без встроенной BLE-библиотеки ядро при старте освобождает память контроллера Bluetooth,
// и NimBLE потом не запускается. Сообщаем ядру, что Bluetooth нужен.
bool btInUse() { return true; }
#include <time.h>
#include <Preferences.h>
#include <esp_system.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, TG_TOKEN, TG_CHAT, OTA_PASS

#ifndef BMS_MAC
#define BMS_MAC ""     // пусто — ищем по имени «JK…»; можно вписать MAC из /scan
#endif

// ---------- Пины ----------
const int PIN_MAINS = 13;      // D13: делитель 100k / (20k ∥ 47k) от +24 В PROLUM (~3,0 В = есть сеть)

// ---------- Пороги ----------
const int SOC_WARN          = 30;    // предупреждение
const int SOC_SHUTDOWN      = 20;    // команда ПК выключиться
const float CELL_SHUTDOWN_V = 3.00;  // или любая ячейка ниже (страховка, если SOC врёт)
const float CELL_DELTA_WARN = 0.05;  // разбаланс ячеек
const float TEMP_WARN_C     = 45.0;
const uint32_t MAINS_CONFIRM_MS = 5000;
const uint32_t OUTAGE_STATUS_MS = 3600UL * 1000;   // сводка каждый час без света
const uint32_t BMS_STALE_MS     = 60000;           // данных нет дольше — считаем устаревшими
const uint32_t BMS_LOST_MS      = 15UL * 60000;    // столько без связи — сообщение (телефон мог занять BMS)
const uint32_t BLIND_SHUTDOWN_MS = 8UL * 3600000;  // нет данных BMS и нет света 8 ч — всё равно выключить ПК

WiFiMulti wifi;
WebServer web(80);

// ---------- Данные BMS ----------
struct Bms {
  float cell[4] = {0, 0, 0, 0};
  float v = 0, i = 0, t1 = NAN, t2 = NAN, mos = NAN, remainAh = 0, capAh = 0, balI = 0;
  int soc = -1;
  uint32_t cycles = 0;
  bool chg = false, dsg = false, bal = false;
  uint32_t at = 0;             // millis() последнего кадра
  int layout = 0;              // смещение полей: 32 — JK02_32S, 0 — JK02_24S
} bms;
String bmsName, bmsHw, bmsSw, bmsAddr;

bool bmsFresh() { return bms.at && millis() - bms.at < BMS_STALE_MS; }
float cellMin() { float m = 9; for (float c : bms.cell) m = min(m, c); return m; }
float cellMax() { float m = 0; for (float c : bms.cell) m = max(m, c); return m; }

// ---------- Состояние ----------
bool mainsPin = true, mains = true, mainsRaw = true;
uint32_t mainsChangeAt = 0;
uint32_t outageStart = 0, lastOutageStatus = 0;
int outageSocStart = -1, outageSocMin = 101;
float outageAh = 0;
bool warnSent = false, shutdownSent = false, dsgOffSent = false;
bool charging = false, noChargeSent = false;
uint32_t chargedSince = 0, noChargeSince = 0, lastTempWarn = 0, lastDeltaWarn = 0;
int outagesToday = 0;

// ---------- Очередь сообщений (если Wi-Fi пропал) ----------
const int QN = 10;
String queue[QN];
int qHead = 0, qLen = 0;

void enqueue(const String &m) {
  if (qLen == QN) { qHead = (qHead + 1) % QN; qLen--; }
  queue[(qHead + qLen) % QN] = m;
  qLen++;
}

String urlencode(const String &s) {
  String out;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

int tgLastCode = 0;            // последний ответ Telegram (для /data)

int tgDropped = 0;             // сколько сообщений Telegram отверг (4xx) и они выброшены
String tgLastDropped;

int tgSend(const String &text) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  http.begin(cli, String("https://api.telegram.org/bot") + TG_TOKEN + "/sendMessage");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST("chat_id=" + String(TG_CHAT) + "&text=" + urlencode(text));
  http.end();
  tgLastCode = code;
  return code;
}

// Отказ 4xx (кроме 429 «слишком часто») повтором не исправить — такое сообщение выбрасываем,
// иначе оно навсегда блокирует очередь и все следующие уведомления
void flushQueue() {
  while (qLen > 0) {
    int code = tgSend(queue[qHead]);
    bool rejected = code >= 400 && code < 500 && code != 429;
    if (code != 200 && !rejected) break;
    if (rejected) { tgDropped++; tgLastDropped = queue[qHead]; }
    qHead = (qHead + 1) % QN;
    qLen--;
  }
}

void notify(const String &m) {
  Serial.println(m);
  enqueue("ИБП ПК: " + m);
  flushQueue();
}

String fmtDur(uint32_t ms) {
  uint32_t m = ms / 60000;
  char b[16];
  snprintf(b, sizeof(b), "%lu:%02lu", m / 60, m % 60);
  return b;
}

String batLine() {
  if (!bmsFresh()) return "нет данных BMS";
  String s = "АКБ " + String(bms.soc) + "%, " + String(bms.v, 2) + " В, " + String(bms.i, 1) + " А";
  s += ", ячейки " + String(cellMin(), 3) + "–" + String(cellMax(), 3) + " В";
  if (!isnan(bms.t1)) s += ", " + String(max(bms.t1, bms.t2), 1) + " °C";
  return s;
}

// Сколько осталось до выключения ПК при текущем токе разряда
String timeLeft() {
  if (!bmsFresh() || bms.i > -0.3 || bms.capAh <= 0) return "";
  float ah = bms.remainAh - bms.capAh * SOC_SHUTDOWN / 100.0;
  if (ah <= 0) return "";
  return ", до выключения ПК ~" + fmtDur(ah / -bms.i * 3600000.0);
}

// ---------- JK BMS по Bluetooth ----------
// Сервис FFE0, характеристика FFE1. Команда — 20 байт, ответ — кадр 300 байт из нескольких уведомлений:
// 55 AA EB 90 <тип> …, тип 02 — ячейки/ток/SOC, 03 — сведения об устройстве; байт 299 — сумма 0..298.
static NimBLEUUID SVC_UUID((uint16_t)0xFFE0), CHR_UUID((uint16_t)0xFFE1);
NimBLEClient *bleClient = nullptr;
NimBLERemoteCharacteristic *bleChr = nullptr;
uint32_t blePausedUntil = 0, lastScan = 0, lastCellCmd = 0;
String scanLog;

portMUX_TYPE frameMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t rx[400];
size_t rxLen = 0;
uint8_t cellFrame[300], infoFrame[300];
volatile bool cellReady = false, infoReady = false;
uint8_t lastFrame[300];        // для /raw
bool haveLastFrame = false;

void onNotify(NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) {
  if (n >= 4 && d[0] == 0x55 && d[1] == 0xAA && d[2] == 0xEB && d[3] == 0x90) rxLen = 0;
  if (rxLen + n > sizeof(rx)) { rxLen = 0; return; }
  memcpy(rx + rxLen, d, n);
  rxLen += n;
  if (rxLen < 300) return;
  uint8_t crc = 0;
  for (int k = 0; k < 299; k++) crc += rx[k];
  if (crc == rx[299] && rx[0] == 0x55) {
    portENTER_CRITICAL(&frameMux);
    if (rx[4] == 0x02) { memcpy(cellFrame, rx, 300); cellReady = true; }
    else if (rx[4] == 0x03) { memcpy(infoFrame, rx, 300); infoReady = true; }
    portEXIT_CRITICAL(&frameMux);
  }
  rxLen = 0;
}

void jkCommand(uint8_t addr) {
  if (!bleChr) return;
  uint8_t f[20] = {0xAA, 0x55, 0x90, 0xEB, addr};
  uint8_t crc = 0;
  for (int k = 0; k < 19; k++) crc += f[k];
  f[19] = crc;
  bleChr->writeValue(f, 20, false);
}

uint16_t u16(const uint8_t *b, int i) { return b[i] | (b[i + 1] << 8); }
uint32_t u32(const uint8_t *b, int i) { return b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | ((uint32_t)b[i + 3] << 24); }

// Положение полей зависит от прошивки BMS (24 или 32 ячейки в кадре): выбираем то, где напряжение
// пакета совпадает с суммой ячеек
void parseCells(const uint8_t *f) {
  float c[4], sum = 0;
  for (int k = 0; k < 4; k++) { c[k] = u16(f, 6 + 2 * k) * 0.001; sum += c[k]; }
  int off = -1;
  for (int o : {32, 0})
    if (fabs(u32(f, 118 + o) * 0.001 - sum) < 0.5) { off = o; break; }
  if (off < 0 || sum < 8) { Serial.printf("BMS: кадр не распознан (сумма ячеек %.2f)\n", sum); return; }
  for (int k = 0; k < 4; k++) bms.cell[k] = c[k];
  bms.layout = off;
  bms.v = u32(f, 118 + off) * 0.001;
  bms.i = (int32_t)u32(f, 126 + off) * 0.001;           // + заряд, − разряд
  bms.t1 = (int16_t)u16(f, 130 + off) * 0.1;
  bms.t2 = (int16_t)u16(f, 132 + off) * 0.1;
  bms.mos = (int16_t)u16(f, off ? 112 + off : 134) * 0.1;
  bms.balI = (int16_t)u16(f, 138 + off) * 0.001;
  bms.soc = f[141 + off];
  bms.remainAh = u32(f, 142 + off) * 0.001;
  bms.capAh = u32(f, 146 + off) * 0.001;
  bms.cycles = u32(f, 150 + off);
  bms.chg = f[166 + off];
  bms.dsg = f[167 + off];
  bms.bal = f[168 + off];
  bms.at = millis();
}

String fixedStr(const uint8_t *f, int i, int n) {
  String s;
  for (int k = 0; k < n && f[i + k]; k++) if (f[i + k] >= 32 && f[i + k] < 127) s += (char)f[i + k];
  return s;
}

void parseInfo(const uint8_t *f) {   // пароль BMS (байты 62..77) не читаем и не показываем
  bmsHw = fixedStr(f, 22, 8);
  bmsSw = fixedStr(f, 30, 8);
  bmsName = fixedStr(f, 46, 16);
}

void bmsDisconnect() {
  if (bleClient && bleClient->isConnected()) bleClient->disconnect();
  bleChr = nullptr;
}

// Полный сброс Bluetooth: после обрыва связи клиент NimBLE мог больше не подключаться
void bleReset() {
  bmsDisconnect();
  NimBLEDevice::deinit(true);
  bleClient = nullptr;
  rxLen = 0;
  delay(200);
  NimBLEDevice::init("pc-ups");
  lastScan = 0;
}

bool bmsConnect() {
  // Клиент после обрыва связи не переподключался (BMS видна, а соединения нет часами) — создаём заново
  if (bleClient && !bleClient->isConnected()) {
    NimBLEDevice::deleteClient(bleClient);
    bleClient = nullptr;
    bleChr = nullptr;
  }
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  bool started = scan->start(5000, false, true);
  for (int k = 0; k < 60 && scan->isScanning(); k++) delay(100);
  NimBLEScanResults res = scan->getResults();
  Serial.printf("BLE scan: started=%d init=%d count=%d\n", started, NimBLEDevice::isInitialized(), res.getCount());
  String mac = BMS_MAC;
  mac.toLowerCase();
  const NimBLEAdvertisedDevice *dev = nullptr;
  scanLog = "видно устройств: " + String(res.getCount()) + "\n";
  for (int k = 0; k < res.getCount(); k++) {
    const NimBLEAdvertisedDevice *d = res.getDevice(k);
    String addr = d->getAddress().toString().c_str(), name = d->getName().c_str();
    scanLog += addr + "  " + name + "  " + d->getRSSI() + " dBm" +
               (d->isAdvertisingService(SVC_UUID) ? "  FFE0" : "") + "\n";
    if (!dev && (mac.length() ? addr == mac : name.startsWith("JK"))) dev = d;
  }
  bool ok = false;
  if (dev) {
    if (!bleClient) bleClient = NimBLEDevice::createClient();
    if (bleClient->connect(dev)) {
      NimBLERemoteService *svc = bleClient->getService(SVC_UUID);
      bleChr = svc ? svc->getCharacteristic(CHR_UUID) : nullptr;
      if (bleChr && bleChr->canNotify() && bleChr->subscribe(true, onNotify)) {
        bmsAddr = dev->getAddress().toString().c_str();
        if (bmsName.isEmpty()) bmsName = dev->getName().c_str();
        ok = true;
      } else bmsDisconnect();
    }
  }
  scan->clearResults();
  if (!ok) return false;
  jkCommand(0x97);             // сведения об устройстве
  delay(300);
  jkCommand(0x96);             // поток данных ячеек
  lastCellCmd = millis();
  Serial.println("BMS подключена: " + bmsAddr);
  return true;
}

void handleBle(uint32_t now) {
  if (cellReady || infoReady) {
    uint8_t c[300], in[300];
    bool hc, hi;
    portENTER_CRITICAL(&frameMux);
    hc = cellReady; hi = infoReady;
    if (hc) memcpy(c, cellFrame, 300);
    if (hi) memcpy(in, infoFrame, 300);
    cellReady = infoReady = false;
    portEXIT_CRITICAL(&frameMux);
    if (hc) { parseCells(c); memcpy(lastFrame, c, 300); haveLastFrame = true; }
    if (hi) parseInfo(in);
  }
  if (now < blePausedUntil) return;
  bool connected = bleClient && bleClient->isConnected() && bleChr;
  if (connected) {
    // Поток остановился — переспросить, не помогло — переподключиться
    if (now - bms.at > 20000 && now - lastCellCmd > 20000) { jkCommand(0x96); lastCellCmd = now; }
    if (bms.at && now - bms.at > 90000) bmsDisconnect();
    return;
  }
  if (now - lastScan < 20000) return;
  lastScan = now;
  bmsConnect();
}

// ---------- Сеть, отключения, выключение ПК ----------
bool shutdownNeeded() {
  if (mains) return false;
  if (bmsFresh()) return bms.soc <= SOC_SHUTDOWN || cellMin() <= CELL_SHUTDOWN_V;
  return millis() - outageStart > BLIND_SHUTDOWN_MS;
}

void handleMains(uint32_t now) {
  bool raw = mainsPin;
  if (raw != mainsRaw) { mainsRaw = raw; mainsChangeAt = now; }
  if (mainsRaw == mains || now - mainsChangeAt < MAINS_CONFIRM_MS) return;
  mains = mainsRaw;

  if (!mains) {
    outageStart = lastOutageStatus = now;
    outageSocStart = bmsFresh() ? bms.soc : -1;
    outageSocMin = 101;
    outageAh = 0;
    warnSent = shutdownSent = false;
    outagesToday++;
    notify("свет пропал. " + batLine());
  } else {
    charging = false;
    noChargeSince = 0;
    noChargeSent = false;
    String s = "свет вернулся через " + fmtDur(now - outageStart) + ". Израсходовано " +
               String(outageAh, 1) + " А·ч";
    if (outageSocStart >= 0 && outageSocMin <= 100)
      s += ", заряд " + String(outageSocStart) + "% → " + String(outageSocMin) + "%";
    notify(s + ". " + batLine());
  }
}

void handleOutage(uint32_t now) {
  static uint32_t lastAt = 0;
  if (mains) { lastAt = 0; return; }
  if (bmsFresh()) {
    outageSocMin = min(outageSocMin, bms.soc);
    if (lastAt && bms.i < 0) outageAh += -bms.i * (now - lastAt) / 3600000.0;
    lastAt = now;
  }
  if (shutdownNeeded() && !shutdownSent) {
    shutdownSent = warnSent = true;
    notify("выключаю ПК: " + batLine() + (bmsFresh() ? "" : ", без света " + fmtDur(now - outageStart)));
  } else if (!warnSent && bmsFresh() && bms.soc <= SOC_WARN) {
    warnSent = true;
    notify("заряд " + String(bms.soc) + "%. При " + String(SOC_SHUTDOWN) + "% ПК будет выключен" + timeLeft());
  }
  if (now - lastOutageStatus >= OUTAGE_STATUS_MS) {
    lastOutageStatus = now;
    notify("без света " + fmtDur(now - outageStart) + ". " + batLine() + timeLeft());
  }
}

void handleCharging(uint32_t now) {
  if (!mains || !bmsFresh()) return;
  // Сеть есть, а ток в батарею не идёт — PROLUM, XL4016, D3, предохранитель
  if (bms.soc < 95 && bms.i < 0.3) {
    if (!noChargeSince) noChargeSince = now;
    if (!noChargeSent && now - noChargeSince > 600000UL) {
      noChargeSent = true;
      notify("сеть есть, но зарядка не идёт: " + batLine() + ". Проверьте XL4016, D3 и предохранитель");
    }
  } else noChargeSince = 0;

  if (bms.i > 1.0) { charging = true; chargedSince = 0; }
  else if (charging && bms.i < 0.3) {
    if (!chargedSince) chargedSince = now;
    if (now - chargedSince > 600000UL) {
      charging = false;
      notify("АКБ заряжена: " + batLine());
    }
  } else chargedSince = 0;
}

void handleAlarms(uint32_t now) {
  static uint32_t lostSince = 0;
  static bool lostSent = false;
  if (bmsFresh()) {
    if (lostSent) notify("связь с BMS восстановлена. " + batLine());
    lostSince = 0;
    lostSent = false;
  } else if (now >= blePausedUntil) {
    if (!lostSince) lostSince = now;
    if (!lostSent && now - lostSince > BMS_LOST_MS) {
      lostSent = true;
      notify("нет связи с BMS " + fmtDur(now - lostSince) + " (приложение на телефоне занимает Bluetooth?)");
    }
  }
  if (!bmsFresh()) return;

  // Разряд отключила сама BMS — ПК и монитор уже без питания
  if (!bms.dsg && !dsgOffSent) { dsgOffSent = true; notify("BMS ОТКЛЮЧИЛА РАЗРЯД (защита). " + batLine()); }
  if (bms.dsg) dsgOffSent = false;

  if (cellMax() - cellMin() > CELL_DELTA_WARN && now - lastDeltaWarn > 6 * 3600000UL) {
    lastDeltaWarn = now;
    notify("разбаланс ячеек " + String((cellMax() - cellMin()) * 1000, 0) + " мВ: " +
           String(bms.cell[0], 3) + " / " + String(bms.cell[1], 3) + " / " +
           String(bms.cell[2], 3) + " / " + String(bms.cell[3], 3));
  }
  float t = max(bms.t1, bms.t2);
  if (t > TEMP_WARN_C && now - lastTempWarn > 3600000UL) {
    lastTempWarn = now;
    notify("перегрев батареи: " + String(t, 1) + " °C");
  }
}

// ---------- Сторож Wi-Fi: в ядре 3.3 WiFiMulti после потери связи может не переподключиться.
// Состояние отключения света переживает программный сброс.
const uint32_t RTC_MAGIC = 0x50435550;
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR time_t rtcOutageEpoch;
RTC_NOINIT_ATTR int rtcSocStart, rtcSocMin;
RTC_NOINIT_ATTR float rtcAh;
RTC_NOINIT_ATTR uint32_t rtcBleRestarts;   // перезапуски подряд из-за BMS — не больше 3, пока данные не придут

void saveOutageAndRestart() {
  rtcMagic = mains ? 0 : RTC_MAGIC;
  rtcOutageEpoch = time(nullptr) - (millis() - outageStart) / 1000;
  rtcSocStart = outageSocStart;
  rtcSocMin = outageSocMin;
  rtcAh = outageAh;
  ESP.restart();
}

void restoreOutage() {
  if (esp_reset_reason() != ESP_RST_SW || rtcMagic != RTC_MAGIC || mains) return;
  rtcMagic = 0;
  time_t elapsed = time(nullptr) - rtcOutageEpoch;
  if (elapsed > 0 && elapsed < 7 * 86400 && (uint32_t)elapsed * 1000 < millis() + 7UL * 86400 * 1000)
    outageStart = millis() - (uint32_t)elapsed * 1000;
  outageSocStart = rtcSocStart;
  outageSocMin = rtcSocMin;
  outageAh = rtcAh;
  outagesToday--;
}

// ---------- Чёрный ящик: причины перезапуска и попытки связи, уходят в Telegram после подключения ----------
Preferences diagStore;
int discReasons[4] = {0, 0, 0, 0};
int discN = 0;

void diagAppend(const String &line) {
  String log = diagStore.getString("log", "");
  log += line + "\n";
  if (log.length() > 1500) {
    // Хвост по целым строкам: обрезка по байтам рвала русскую букву, и Telegram отвечал 400
    int cut = log.indexOf('\n', log.length() - 1500);
    log = cut >= 0 ? log.substring(cut + 1) : String();
  }
  diagStore.putString("log", log);
  Serial.print("diag: " + line + "\n");
}

const char *resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "включение питания";
    case ESP_RST_BROWNOUT: return "ПРОСАДКА ПИТАНИЯ (brownout)";
    case ESP_RST_PANIC:    return "падение прошивки";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "сторожевой таймер";
    case ESP_RST_SW:       return "программный сброс";
    default:               return "другое";
  }
}

void diagAttempt() {
  int n = WiFi.scanNetworks(false, false, false, 300);
  String s = "TX " + String(WiFi.getTxPower() / 4.0, 1) + " dBm; видно " + String(max(n, 0)) + ":";
  for (int k = 0; k < n && k < 5; k++) s += " " + WiFi.SSID(k) + "(" + WiFi.RSSI(k) + ")";
  WiFi.scanDelete();
  s += "; отказы:";
  for (int k = 0; k < discN && k < 4; k++) s += " " + String(discReasons[k]);
  discN = 0;
  diagAppend(s);
}

void diagReportIfAny() {
  String log = diagStore.getString("log", "");
  if (log.length() == 0) return;
  notify("журнал связи:\n" + log);
  diagStore.putString("prev", log);    // копия для /diag, даже если Telegram её не примет
  diagStore.putString("log", "");
}

const wifi_power_t TX_LEVELS[] = {WIFI_POWER_8_5dBm, WIFI_POWER_15dBm, WIFI_POWER_19_5dBm};
int txIdx = 0;

void nextTxPower() {
  txIdx = (txIdx + 1) % 3;
  WiFi.setTxPower(TX_LEVELS[txIdx]);
}

// ---------- Сторож BMS: 3 мин без данных — сброс Bluetooth, 10 мин — перезапуск ESP ----------
const uint32_t BLE_RESET_MS   = 3UL * 60000;
const uint32_t BLE_RESTART_MS = 10UL * 60000;
uint32_t bmsStaleSince = 0, lastBleReset = 0;

void bleWatchdog(uint32_t now) {
  if (bmsFresh() || now < blePausedUntil) {   // данные идут или BMS отпущена для телефона
    if (bmsFresh()) rtcBleRestarts = 0;
    bmsStaleSince = 0;
    return;
  }
  if (!bmsStaleSince) { bmsStaleSince = lastBleReset = now; return; }
  if (now - bmsStaleSince > BLE_RESTART_MS && rtcBleRestarts < 3 && !shutdownNeeded()) {
    rtcBleRestarts++;
    diagAppend("нет данных BMS " + fmtDur(now - bmsStaleSince) + " — перезапуск ESP");
    saveOutageAndRestart();
  }
  if (now - lastBleReset > BLE_RESET_MS) {
    lastBleReset = now;
    diagAppend("нет данных BMS " + fmtDur(now - bmsStaleSince) + " — сброс Bluetooth");
    bleReset();
  }
}

// ---------- Веб ----------
String jsonNum(float v, int d) { return isnan(v) ? String("null") : String(v, d); }

void handleWebData() {
  String cells = "[";
  for (int k = 0; k < 4; k++) cells += (k ? "," : "") + String(bms.cell[k], 3);
  cells += "]";
  web.send(200, "application/json",
    String("{\"mains\":") + (mains ? "true" : "false") +
    ",\"shutdown\":" + (shutdownNeeded() ? "true" : "false") +
    ",\"bms\":" + (bmsFresh() ? "true" : "false") +
    ",\"soc\":" + bms.soc + ",\"v\":" + String(bms.v, 3) + ",\"i\":" + String(bms.i, 3) +
    ",\"cells\":" + cells + ",\"t1\":" + jsonNum(bms.t1, 1) + ",\"t2\":" + jsonNum(bms.t2, 1) +
    ",\"mos\":" + jsonNum(bms.mos, 1) + ",\"remAh\":" + String(bms.remainAh, 1) +
    ",\"capAh\":" + String(bms.capAh, 1) + ",\"cycles\":" + bms.cycles +
    ",\"chg\":" + (bms.chg ? "true" : "false") + ",\"dsg\":" + (bms.dsg ? "true" : "false") +
    ",\"bal\":" + (bms.bal ? "true" : "false") + ",\"balI\":" + String(bms.balI, 3) +
    ",\"age\":" + (bms.at ? String((millis() - bms.at) / 1000) : String(-1)) + ",\"layout\":" + bms.layout +
    ",\"outage\":\"" + (mains ? String("") : fmtDur(millis() - outageStart)) + "\"" +
    ",\"blePause\":" + (millis() < blePausedUntil ? (blePausedUntil - millis()) / 1000 : 0) +
    ",\"bmsName\":\"" + bmsName + "\",\"bmsAddr\":\"" + bmsAddr + "\",\"hw\":\"" + bmsHw +
    "\",\"sw\":\"" + bmsSw + "\",\"ssid\":\"" + WiFi.SSID() + "\",\"rssi\":" + WiFi.RSSI() +
    ",\"up\":\"" + fmtDur(millis()) + "\",\"tg\":" + tgLastCode + ",\"queue\":" + qLen +
    ",\"heap\":" + ESP.getFreeHeap() + ",\"maxBlock\":" + ESP.getMaxAllocHeap() + "}");
}

void handleWebRoot() {
  web.send(200, "text/html; charset=utf-8",
    "<!doctype html><meta name=viewport content='width=device-width'><title>ИБП ПК</title>"
    "<body style='font-family:sans-serif;font-size:1.4em;padding:1em;line-height:1.5'>"
    "<div id=d>…</div><p><button onclick=\"fetch('/ble?pause=10').then(u)\">Отпустить BMS на 10 мин</button> "
    "<button onclick=\"fetch('/ble?pause=0').then(u)\">Вернуть</button></p>"
    "<div id=s style='font-size:.55em;color:#888'></div>"
    "<script>async function u(){try{const r=await (await fetch('/data')).json();"
    "d.innerHTML=(r.mains?'сеть есть':'<b>нет сети '+r.outage+'</b>')+'<br>'+"
    "(r.bms?r.soc+'% · '+r.v.toFixed(2)+' В · '+r.i.toFixed(2)+' А<br>ячейки '+r.cells.map(c=>c.toFixed(3)).join(' / ')+"
    "'<br>'+r.t1+' / '+r.t2+' °C, MOS '+r.mos+' °C<br>'+r.remAh+' из '+r.capAh+' А·ч'+(r.dsg?'':' <b>РАЗРЯД ОТКЛЮЧЁН</b>')"
    ":'нет данных BMS')+(r.blePause?'<br>BMS отпущена ещё '+r.blePause+' с':'')+(r.shutdown?'<br><b>ПК выключается</b>':'');"
    "s.textContent=r.bmsName+' '+r.bmsAddr+' · '+r.ssid+' '+r.rssi+' dBm · работает '+r.up}catch(e){s.textContent='нет связи'}}"
    "u();setInterval(u,2000)</script>");
}

void handleWebBle() {
  int m = web.arg("pause").toInt();
  blePausedUntil = m > 0 ? millis() + m * 60000UL : 0;
  if (m > 0) bmsDisconnect();
  lastScan = 0;
  web.send(200, "text/plain; charset=utf-8", m > 0 ? "BMS отпущена на " + String(m) + " мин" : "BMS снова опрашивается");
}

void handleWebScan() { web.send(200, "text/plain; charset=utf-8", scanLog.length() ? scanLog : "пусто — подождите 20 с"); }

void handleWebDiag() {
  String s = String("запуск: ") + resetReasonText(esp_reset_reason()) + ", работает " + fmtDur(millis()) +
             "\nTelegram: последний ответ " + tgLastCode + ", в очереди " + qLen + ", отвергнуто " + tgDropped +
             "\n\n--- журнал (ещё не отправлен) ---\n" + diagStore.getString("log", "") +
             "\n--- последний отправленный журнал ---\n" + diagStore.getString("prev", "") +
             "\n--- последнее отвергнутое Telegram ---\n" + tgLastDropped + "\n";
  web.send(200, "text/plain; charset=utf-8", s);
}

void handleWebRaw() {
  String s;
  for (int k = 0; k < 300 && haveLastFrame; k++) {
    char b[4];
    snprintf(b, sizeof(b), "%02X ", lastFrame[k]);
    s += b;
    if (k % 16 == 15) s += "\n";
  }
  web.send(200, "text/plain", s);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_MAINS, INPUT);   // без подтяжки: внутренние ~45k параллельно R2 опускают сигнал ниже порога

  if (esp_reset_reason() != ESP_RST_SW) rtcBleRestarts = 0;   // после включения питания в RTC мусор
  diagStore.begin("diag");
  esp_reset_reason_t rr = esp_reset_reason();
  if (rr != ESP_RST_POWERON && rr != ESP_RST_SW)   // обычное включение и OTA в журнал не пишем
    diagAppend(String("запуск: ") + resetReasonText(rr));

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    if (discN < 4) discReasons[discN++] = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  wifi.addAP(WIFI_SSID, WIFI_PASS);
  for (int k = 0; k < 3 && wifi.run(10000) != WL_CONNECTED; k++) { diagAttempt(); nextTxPower(); }
  ArduinoOTA.setHostname("pc-ups");
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.begin();            // заодно mDNS: pc-ups.local
  web.on("/", handleWebRoot);
  web.on("/data", handleWebData);
  web.on("/ble", handleWebBle);
  web.on("/scan", handleWebScan);
  web.on("/raw", handleWebRaw);
  web.on("/diag", handleWebDiag);
  web.begin();
  // NTP по IP: в ядре 3.3 DNS-запрос SNTP, совпавший с HTTPS к Telegram, валит плату
  configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "216.239.35.0", "162.159.200.123", "216.239.35.4");

  NimBLEDevice::init("pc-ups");
  bmsConnect();
  for (int k = 0; k < 30 && !cellReady; k++) delay(100);   // первый кадр — чтобы данные были в стартовом сообщении
  handleBle(millis());

  mainsPin = digitalRead(PIN_MAINS) == HIGH;
  mains = mainsRaw = mainsPin;
  outageStart = lastOutageStatus = millis();
  outageSocStart = bmsFresh() ? bms.soc : -1;
  if (!mains) outagesToday++;
  restoreOutage();
  notify(String("монитор запущен (Wi-Fi ") + WiFi.SSID() + ", " + WiFi.RSSI() + " dBm, http://" +
         WiFi.localIP().toString() + "). " + (mains ? "Сеть есть. " : "Сети нет. ") + batLine());
  if (WiFi.status() == WL_CONNECTED) diagReportIfAny();
}

void loop() {
  static uint32_t lastTick = 0, lastFlush = 0;
  ArduinoOTA.handle();
  web.handleClient();
  uint32_t now = millis();
  handleBle(now);
  if (now - lastTick < 1000) return;
  lastTick = now;

  static uint32_t lastWifi = 0, wifiLostAt = 0;
  if (WiFi.status() == WL_CONNECTED) wifiLostAt = 0;
  else if (!wifiLostAt) wifiLostAt = now;
  else if (now - wifiLostAt > 300000 && !shutdownNeeded()) {   // во время выключения ПК не перезапускаемся
    diagAppend("нет Wi-Fi 5 мин — перезапуск");
    saveOutageAndRestart();
  }
  if (WiFi.status() != WL_CONNECTED && now - lastWifi > 30000) {
    lastWifi = now;
    diagAttempt();
    nextTxPower();
    if (wifi.run(10000) == WL_CONNECTED) {
      notify(String("Wi-Fi восстановлен: ") + WiFi.SSID() + ", " + WiFi.RSSI() + " dBm, http://" +
             WiFi.localIP().toString());
      diagReportIfAny();
    }
  }

  mainsPin = digitalRead(PIN_MAINS) == HIGH;
  handleMains(now);
  handleOutage(now);
  handleCharging(now);
  handleAlarms(now);
  bleWatchdog(now);

  if (qLen && now - lastFlush > 30000) { lastFlush = now; flushQueue(); }
}
