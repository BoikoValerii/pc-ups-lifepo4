// Скопируйте в secrets.h (рядом с .ino) и впишите свои значения.
// secrets.h в репозиторий не попадает (.gitignore).
#pragma once

// Сеть Wi-Fi 2,4 ГГц
#define WIFI_SSID  "сеть"
#define WIFI_PASS  "пароль"

// Бот Telegram и чат, куда слать сообщения
#define TG_TOKEN "123456:ABC..."
#define TG_CHAT  "123456789"

// Пароль обновления по Wi-Fi (ArduinoOTA)
#define OTA_PASS "придумайте_длинный_пароль"

// MAC JK BMS (необязательно): пусто — ищется по имени «JK…», список видимых — на странице /scan
// #define BMS_MAC "aa:bb:cc:dd:ee:ff"
