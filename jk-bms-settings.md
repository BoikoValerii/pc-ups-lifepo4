# JK BMS B1A8S20P — заводские настройки (снято 2026-11-07 19:23)

Батарея: 4× EVE LF100LA, 13,13 В, ячейки 3,282–3,284 (Δ 2 мВ), SOC 68 %, 69,6 А·ч, T1 20,4 °C, T2 20,6 °C, CMOS 22,5 °C.
Заголовок в приложении (3H47M30S и т. п.) — это время работы BMS, а не имя устройства.

| Раздел | Параметр | Заводское | Рекомендация |
|---|---|---|---|
| Основное | Cell Count | 4 | |
| | Capacity, А·ч | 102 | |
| | Cell type | LFP | |
| Балансир | Balancer | OFF → включён | ON |
| | Bal. Delta Volt | 0,010 | |
| | Bal. Start Volt | 3,35 | **3,40** |
| | Max Bal. Current | 1,0 | |
| Напряжение | Cell OVP | 3,650 | |
| | Cell RCV | 3,600 | |
| | SOC-100% Volt | 3,580 | **3,45** (зарядка 14,0 В = 3,50/яч., иначе SOC не калибруется) |
| | Cell OVPR | 3,450 | |
| | Cell UVPR | 2,900 | |
| | SOC-0% Volt | 2,750 | |
| | Cell UVP | 2,550 | **2,70** |
| | PwrOff Volt | 2,50 | |
| Ток | Continued ChgCurr, А | 200 | 15 (по желанию) |
| | Chg OCP Delay / OCPR, с | 60 / 60 | |
| | Continued DsgCurr, А | 200 | 50 (по желанию) |
| | Dsg OCP Delay / OCPR, с | 300 / 60 | |
| | Discharge OCP 2 | ON | |
| Температура | Disable battery temp | OFF | OFF |
| | Charge OTP / OTPR, °C | 65 / 60 | |
| | Charge UTP, °C | −10,0 | **2** (LiFePO4 нельзя заряжать на морозе) |
| | Charge UTPR, °C | 0,0 | **5** |
| | Discharge OTP / OTPR, °C | 70 / 60 | |
| | Discharge UTP / UTPR, °C | −35 / −30 | |
| | CMOS OTP / OTPR, °C | 80 / 65 | |
| КЗ | SCP Delay, мкс | 5 | |
| | SCPR Timer, с | 5 | |
| Тревоги | Intermittent alarm | OFF | |
| | LCD Buzzer Trigger | 09 - MOSFET… | |
| | LCD Buzzer Trigger / Release | 100 / 95 | |
| | Button Trg. Emerg. | OFF | |
| | Emerg. Time, мин | 30 | |
| Сон | Smart Sleep | OFF | OFF |
| | Smart Sleep Volt / Time | 3,500 / 24 | |
| | Sleep Timer (статус), с | 86400 | |
| Связь | Device Address | 1 | |
| | Timed Stored Data / период, с | OFF / 3600 | |
| | User Data 1 / 2 | JK-BMS / JK-BMS | |
| | UART1 Protocol | 001 - JK B… | |
| Control | Charge / Discharge | ON / ON | |
| | Emerg / GPS / GPS | OFF / OFF / OFF | |
| | LCD Alarm | ON | |
| Калибровка (Factory) | Volt / Curr | 13,131 / 0,0 | не трогать |
| | SOC / SOH | 68 / 100 | |
| | CycleCapacity Cal. | 0,108 | |
| | Con. Wire Res. 01–07 | 0,00 | |

## Выставлено 2026-11-07 (проверено скриншотами)
Balancer ON, Bal. Start 3,40; SOC-100% Volt 3,46 (3,45 BMS не принимает — должно быть > OVPR 3,45); Cell UVP 2,70; Charge UTP 2,0 / UTPR 5,0. Токи оставлены 200 А.

Пароль: заводской — сменить.
