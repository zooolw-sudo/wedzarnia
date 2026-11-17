# Wędzarnia

Sterownik wędzarni na ESP32 i aplikacja na tablet z Androidem:

- 4 sondy mięsa i 1 sonda komory (NTC, np. Sonoff MPS01),
- regulacja temperatury komory grzałką 230 V przez przekaźnik SSR,
- automatyczny koniec: gdy mięso dojdzie do zadanej temperatury, ESP32 wyłącza grzałkę, a tablet daje alarm,
- podgląd na żywo, wykres z całego procesu (do 12 h), szacowany czas dojścia do celu.

Sterowanie odbywa się w ESP32. Tablet tylko wyświetla i zmienia ustawienia, więc wyłączony tablet albo zanik WiFi nie przerywają wędzenia.

- **Aplikacja na Androida:** [pobierz wedzarnia.apk](https://github.com/zooolw-sudo/wedzarnia/releases/latest/download/wedzarnia.apk) (buduje się sama po każdej zmianie w `main`, jak Pogoda Buk)
- **Program ESP32:** [`esp32/wedzarnia/wedzarnia.ino`](esp32/wedzarnia/wedzarnia.ino)

## Części

| Część | Ilość | Uwagi |
|---|---|---|
| ESP32 DevKit (WROOM-32, 38 pin) | 1 | przy słabym WiFi przy wędzarni: wersja WROOM-32U z anteną zewnętrzną |
| Sonda Sonoff MPS01 | 5 | sprzedawane po 2 szt. |
| Gniazdo jack do sondy | 5 | dopasuj średnicę do wtyku sondy |
| Rezystor 47 kΩ 1% | 5 | patrz „Dobór rezystora” |
| Kondensator 100 nF | 5 | |
| Tranzystor BC547 (albo 2N2222) | 1 | |
| Rezystory 1 kΩ i 10 kΩ | po 1 | sterownik SSR |
| Przekaźnik SSR-40DA + radiator | 1 | wejście 3–32 V DC, wyjście 24–380 V AC |
| Termik bezpieczeństwa NC, ok. 130 °C | 1 | na prąd grzałki, montowany w komorze |
| Wyłącznik nadprądowy B10 | 1 | dla grzałki do 2 kW |
| Zasilacz 230 V → 5 V, min. 1 A | 1 | np. HLK-5M05 albo ładowarka USB |

## Podłączenie

| Element | Pin ESP32 |
|---|---|
| S1 (mięso) | GPIO32 |
| S2 (mięso) | GPIO33 |
| S3 (mięso) | GPIO34 |
| S4 (mięso) | GPIO35 |
| K (komora) | GPIO36 (VP) |
| Sterownik SSR | GPIO26 |
| Zasilanie | VIN (5 V), GND |

**Kanał sondy (5×):** 3V3 → 47 kΩ → pin ESP32 → sonda → GND, kondensator 100 nF między pinem a GND.

**Sterownik SSR:** GPIO26 → 1 kΩ → baza BC547, 10 kΩ baza–GND (grzałka zostaje wyłączona przy starcie ESP32), emiter → GND, kolektor → SSR 4 (−), SSR 3 (+) → 5 V.

**230 V:** L → wyłącznik B10 → termik NC → SSR 1 → SSR 2 → grzałka → N. Zasilacz 5 V za wyłącznikiem, przed termikiem. Obudowa wędzarni do PE.

Termik jest konieczny. Uszkodzony SSR zwykle zwiera na stałe, a wtedy tylko termik wyłączy grzałkę.

### Dobór rezystora

Zmierz multimetrem rezystancję sondy w temperaturze pokojowej:

- ok. 100 kΩ → 47 kΩ (domyślnie),
- ok. 1 MΩ → 470 kΩ i zmień `R_FIX` w programie,
- kilka Ω → to termopara, ten układ się nie nada.

Zasada: rezystor ≈ połowa rezystancji sondy w 25 °C. Wtedy pomiar działa od zimnego mięsa z lodówki do ok. 130 °C w komorze.

## Program ESP32

1. Arduino IDE, pakiet płytek „esp32” (Espressif), płytka **ESP32 Dev Module**.
2. Otwórz `esp32/wedzarnia/wedzarnia.ino` i wgraj. Dodatkowe biblioteki nie są potrzebne.
3. Pierwsze uruchomienie: ESP32 tworzy sieć **Wedzarnia-ESP32** (hasło `wedzarnia`). Połącz się z nią telefonem, otwórz `192.168.4.1/wifi`, wybierz domowe WiFi i wpisz hasło.
4. Po restarcie sterownik jest pod adresem `http://wedzarnia.local` (albo IP z routera). Aplikacja znajdzie go sama.

Przycisk BOOT przytrzymany 5 s (poza wędzeniem) kasuje WiFi i znów uruchamia sieć konfiguracyjną. Gdy domowe WiFi zniknie na dłużej niż 30 s, ESP32 też włącza własną sieć, a tablet może się z nią połączyć bezpośrednio.

Dioda na płytce: świeci = grzałka włączona, miga = brak połączenia z WiFi.

## Kalibracja

W aplikacji: Ustawienia → Kalibracja sond.

1. Wszystkie sondy razem do wody z lodem, odczekaj 2 minuty → **Punkt 0 °C**.
2. Wszystkie sondy do wrzątku → **Punkt 100 °C**.

Każda sonda dostaje własną krzywą. Zamiast 0/100 °C możesz wpisać temperaturę z termometru wzorcowego.

## Jak działa regulacja

- **START:** grzałka włącza się, gdy komora spadnie poniżej zadanej − ½ histerezy, i wyłącza powyżej zadanej + ½ histerezy. Między włączeniami jest co najmniej 10 s przerwy.
- **Cel sondy:** sonda z celem „—” służy tylko do podglądu. Sonda podłączona w trakcie wędzenia też się liczy.
- **Koniec:** gdy wszystkie sondy z celem dojdą (albo dowolna, do wyboru w ustawieniach). Grzałka OFF, alarm na tablecie.
- **Zabezpieczenia:** przegrzanie komory (domyślnie 140 °C), brak odczytu komory przez 5 min, grzałka pracuje 20 min bez przerwy, a komora stoi daleko poniżej zadanej (sonda komory wypadła?), maksymalny czas (domyślnie 12 h). Zawieszony program restartuje watchdog. Bez odczytu komory grzałka jest zawsze wyłączona. Po restarcie ESP32 wędzenie nie wznawia się samo.

## API (dla ciekawych)

| Adres | Opis |
|---|---|
| `GET /api/state` | stan: temperatury, nastawy, grzałka, WiFi |
| `GET /api/history` | wykres: punkt co minutę (temperatury ×10, % pracy grzałki) |
| `POST /api/cmd?do=start\|stop\|ack` | start, stop, potwierdzenie końca/awarii |
| `POST /api/set?k=75&h=2&t1=68&…&end=0&maxmin=720&kmax=140` | nastawy |
| `POST /api/cal?t=100` | punkt kalibracji (wszystkie sondy) |
| `GET /` | prosty podgląd w przeglądarce |
| `GET /wifi` | ustawienia WiFi |
