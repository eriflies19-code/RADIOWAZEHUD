/*
  ============================================================
  ESP32-C3 INTERNET RADIO + WAZE HUD - V3.2.1 (WAVEFORM 64 bars)

  MAC DINH:  Internet radio (WiFi) + web control, BLE tat.
  GIU GPIO4 2 GIAY: dung radio, tat WiFi, bat BLE cho WazeMod.
  BLE KET NOI (WazeMod tren Android): chuyen sang Waze HUD.
  MAT BLE (sau HUD_EXIT_DELAY_MS) hoac giu GPIO4 2 giay lai: khoi dong lai ve radio.

  NUT:     GPIO4  -> NUT -> GND   radio: kenh tiep theo | HUD: tat/bat tieng bip
           GPIO10 -> NUT -> GND   radio: am luong
  I2S:     GPIO1 = LRC/WS, GPIO2 = BCLK, GPIO3 = DIN   (dung chung radio va HUD)
  OLED:    GPIO20 = SCL, GPIO21 = SDA                  (dung chung radio va HUD)

  THU VIEN: ESP8266Audio, Adafruit GFX, Adafruit SSD1306,
            U8g2, ArduinoJson (7.x), NimBLE-Arduino (2.x)
  BOARD:    ESP32C3 Dev Module, USB CDC On Boot = Enabled,
            Partition Scheme = Huge APP (3MB No OTA)

  TEN BAI HAT: doc tu ICY metadata cua dai (neu dai co gui), hien tren Serial + web.
  LOI BAI HAT: trang web tren DIEN THOAI tu tra tu lrclib.net theo ten bai
               (ESP32 khong tai loi => khong ton RAM). Can dien thoai co Internet.
 TAT BAO VE MAN HINH SAU 5 PHUT: RADIO TAT MAN OLED de tiet kiem dien.
  ============================================================
*/

// 1 = cho phep mo Waze HUD bang nut GPIO4,  0 = chi radio
#define ENABLE_HUD 1
// 1 = vao thang HUD voi du lieu gia de thu man hinh (khong radio, khong BLE)
#define DEMO_MODE  0

#if DEMO_MODE && !ENABLE_HUD
#error "DEMO_MODE can ENABLE_HUD = 1"
#endif

#include <esp_system.h>   // esp_reset_reason()
#include <WiFi.h>
#include <time.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#if ENABLE_HUD
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#endif

#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourceICYStream.h"   // doc ten bai hat (ICY metadata)
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"


// ============================================================
// CONFIG
// ============================================================

// Mat WiFi lien tuc bao lau thi mo WiFi cai dat "ESP32-RADIO"
#define WIFI_AP_FALLBACK_MS 30000UL

const char* TIME_ZONE = "WIB-7";

#define BUTTON_PIN       4
#define VOLUME_BUTTON_PIN 10

#define I2S_BCK_IO   2
#define I2S_WS_IO    1
#define I2S_DO_IO    3

#define I2C_SDA_PIN  21
#define I2C_SCL_PIN  20

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3C

// Buffer stream: ~22 KB (giu muc nay de con RAM cho BLE + Wi-Fi; tang buffer co the lam het heap)
#define AUDIO_BUFFER_SIZE   22528

// Volume: moi lan bam GPIO10 se tang 1 muc, den muc lon nhat thi quay ve muc nho nhat
const float VOLUME_LEVELS[] = { 0.20, 0.35, 0.50, 0.65, 0.80 };
const uint8_t VOLUME_LEVEL_COUNT = sizeof(VOLUME_LEVELS) / sizeof(VOLUME_LEVELS[0]);
uint8_t volumeLevel = 0;
#define AUDIO_GAIN VOLUME_LEVELS[volumeLevel]

#define BUTTON_DEBOUNCE_MS   60UL
#define HUD_HOLD_MS        2000UL  // Nhan giu GPIO4 2 giay de bat/tat BLE HUD
#define RADIO_RETRY_MS     3000UL
#define WIFI_RETRY_MS      5000UL
#define OLED_REFRESH_MS     250UL  // 4 fps: giam tranh CPU/I2C voi giai ma MP3
#define CHANNEL_MESSAGE_MS 1200UL

#if ENABLE_HUD
// Mat BLE bao lau thi thoat HUD (khoi dong lai de quay ve radio).
// Du dai de BLE chap chon / app Android khoi dong lai khong lam doi che do.
#define HUD_EXIT_DELAY_MS 8000UL

// Tieng bip HUD (khi vuot toc do)
#define BEEP_VOL        0.1f     // 0.0 - 1.0
#define HUD_SAMPLE_RATE 16000
#endif


// ============================================================
// STATIONS
// ============================================================

struct RadioStation
{
    const char* name;
    const char* title;
    const char* format;
    const char* url;
};

RadioStation stations[] =
{
    { "VIETNAM NATION", "Vietnamese Radio", "MP3 48k",  "http://c13.radioboss.fm:8127/stream" },
    //{ "TNT ATLANTA",    "Vietnamese Radio", "MP3 64K",  "https://cast4.servcast.net/proxy/longpham2/stream" },
    { "VIET RADIO 1560","news",             "MP3 109K", "http://71.4.56.20/3rdout/radio.hou.std2/icecast.audio" },
    { "VIET WASHINGTON","POP Radio",        "MP3 96K",  "http://varadio.serverroom.net:7968/" },
    { "Music Lake",     "Music Radio",      "MP3 64K",  "http://nap.casthost.net:8626/stream" },
    { "Groove Salad",        "Groove Salad",     "MP3 128K", "https://ice2.somafm.com/groovesalad-128-mp3" },
    { "FOLK MUSIC",        "FOLK MUSIC",       "MP3 128K", "https://ice5.somafm.com/folkfwd-128-mp3" },
    { "Drone zone",        "Drone zone",       "MP3 128K", "https://ice2.somafm.com/dronezone-128-mp3" },
    //{ "Darkzone",        "Darkzone",       "MP3 128K", "https://ice2.somafm.com/darkzone-128-mp3" },
    { "London soul",        "",       "MP3 128K", "http://londonsoulradio.out.airtime.pro:8000/londonsoulradio_a" },
    { "RADIO PARADISE", "Eclectic Music",   "MP3 32K",  "http://stream-dc1.radioparadise.com/mp3-32" },
    //{ "FIP ROCK", "rock",   "MP3 128K",  "http://icecast.radiofrance.fr/fiprock-midfi.mp3" },
    { "Black ROCK", "rock",   "MP3 128K",  "https://ice5.somafm.com/brfm-128-mp3" },
        //{ "Loveon9scafe",   "POP Radio",        "MP3 128K", "http://listen.57fm.com/lscafe" },
    { "skyrock",       "rock",                 "MP3 128K",  "http://icecast.skyrock.net/s/natio_mp3_128k" },
        //{ "Organic hiphop", "",                 "",         "http://organichiphop.filehostia.com:8504/stream" }
};

const uint8_t STATION_COUNT = sizeof(stations) / sizeof(stations[0]);

uint8_t currentStation = 0;

#if ENABLE_HUD
// Dat o day (truoc moi ham) de Arduino tu tao prototype hieu cac kieu nay.
struct Hud {
  int nav = 0, spd = 0, lim = 0, over = 0, trn = 0, dst = -1, exitN = 0;
  int avg = 0, avgL = 0, alr = 0, alrD = -1, alrV = 0;
} hud;

struct AlertIcon {
  const unsigned char* bmp;   // anh icon 24x24, nullptr neu khong co
  int maxM;                   // khoang cach toi da (met) de bat dau hien
  bool speedNum;              // true: ve bien toc do co so (alrV)
};
#endif
// ------------------------------------------------------------
// Kenh do nguoi dung them tren web (luu trong NVS, toi da 10 kenh)
// ------------------------------------------------------------
#define MAX_CUSTOM_STATIONS 10

struct CustomStation
{
    char name[24];
    char url[128];
};

CustomStation customStations[MAX_CUSTOM_STATIONS];
uint8_t       customCount = 0;
Preferences   customPrefs;

uint8_t totalStations()
{
    return STATION_COUNT + customCount;
}

const char* stName(uint8_t i)
{
    return i < STATION_COUNT ? stations[i].name : customStations[i - STATION_COUNT].name;
}

const char* stTitle(uint8_t i)
{
    return i < STATION_COUNT ? stations[i].title : "Custom station";
}

const char* stFormat(uint8_t i)
{
    return i < STATION_COUNT ? stations[i].format : "CUSTOM";
}

const char* stUrl(uint8_t i)
{
    return i < STATION_COUNT ? stations[i].url : customStations[i - STATION_COUNT].url;
}

void loadCustomStations()
{
    customPrefs.begin("custom", false);

    customCount = customPrefs.getUChar("cnt", 0);

    if (customCount > MAX_CUSTOM_STATIONS)
        customCount = 0;

    for (uint8_t i = 0; i < customCount; i++)
    {
        String n = customPrefs.getString((String("n") + i).c_str(), "");
        String u = customPrefs.getString((String("u") + i).c_str(), "");

        strncpy(customStations[i].name, n.c_str(), sizeof(customStations[i].name) - 1);
        customStations[i].name[sizeof(customStations[i].name) - 1] = '\0';
        strncpy(customStations[i].url, u.c_str(), sizeof(customStations[i].url) - 1);
        customStations[i].url[sizeof(customStations[i].url) - 1] = '\0';
    }

    Serial.printf("CUSTOM STATIONS: %u\n", customCount);
}

void saveCustomStations()
{
    customPrefs.putUChar("cnt", customCount);

    for (uint8_t i = 0; i < MAX_CUSTOM_STATIONS; i++)
    {
        String kn = String("n") + i;
        String ku = String("u") + i;

        if (i < customCount)
        {
            customPrefs.putString(kn.c_str(), customStations[i].name);
            customPrefs.putString(ku.c_str(), customStations[i].url);
        }
        else
        {
            customPrefs.remove(kn.c_str());
            customPrefs.remove(ku.c_str());
        }
    }
}


// ============================================================
// LAST STATION MEMORY
// ============================================================

Preferences radioPrefs;
WebServer server(80);
Preferences wifiPrefs;
// Wi-Fi được lưu trong NVS, nên không mất khi tắt nguồn.
bool wifiSetupMode = false;
WebServer setupServer(80);
DNSServer dnsServer;                  // captive portal: dien thoai tu mo trang cai dat

bool          normalServerStarted = false;
unsigned long wifiLostSince       = 0;   // 0 = dang co WiFi
unsigned long setupLastCheck      = 0;
unsigned long setupLastRetry      = 0;

String savedSSID;
String savedPASS;

void startWiFiSetupAP();
bool connectSavedWiFi();
void startNormalWebServer();
String jsonEsc(const String& s);


const char* PREF_NAMESPACE = "radio";
const char* PREF_STATION   = "station";


// Dem so sample da phat ra loa: dung de phat hien dai chet / treo
volatile uint32_t audioSampleCount = 0;


// ============================================================
// OBJECTS
// ============================================================

Adafruit_SSD1306 display(
    SCREEN_WIDTH,
    SCREEN_HEIGHT,
    &Wire,
    OLED_RESET
);

// Waveform: 42 cot x 3 pixel (cot rong 2px + 1px cach) = 126px, moi cot = 32 sample
#define WAVE_BARS            64
#define WAVE_BAR_PITCH       2
#define WAVE_SAMPLES_PER_BAR 32

uint16_t waveBarIndex    = 0;
uint16_t waveSampleInBar = 0;
int32_t  wavePeak        = 0;
uint8_t  waveBars[WAVE_BARS];

class RadioOutputI2S : public AudioOutputI2S
{
public:
    bool ConsumeSample(int16_t sample[2]) override
    {
        audioSampleCount++;

        // Peak amplitude (mono) truoc khi vao I2S
        int32_t mono  = ((int32_t)sample[0] + (int32_t)sample[1]) / 2;
        int32_t level = mono < 0 ? -mono : mono;

        if (level > wavePeak)
            wavePeak = level;

        if (++waveSampleInBar >= WAVE_SAMPLES_PER_BAR)
        {
            int value = (wavePeak * 31L) / 32767L;

            if (value > 31)
                value = 31;

            waveBars[waveBarIndex] = (uint8_t)value;

            if (++waveBarIndex >= WAVE_BARS)
                waveBarIndex = 0;

            waveSampleInBar = 0;
            wavePeak        = 0;
        }

        return AudioOutputI2S::ConsumeSample(sample);
    }
};

RadioOutputI2S*            out  = nullptr;
AudioGeneratorMP3*         mp3  = nullptr;
AudioFileSourceICYStream*  file = nullptr;
AudioFileSourceBuffer*     buff = nullptr;


// ============================================================
// SONG TITLE (ICY metadata)
// ============================================================

char     songTitle[128] = "";
uint16_t songVersion    = 0;      // tang moi khi ten bai doi (web dung de biet khi nao cap nhat)

// Duoc goi tu ben trong luc doc stream: chi copy nhanh, khong lam gi nang.
void onStreamMetadata(void* cbData, const char* type, bool isUnicode, const char* str)
{
    (void)cbData;
    (void)isUnicode;

    if (type == nullptr || str == nullptr || strcmp(type, "StreamTitle") != 0)
        return;

    strncpy(songTitle, str, sizeof(songTitle) - 1);
    songTitle[sizeof(songTitle) - 1] = '\0';
    songVersion++;

    Serial.printf("SONG: %s\n", songTitle);
}


// ============================================================
// STATE
// ============================================================

bool oledReady    = false;
bool radioStarted = false;
bool networkTimeConfigured = false;

// Nut
bool          buttonStableState = HIGH;
bool          buttonLastReading = HIGH;
unsigned long buttonChangeTime  = 0;

bool          volumeButtonStableState = HIGH;
bool          volumeButtonLastReading = HIGH;
unsigned long volumeButtonChangeTime  = 0;

// Nhan nut chi dat yeu cau, loop se mo stream
bool channelChangeRequested = false;
bool selectedStationNeedsSave = false;

// Nhan giu GPIO4: dung chung (ENABLE_HUD 0 van can de bam ngan hoat dong)
unsigned long buttonPressStarted = 0;
bool buttonLongPressHandled = false;

#if ENABLE_HUD
bool bleInitialized = false;      // BLE chi khoi tao lan dau khi nguoi dung yeu cau
bool bleEnabled = false;           // BLE dang quang ba, san sang ket noi Waze
void setHudAvailability(bool enable);
#endif

// Reconnect
unsigned long lastRadioRetry = 0;

// Watchdog dai: dai chet (khong co du lieu / khong phai MP3) se tu dong dung
// va thu lai voi thoi gian cho tang dan, khong lam treo nut / web
#define RADIO_STALL_MS      10000UL
#define RADIO_RETRY_MAX_MS  30000UL
unsigned long lastProgressMs  = 0;
uint32_t      lastSampleSeen  = 0;
uint8_t       radioFailCount  = 0;
unsigned long radioRetryDelay = RADIO_RETRY_MS;

void noteRadioFailure()
{
    if (radioFailCount < 250)
        radioFailCount++;

    // 3s, 6s, 12s, 24s, 30s...
    unsigned long d = RADIO_RETRY_MS;

    for (uint8_t i = 1; i < radioFailCount && d < RADIO_RETRY_MAX_MS; i++)
        d *= 2;

    if (d > RADIO_RETRY_MAX_MS)
        d = RADIO_RETRY_MAX_MS;

    radioRetryDelay = d;
    lastRadioRetry  = millis();

    Serial.printf("RADIO FAIL #%u -> thu lai sau %lu ms\n",
                  radioFailCount, radioRetryDelay);
}

void noteRadioOK()
{
    radioFailCount  = 0;
    radioRetryDelay = RADIO_RETRY_MS;
}
unsigned long lastWiFiRetry  = 0;

// OLED
bool          showingChannelMessage = false;
unsigned long channelMessageTime    = 0;
unsigned long lastOLEDUpdate        = 0;
// Tat man hinh radio khi khong bam nut 5 phut (HUD luon sang)
#define OLED_SLEEP_MS 300000UL
bool          oledSleeping   = false;
unsigned long lastActivityMs = 0;
bool          buttonWokeScreen = false;

void oledWake()
{
    lastActivityMs = millis();

    if (oledSleeping && oledReady)
    {
        display.ssd1306_command(SSD1306_DISPLAYON);
        oledSleeping   = false;
        lastOLEDUpdate = 0;
    }
}

// Goi khi co thao tac nut: tra ve true neu man hinh dang tat (luc do chi danh thuc)
bool oledTouch()
{
    bool wasSleeping = oledSleeping;
    oledWake();
    return wasSleeping;
}

int           cachedRSSI             = -99;
unsigned long lastRSSIUpdate         = 0;


// ============================================================
// OLED HELPERS
// ============================================================

void oledClear()
{
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
}

void showStatus(const char* line1, const char* line2 = "")
{
    if (!oledReady)
        return;

    oledClear();

    display.setCursor(0, 20);
    display.print(line1);

    display.setCursor(0, 36);
    display.print(line2);

    display.display();
}

void showSetupScreen()
{
    if (!oledReady)
        return;

    oledClear();

    display.setCursor(0, 0);
    display.print("WIFI SETUP");

    display.setCursor(0, 16);
    display.print("Join WiFi:");

    display.setCursor(0, 26);
    display.print("ESP32-RADIO");

    display.setCursor(0, 44);
    display.print("Open 192.168.4.1");

    display.display();
}

void drawMusicIcon(int x, int y)
{
    display.fillRect(x + 9, y, 2, 12, SSD1306_WHITE);
    display.fillRect(x + 10, y, 7, 2, SSD1306_WHITE);
    display.fillCircle(x + 5, y + 12, 4, SSD1306_WHITE);
}

void drawSignalBars(
    int x,
    int y
)
{
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        return;
    }


    int rssi = cachedRSSI;


    int bars = 0;


    if (rssi >= -55)
        bars = 4;

    else if (rssi >= -65)
        bars = 3;

    else if (rssi >= -75)
        bars = 2;

    else if (rssi >= -85)
        bars = 1;

    else
        bars = 0;


    for (
        int i = 0;
        i < 4;
        i++
    )
    {
        int h =
            3 + i * 3;

        int bx =
            x + i * 5;

        int by =
            y + 12 - h;


        if (i < bars)
        {
            display.fillRect(
                bx,
                by,
                3,
                h,
                SSD1306_WHITE
            );
        }
        // Khong ve gi neu i >= bars - chi hien dung so vach tin hieu that
    }
}

bool isPlaying()
{
    return radioStarted && mp3 != nullptr && mp3->isRunning();
}

// Doi UTF-8 (tieng Viet co dau...) sang ASCII de font OLED mac dinh hien duoc
static const char FOLD_A[] = "AAAAAA CEEEEIIII NOOOOO  UUUUY  aaaaaa ceeeeiiii nooooo  uuuuy yAaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHh  IiIiIiIiI   JjKk LlLlLl    NnNnNn   OoOoOo  RrRrRrSsSsSsSsTtTt  UuUuUuUuUuUuWwYyYZzZzZz                                 Oo             Uu";  // U+00C0..U+01B0
static const char FOLD_V[] = "AaAaAaAaAaAaAaAaAaAaAaAaEeEeEeEeEeEeEeEeIiIiOoOoOoOoOoOoOoOoOoOoOoOoUuUuUuUuUuUuUuYyYyYyYy      ";  // U+1EA0..U+1EFF

void asciiFold(const char* in, char* outBuf, size_t outSize)
{
    size_t o = 0;

    while (*in && o + 1 < outSize)
    {
        uint8_t c = (uint8_t)*in++;
        uint32_t cp = c;

        if (c >= 0xC0 && c < 0xE0 && *in)            // 2 byte
            cp = ((c & 0x1F) << 6) | ((uint8_t)*in++ & 0x3F);
        else if (c >= 0xE0 && c < 0xF0 && in[0] && in[1])   // 3 byte
        {
            cp = ((c & 0x0F) << 12) | (((uint8_t)in[0] & 0x3F) << 6) | ((uint8_t)in[1] & 0x3F);
            in += 2;
        }
        else if (c >= 0xF0)                          // 4 byte: bo qua
        {
            while (*in && ((uint8_t)*in & 0xC0) == 0x80) in++;
            continue;
        }

        char r;

        if (cp < 0x80)                                   r = (char)cp;
        else if (cp == 0x110)                            r = 'D';
        else if (cp == 0x111)                            r = 'd';
        else if (cp >= 0xC0 && cp <= 0x1B0)              r = FOLD_A[cp - 0xC0];
        else if (cp >= 0x1EA0 && cp <= 0x1EFF)          r = FOLD_V[cp - 0x1EA0];
        else                                             r = ' ';

        if (r < 32 || r > 126) r = ' ';
        outBuf[o++] = r;
    }

    outBuf[o] = '\0';
}

void drawWaveform()
{
    // Doi xung quanh y=48, cao toi da +-5 px (y=43..53): duoi duong ke y=41, tren footer y=56
    for (int i = 0; i < WAVE_BARS; i++)
    {
        int half = map(waveBars[i], 0, 31, 1, 5);

        display.fillRect(i * WAVE_BAR_PITCH, 48 - half, 1, half * 2 + 1, SSD1306_WHITE);
    }
}

void drawMainPage()
{
    oledClear();

    // Header
    drawMusicIcon(1, 1);

    display.setCursor(24, 3);
    display.print("RADIO");

    time_t now = time(nullptr);
    display.setCursor(72, 3);
    if (now > 1700000000) {
        struct tm localTime;
        localtime_r(&now, &localTime);
        char clockText[6];
        strftime(clockText, sizeof(clockText), "%H:%M", &localTime);
        display.print(clockText);
    } else {
        display.print("--:--");
    }

    drawSignalBars(108, 1);

    // Ten bai hat (chay ngang neu dai); dai khong gui ten bai thi hien ten dai
    {
        char line[130];

        if (songTitle[0] != '\0')
            asciiFold(songTitle, line, sizeof(line));
        else
            asciiFold(stName(currentStation), line, sizeof(line));

        const int maxChars = 21;          // 128px / 6px
        size_t len = strlen(line);

        display.setCursor(0, 20);

        if (len <= maxChars)
        {
            display.print(line);
        }
        else
        {
            static uint16_t scrollVer   = 0xFFFF;
            static unsigned long scrollStart = 0;

            // Doi bai: chay lai tu dau
            if (scrollVer != songVersion)
            {
                scrollVer   = songVersion;
                scrollStart = millis();
            }

            size_t total = len + 3;
            size_t pos = ((millis() - scrollStart) / 250) % total;   // 4 ky tu/giay

            for (int i = 0; i < maxChars; i++)
            {
                size_t index = (pos + i) % total;
                display.write(index < len ? line[index] : ' ');
            }
        }
    }

    // Trang thai + format + RSSI
    display.setCursor(0, 28);
    display.print(isPlaying() ? "PLAY " : "WAIT ");
    display.print(stFormat(currentStation));

    if (WiFi.status() == WL_CONNECTED)
    {
        display.setCursor(92, 28);
        display.print(cachedRSSI);
        display.print("dB");
    }

    //display.drawLine(0, 41, 127, 41, SSD1306_WHITE); dong 2
    drawWaveform();

    // Footer
    display.setCursor(0, 56);
    display.print("CH ");
    display.print(currentStation + 1);
    display.print("/");
    display.print(totalStations());

    // Volume: chi hien so luong dau cham tuong ung muc volume
    display.setCursor(90, 56);
    for (int i = 0; i <= volumeLevel; i++)
    {
        display.print(".");
    }

    display.display();
}

void updateOLED()
{
    if (!oledReady)
        return;

    unsigned long now = millis();
        if (oledSleeping)
        return;

    if (now - lastActivityMs >= OLED_SLEEP_MS)
    {
        display.ssd1306_command(SSD1306_DISPLAYOFF);
        oledSleeping = true;
        Serial.println("OLED OFF");
        return;
    }

    // RSSI la thao tac driver Wi-Fi; chi cap nhat moi 5 giay, khong goi moi frame OLED.
    if (WiFi.status() == WL_CONNECTED && now - lastRSSIUpdate >= 5000UL) {
        cachedRSSI = WiFi.RSSI();
        lastRSSIUpdate = now;
    }

    if (showingChannelMessage)
    {
        if (now - channelMessageTime < CHANNEL_MESSAGE_MS)
            return;

        showingChannelMessage = false;
    }

    if (now - lastOLEDUpdate < OLED_REFRESH_MS)
        return;

    lastOLEDUpdate = now;

    if (WiFi.status() != WL_CONNECTED)
    {
        cachedRSSI = -99;
        showStatus("WIFI ERROR", "RECONNECTING...");
        return;
    }

    drawMainPage();
}


// ============================================================
// STREAM
// ============================================================

void stopStream()
{
    if (mp3 != nullptr && mp3->isRunning())
        mp3->stop();

    if (buff != nullptr)
    {
        delete buff;
        buff = nullptr;
    }

    if (file != nullptr)
    {
        delete file;
        file = nullptr;
    }

    radioStarted = false;
}

// Log trang thai buffer: "Buffer underflow" = mang cham hon toc do phat,
// buffer can nap lai het (nghe thay giat). Dung de kiem tra luc moi mo kenh.
void onBufferStatus(void* cbData, int code, const char* msg)
{
    (void)cbData;
    Serial.printf("BUF [%lu ms] %d: %s\n", millis(), code, msg);
}

bool startStream()
{
    if (WiFi.status() != WL_CONNECTED || out == nullptr)
        return false;

    uint32_t heapBefore = ESP.getFreeHeap();

    stopStream();

    Serial.printf("\n-- MO DAI: %s\n", stName(currentStation));

    file = new AudioFileSourceICYStream(stUrl(currentStation));

    if (file == nullptr)
    {
        Serial.println("ERROR: HTTP STREAM NULL");
        return false;
    }

    // Doi dai: xoa ten bai cu, cho dai gui ten bai moi
    songTitle[0] = '\0';
    songVersion++;
    file->RegisterMetadataCB(onStreamMetadata, nullptr);

    buff = new AudioFileSourceBuffer(file, AUDIO_BUFFER_SIZE);

    if (buff == nullptr)
    {
        Serial.println("ERROR: BUFFER NULL");
        stopStream();
        return false;
    }

    buff->RegisterStatusCB(onBufferStatus, nullptr);

    uint32_t heapAfterBuffer = ESP.getFreeHeap();

    if (mp3 == nullptr)
    {
        mp3 = new AudioGeneratorMP3();

        if (mp3 == nullptr)
        {
            Serial.println("ERROR: MP3 NULL");
            stopStream();
            return false;
        }
    }

    if (!mp3->begin(buff, out))
    {
        Serial.println("ERROR: MP3 BEGIN FAILED");
        stopStream();
        return false;
    }

    uint32_t heapAfterMP3 = ESP.getFreeHeap();

    Serial.printf(
        "HEAP: before=%u  buffer=%u  mp3=%u  (decoder dung %u bytes)\n",
        heapBefore, heapAfterBuffer, heapAfterMP3,
        heapAfterBuffer - heapAfterMP3
    );

    radioStarted   = true;
    lastOLEDUpdate = 0;

    // Bat dau dem thoi gian cho du lieu (khong tinh luc dang ket noi)
    lastSampleSeen = audioSampleCount;
    lastProgressMs = millis();

    return true;
}


// ============================================================
// BUTTON / CHANNEL
// ============================================================

void requestChannelChange()
{
    currentStation++;

    if (currentStation >= totalStations())
        currentStation = 0;

    selectedStationNeedsSave = true;
    channelChangeRequested = true;

    showingChannelMessage = true;
    channelMessageTime    = millis();

    Serial.printf("\nCHANNEL REQUEST -> %u/%u\n", currentStation + 1, totalStations());
}

void handleVolumeButton()
{
    bool reading = digitalRead(VOLUME_BUTTON_PIN);
    unsigned long now = millis();

    if (reading != volumeButtonLastReading)
    {
        volumeButtonChangeTime  = now;
        volumeButtonLastReading = reading;
    }

    if (now - volumeButtonChangeTime < BUTTON_DEBOUNCE_MS)
        return;

    if (reading != volumeButtonStableState)
    {
        volumeButtonStableState = reading;

        // LOW = vua nhan
        if (volumeButtonStableState == LOW)
        { 
            if (oledTouch())
                return;   // lan bam dau chi danh thuc man hinh
            volumeLevel++;

            if (volumeLevel >= VOLUME_LEVEL_COUNT)
                volumeLevel = 0;

            if (out != nullptr)
                out->SetGain(VOLUME_LEVELS[volumeLevel]);

            Serial.printf("VOLUME -> %u/%u (gain %.2f)\n",
                          volumeLevel + 1,
                          VOLUME_LEVEL_COUNT,
                          VOLUME_LEVELS[volumeLevel]);
        }
    }
}

void handleButton()
{
    bool reading = digitalRead(BUTTON_PIN);
    unsigned long now = millis();

    if (reading != buttonLastReading)
    {
        buttonChangeTime  = now;
        buttonLastReading = reading;
    }

    // Debounce: chi xu ly khi muc nut on dinh
    if (now - buttonChangeTime < BUTTON_DEBOUNCE_MS)
        return;

    if (reading != buttonStableState)
    {
        buttonStableState = reading;

        if (buttonStableState == LOW)
        {
            // Bat dau nhan: chua doi kenh ngay, cho xem co phai nhan giu 2 giay
            buttonPressStarted = now;
            buttonLongPressHandled = false;
            buttonWokeScreen = oledTouch();
        }
        else
        {
            // Tha nut truoc 2 giay = bam ngan, chuyen kenh
            // (khi dang cho BLE thi radio da tat: khong doi kenh am tham)
            bool radioActive = true;
#if ENABLE_HUD
            radioActive = !bleEnabled;
#endif
            radioActive = radioActive && !wifiSetupMode;
            if (!buttonLongPressHandled && radioActive && !buttonWokeScreen)
                requestChannelChange();
            buttonPressStarted = 0;
        }
    }

#if ENABLE_HUD
    // Nhan giu 2 giay: bat/tat quang ba BLE de mo/ dong kha nang ket noi Waze HUD.
    // Khong chuyen kenh khi thao tac nhan giu.
    if (buttonStableState == LOW && buttonPressStarted != 0 &&
        !buttonLongPressHandled && now - buttonPressStarted >= HUD_HOLD_MS)
    {
        buttonLongPressHandled = true;
        setHudAvailability(!bleEnabled);
    }
#endif
}

void saveSelectedStation()
{
    if (!selectedStationNeedsSave)
        return;

    selectedStationNeedsSave = false;
    if (radioPrefs.putUChar(PREF_STATION, currentStation) != sizeof(currentStation))
        Serial.println("ERROR: FAILED TO SAVE SELECTED STATION");
}

void handleChannelChange()
{
    if (!channelChangeRequested)
        return;

    // Xoa request truoc, neu loi thi handleRadio se tu retry
    channelChangeRequested = false;

    stopStream();
    saveSelectedStation();

    // Doi kenh: bo qua loi cua kenh cu
    noteRadioOK();

    showStatus("NEXT CHANNEL", stName(currentStation));

    if (WiFi.status() == WL_CONNECTED)
    {
        if (!startStream())
            noteRadioFailure();
    }
}


// ============================================================
// WIFI / RADIO
// ============================================================

void handleWiFi()
{
    unsigned long now = millis();

    if (WiFi.status() == WL_CONNECTED)
    {
        wifiLostSince = 0;
        if (!networkTimeConfigured) {
            configTzTime(TIME_ZONE, "pool.ntp.org", "time.nist.gov");
            networkTimeConfigured = true;
        }
        return;
    }

    // Vua mat WiFi
    if (wifiLostSince == 0)
    {
        wifiLostSince = now;
        stopStream();
    }

    // Mat WiFi qua lau -> mo WiFi cai dat de chon mang khac
    if (now - wifiLostSince >= WIFI_AP_FALLBACK_MS)
    {
        Serial.println("WIFI LOST TOO LONG -> START SETUP AP");

        stopStream();
        startWiFiSetupAP();
        return;
    }

    if (now - lastWiFiRetry < WIFI_RETRY_MS)
        return;

    lastWiFiRetry = now;

    Serial.println("WIFI LOST - RECONNECT");

    // Ket noi lai bang WiFi da luu (khong phai WiFi trong code)
    WiFi.disconnect();
    WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
}

void handleRadio()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    // mp3->loop() phai duoc goi lien tuc, khong delay
    if (isPlaying())
    {
        if (!mp3->loop())
        {
            Serial.println("RADIO STREAM STOPPED");

            stopStream();
            noteRadioFailure();
            return;
        }

        // Watchdog: co sample ra loa thi dai song, khong thi cho toi RADIO_STALL_MS
        unsigned long nowP = millis();
        uint32_t      sc   = audioSampleCount;

        if (sc != lastSampleSeen)
        {
            lastSampleSeen = sc;
            lastProgressMs = nowP;

            if (radioFailCount > 0)
                noteRadioOK();
        }
        else if (nowP - lastProgressMs >= RADIO_STALL_MS)
        {
            Serial.println("RADIO STALLED (khong co am thanh) -> DUNG, THU LAI SAU");

            stopStream();
            noteRadioFailure();
        }

        return;
    }

    unsigned long now = millis();

    if (now - lastRadioRetry < radioRetryDelay)
        return;

    lastRadioRetry = now;

    Serial.println("TRY RADIO AGAIN");

    if (!startStream())
        noteRadioFailure();
}



// ============================================================
// WEB CONTROL
// ============================================================

String htmlEscape(const char* s)
{
    String o;

    for (; *s; s++)
    {
        switch (*s)
        {
            case '&':  o += "&amp;";  break;
            case '<':  o += "&lt;";   break;
            case '>':  o += "&gt;";   break;
            case '"':  o += "&quot;"; break;
            case '\'': o += "&#39;";  break;
            default:   o += *s;
        }
    }

    return o;
}

String htmlPage()
{
    String html;
    html.reserve(9000);

    html += "<!doctype html><html lang='vi'><head>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'>";
    // (khong dung meta refresh: trang tu cap nhat bang JS o cuoi trang)
    html += "<meta name='theme-color' content='#0b0f14'>";
    html += "<title>ESP32 Internet Radio</title>";
    html += "<style>";
    html += "*{box-sizing:border-box}body{margin:0;background:#0b0f14;color:#eef2f7;font-family:Arial,Helvetica,sans-serif;padding:16px 12px;min-height:100vh}.wrap{max-width:520px;margin:auto}.card{background:#121820;border:1px solid #202a35;border-radius:22px;padding:18px;margin-bottom:14px;box-shadow:0 12px 35px rgba(0,0,0,.28)}";
    html += ".top{display:flex;justify-content:space-between;align-items:center}.brand{font-size:12px;letter-spacing:2px;color:#8d9aaa;font-weight:bold}.live{font-size:11px;padding:6px 9px;border-radius:20px;background:#153321;color:#6ee58a}.now{margin-top:18px}.label{font-size:11px;color:#7f8b99;letter-spacing:1.5px;text-transform:uppercase}.station{font-size:29px;font-weight:800;line-height:1.1;margin-top:6px}.subtitle{font-size:14px;color:#aeb8c5;margin-top:7px}.format{display:inline-block;margin-top:12px;font-size:11px;color:#9aa7b5;border:1px solid #2a3541;border-radius:20px;padding:5px 9px}";
    html += ".controls{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:20px}.btn{display:flex;align-items:center;justify-content:center;min-height:54px;border:0;border-radius:16px;background:#202a35;color:#fff;text-decoration:none;font-size:14px;font-weight:700}.mainbtn{background:#e9edf2;color:#10151b}.btn:active{transform:scale(.97)}.volrow{display:flex;align-items:center;gap:10px;margin-top:14px}.volbtn{width:54px;min-width:54px;min-height:50px}.voltext{flex:1;text-align:center;color:#aeb8c5;font-size:11px;letter-spacing:1px}.dots{font-size:18px;letter-spacing:4px;color:#fff}.section{font-size:12px;color:#7f8b99;letter-spacing:1.4px;text-transform:uppercase;margin-bottom:10px}.stationlist{display:flex;flex-direction:column;gap:8px}.sbtn{display:flex;align-items:center;gap:12px;min-height:56px;padding:9px 12px;border:1px solid transparent;border-radius:15px;background:#171f28;color:#fff;text-decoration:none;text-align:left}.sbtn.active{background:#202d3a;border-color:#3b4b5c}.num{width:30px;height:30px;min-width:30px;border-radius:50%;background:#26313d;display:flex;align-items:center;justify-content:center;color:#aeb9c6;font-size:12px}.sname{font-size:14px;font-weight:700}.ssub{font-size:11px;color:#7f8b99;margin-top:3px}.info{display:flex;justify-content:space-between;margin-top:16px;font-size:10px;color:#687584}.song{margin-top:12px;font-size:14px;color:#6ee58a;font-weight:700;word-break:break-word}.lyrhead{font-size:12px;color:#7f8b99;letter-spacing:1.4px;text-transform:uppercase;margin-bottom:8px}.lyrsong{font-size:15px;font-weight:700;margin-bottom:10px;word-break:break-word}.lyrbox{max-height:340px;overflow:auto;white-space:pre-wrap;line-height:1.6;font-size:14px;color:#cfd7e2;-webkit-overflow-scrolling:touch}.lyrmsg{color:#7f8b99;font-size:13px}.footer{text-align:center;color:#566270;font-size:10px;padding:2px 0 14px;letter-spacing:.8px}.srow{display:flex;gap:6px;align-items:stretch}.srow .sbtn{flex:1}.del{display:flex;align-items:center;justify-content:center;width:44px;border-radius:15px;background:#2a1a1a;color:#ff8d8d;text-decoration:none;font-size:14px}.addf input{width:100%;padding:12px;margin-top:10px;border-radius:12px;border:1px solid #2a3541;background:#171f28;color:#fff;font-size:14px}.addf button{border:0;cursor:pointer}@media(max-width:360px){.station{font-size:25px}.controls{gap:6px}.btn{font-size:13px}}</style>";
    html += "</head><body><div class='wrap'>";
    html += "<div class='card'><div class='top'><div class='brand'>ESP32 INTERNET RADIO</div><div class='live'>● LIVE</div></div>";
    html += "<div class='now'><div class='label'>Đang phát</div><div class='station'>";
    html += htmlEscape(stName(currentStation));
    html += "</div><div class='subtitle'>";
    html += htmlEscape(stTitle(currentStation));
    html += "</div><div class='format'>";
    html += stFormat(currentStation);
    html += "</div>";

    if (songTitle[0] != '\0')
    {
        html += "<div class='song'>♪ ";
        html += htmlEscape(songTitle);
        html += "</div>";
    }

    html += "</div>";
    html += "<div class='controls'><a class='btn' href='/prev'>◀ PREV</a><a class='btn mainbtn' href='/next'>NEXT ▶</a></div>";
    html += "<div class='volrow'><a class='btn volbtn' href='/vol/down'>−</a><div class='voltext'>VOLUME<br><span class='dots'>";
    for (uint8_t i = 0; i < volumeLevel + 1; i++) html += ".";
    html += "</span></div><a class='btn volbtn' href='/vol/up'>+</a></div>";
    html += "<div class='info'><span>CH ";
    html += String(currentStation + 1); html += "/"; html += String(totalStations());
    html += "</span><span>"; html += (isPlaying() ? "PLAYING" : "WAITING"); html += "</span>";
    if (WiFi.status() == WL_CONNECTED) { html += "<span>RSSI "; html += String(WiFi.RSSI()); html += " dBm</span>"; }
    html += "</div></div>";
    html += "<div class='card'><div class='lyrhead'>Lời bài hát</div><div class='lyrsong' id='lyrSong'></div><div class='lyrbox' id='lyrBox'></div></div>";
    html += "<div class='card'><div class='section'>Stations</div><div class='stationlist'>";
    for (uint8_t i = 0; i < totalStations(); i++)
    {
        html += "<div class='srow'><a class='sbtn";
        if (i == currentStation) html += " active";
        html += "' href='/station?i="; html += String(i); html += "'><span class='num'>"; html += String(i + 1); html += "</span><span><div class='sname'>";
        html += htmlEscape(stName(i)); html += "</div><div class='ssub'>"; html += htmlEscape(stTitle(i)); html += " · "; html += htmlEscape(stFormat(i)); html += "</div></span></a>";
        if (i >= STATION_COUNT)
        {
            html += "<a class='del' href='/delstation?i="; html += String(i); html += "'>✕</a>";
        }
        html += "</div>";
    }
    html += "</div></div><div class='footer'>ESP32 • LOCAL WEB CONTROL</div></div>";

    // Form them kenh: nam NGOAI .wrap de khong bi thay noi dung khi trang tu cap nhat
    html += "<div style='max-width:520px;margin:-4px auto 24px;padding:0 0'><div class='card'><details><summary class='section' style='cursor:pointer;margin:0'>+ Thêm kênh</summary>";
    html += "<form class='addf' method='post' action='/addstation'>";
    html += "<input name='name' maxlength='23' placeholder='Tên kênh (tối đa 23 ký tự)'>";
    html += "<input name='url' type='url' maxlength='127' placeholder='http://... (link stream MP3)' required>";
    html += "<button type='submit' class='btn mainbtn' style='width:100%;margin-top:10px'>THÊM &amp; PHÁT THỬ</button>";
    html += "<div class='lyrmsg' style='margin-top:8px'>Nên dùng link http:// (link https:// thường không phát được). Tối đa ";
    html += String(MAX_CUSTOM_STATIONS);
    html += " kênh thêm; bấm ✕ để xóa.</div></form></details></div></div>";

    // Cap nhat khong tai lai trang: poll /status nho, doi thi moi thay noi dung .wrap
    html += R"JS(<script>
var last='',cur={v:-1,t:''},lyr={state:'none',text:''},cache={};
function renderLyr(){
 var s=document.getElementById('lyrSong'),b=document.getElementById('lyrBox');
 if(!s||!b)return;
 s.textContent=cur.t||'';
 b.textContent='';
 function m(t){var e=document.createElement('span');e.className='lyrmsg';e.textContent=t;b.appendChild(e)}
 if(!cur.t){m('Đài này chưa gửi tên bài hát.');return}
 if(lyr.state==='load'){m('Đang tìm lời…');return}
 if(lyr.state==='ok'){b.textContent=lyr.text;return}
 if(lyr.state==='err'){m('Không tải được lời (điện thoại cần có Internet).');return}
 m('Không tìm thấy lời bài hát này.');
}
function parseTitle(t){
 t=t.replace(/[\[\(].*?[\]\)]/g,' ').replace(/\.(mp3|aac)$/i,'').replace(/^\s*\d{1,3}\s*[\.\-]\s+/,'').replace(/\s+/g,' ').trim();
 var i=t.indexOf(' - ');
 if(i>0)return{a:t.slice(0,i).trim(),n:t.slice(i+3).trim()};
 return{a:'',n:t};
}
function stripLrc(s){return s?s.replace(/^\s*\[[^\]]*\]\s*/gm,'').trim():''}
function norm(s){return (s||'').normalize('NFD').replace(/[̀-ͯ]/g,'').replace(/đ/g,'d').replace(/Đ/g,'D').toLowerCase().replace(/[^a-z0-9 ]/g,' ').replace(/\s+/g,' ').trim()}
function enc(x){return encodeURIComponent(x)}
async function lrc(qs,want){
 var r=await fetch('https://lrclib.net/api/search?'+qs);
 if(!r.ok)throw new Error('http');
 var arr=await r.json(),w=norm(want);
 for(var i=0;i<arr.length;i++){
  if(arr[i].instrumental)continue;
  var tn=norm(arr[i].trackName||arr[i].name);
  if(w&&tn&&tn.indexOf(w)<0&&w.indexOf(tn)<0)continue;
  var t=arr[i].plainLyrics||stripLrc(arr[i].syncedLyrics);
  if(t)return t;
 }
 return '';
}
async function ovh(a,n){
 var r=await fetch('https://api.lyrics.ovh/v1/'+enc(a)+'/'+enc(n));
 if(!r.ok)return '';
 var j=await r.json();
 return (j.lyrics||'').trim();
}
async function loadLyrics(title){
 if(!title)return;
 if(cache[title]){lyr=cache[title];renderLyr();return}
 lyr={state:'load',text:''};renderLyr();
 var p=parseTitle(title),n=p.n,a=p.a,jobs=[];
 var full=a?(a+' '+n):n;
 jobs.push(function(){return lrc('q='+enc(full),n)});
 if(a){
  jobs.push(function(){return lrc('track_name='+enc(n)+'&artist_name='+enc(a),n)});
  // nhieu dai ghi nguoc: "Ten bai - Ca si"
  jobs.push(function(){return lrc('track_name='+enc(a)+'&artist_name='+enc(n),a)});
 }
 jobs.push(function(){return lrc('q='+enc(n),n)});
 if(a)jobs.push(function(){return lrc('q='+enc(a),a)});
 if(norm(full)!==full.toLowerCase())jobs.push(function(){return lrc('q='+enc(norm(full)),norm(n))});
 if(a){
  jobs.push(function(){return ovh(a,n)});
  jobs.push(function(){return ovh(n,a)});
 }
 var text='',okNet=false;
 for(var k=0;k<jobs.length&&!text;k++){
  try{text=await jobs[k]();okNet=true}catch(e){}
  if(cur.t!==title)return;
 }
 var res=text?{state:'ok',text:text}:{state:okNet?'none':'err',text:''};
 if(res.state!=='err')cache[title]=res;
 if(cur.t===title){lyr=res;renderLyr()}
}
async function pollSong(){
 try{
  var j=await (await fetch('/song')).json();
  if(j.v!==cur.v){cur=j;lyr={state:'none',text:''};renderLyr();loadLyrics(j.t)}
 }catch(e){}
}
async function swapPage(){
 try{
  var t=await (await fetch('/')).text();
  var d=new DOMParser().parseFromString(t,'text/html');
  var w=d.querySelector('.wrap');
  if(w){
   var ob=document.getElementById('lyrBox'),sc=ob?ob.scrollTop:0;
   document.querySelector('.wrap').innerHTML=w.innerHTML;
   renderLyr();
   var nb=document.getElementById('lyrBox');if(nb)nb.scrollTop=sc;
  }
 }catch(e){}
}
async function poll(force){
 try{
  var s=await (await fetch('/status')).text();
  if(force||(last!==''&&s!==last))await swapPage();
  last=s;
 }catch(e){}
}
document.addEventListener('click',function(e){
 var a=e.target.closest('a');
 if(!a)return;
 var h=a.getAttribute('href');
 if(!h||h.charAt(0)!=='/')return;
 e.preventDefault();
 if(h.indexOf('/delstation')===0&&!confirm('Xóa kênh này?'))return;
 fetch(h+(h.indexOf('?')<0?'?':'&')+'ajax=1').then(function(){poll(true);pollSong()}).catch(function(){});
});
renderLyr();
poll(false);
pollSong();
setInterval(function(){poll(false)},3000);
setInterval(pollSong,3000);
</script>)JS";

    html += "</body></html>";
    return html;
}

void webRedirect()
{
    // Yeu cau tu JS (ajax=1): tra loi gon, khong chuyen trang
    if (server.hasArg("ajax"))
    {
        server.send(204, "text/plain", "");
        return;
    }

    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "OK");
}

void webNext()
{
    requestChannelChange();
    webRedirect();
}

void webPrev()
{
    if (currentStation == 0)
        currentStation = totalStations() - 1;
    else
        currentStation--;

    selectedStationNeedsSave = true;
    channelChangeRequested = true;
    showingChannelMessage = true;
    channelMessageTime = millis();

    Serial.printf("\nWEB CHANNEL REQUEST -> %u/%u\n",
                  currentStation + 1, totalStations());

    webRedirect();
}

void webStation()
{
    if (!server.hasArg("i"))
    {
        webRedirect();
        return;
    }

    int index = server.arg("i").toInt();

    if (index < 0 || index >= totalStations())
    {
        server.send(400, "text/plain", "Invalid station");
        return;
    }

    currentStation = (uint8_t)index;

    selectedStationNeedsSave = true;
    channelChangeRequested = true;
    showingChannelMessage = true;
    channelMessageTime = millis();

    Serial.printf("\nWEB STATION -> %u/%u: %s\n",
                  currentStation + 1,
                  totalStations(),
                  stName(currentStation));

    webRedirect();
}

void webVolumeUp()
{
    volumeLevel++;

    if (volumeLevel >= VOLUME_LEVEL_COUNT)
        volumeLevel = 0;

    if (out != nullptr)
        out->SetGain(VOLUME_LEVELS[volumeLevel]);

    Serial.printf("WEB VOLUME -> %u/%u (gain %.2f)\n",
                  volumeLevel + 1,
                  VOLUME_LEVEL_COUNT,
                  VOLUME_LEVELS[volumeLevel]);

    webRedirect();
}

void webAddStation()
{
    String n = server.arg("name");
    String u = server.arg("url");
    n.trim();
    u.trim();

    if (!(u.startsWith("http://") || u.startsWith("https://")) || u.length() >= sizeof(customStations[0].url))
    {
        server.send(400, "text/plain; charset=utf-8", "Link khong hop le (phai bat dau bang http:// va < 128 ky tu)");
        return;
    }

    if (customCount >= MAX_CUSTOM_STATIONS)
    {
        server.send(400, "text/plain; charset=utf-8", "Da du so kenh toi da, hay xoa bot kenh");
        return;
    }

    if (n.length() == 0)
        n = "Kenh moi";

    CustomStation& c = customStations[customCount];

    strncpy(c.name, n.c_str(), sizeof(c.name) - 1);
    c.name[sizeof(c.name) - 1] = '\0';
    strncpy(c.url, u.c_str(), sizeof(c.url) - 1);
    c.url[sizeof(c.url) - 1] = '\0';

    customCount++;
    saveCustomStations();

    // Chuyen sang kenh moi de phat thu
    currentStation = totalStations() - 1;
    selectedStationNeedsSave = true;
    channelChangeRequested = true;
    showingChannelMessage  = true;
    channelMessageTime     = millis();

    Serial.printf("ADD STATION: %s -> %s\n", c.name, c.url);

    server.sendHeader("Location", "/");
    server.send(303, "text/plain", "OK");
}

void webDelStation()
{
    int i = server.hasArg("i") ? server.arg("i").toInt() : -1;

    if (i < (int)STATION_COUNT || i >= (int)totalStations())
    {
        server.send(400, "text/plain", "Invalid station");
        return;
    }

    for (uint8_t k = i - STATION_COUNT; k + 1 < customCount; k++)
        customStations[k] = customStations[k + 1];

    customCount--;
    saveCustomStations();

    if (currentStation == i)
    {
        // Dang nghe dung kenh bi xoa -> ve kenh 1
        currentStation = 0;
        channelChangeRequested = true;
        showingChannelMessage  = true;
        channelMessageTime     = millis();
    }
    else if (currentStation > i)
    {
        currentStation--;
    }

    selectedStationNeedsSave = true;

    Serial.printf("DEL STATION %d\n", i + 1);

    webRedirect();
}

void webVolumeDown()
{
    if (volumeLevel == 0)
        volumeLevel = VOLUME_LEVEL_COUNT - 1;
    else
        volumeLevel--;

    if (out != nullptr)
        out->SetGain(VOLUME_LEVELS[volumeLevel]);

    Serial.printf("WEB VOLUME -> %u/%u (gain %.2f)\n",
                  volumeLevel + 1,
                  VOLUME_LEVEL_COUNT,
                  VOLUME_LEVELS[volumeLevel]);

    webRedirect();
}

void startWebServer()
{
    server.on("/", HTTP_GET, []()
    {
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/html; charset=utf-8", htmlPage());
    });

    server.on("/next", HTTP_GET, webNext);
    server.on("/prev", HTTP_GET, webPrev);
    server.on("/station", HTTP_GET, webStation);
    server.on("/addstation", HTTP_POST, webAddStation);
    server.on("/delstation", HTTP_GET, webDelStation);
    server.on("/vol/up", HTTP_GET, webVolumeUp);
    server.on("/vol/down", HTTP_GET, webVolumeDown);

    // Trang thai gon de JS biet khi nao can cap nhat giao dien
    server.on("/status", HTTP_GET, []()
    {
        String s = String(currentStation) + "," + String(volumeLevel) + "," +
                   (isPlaying() ? "1" : "0") + "," + String(WiFi.RSSI() / 6) + "," +
                   String(songVersion) + "," + String(totalStations());

        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "text/plain", s);
    });

    // Ten bai hat hien tai (JSON) cho trang web
    server.on("/song", HTTP_GET, []()
    {
        String j = "{\"v\":";
        j += String(songVersion);
        j += ",\"t\":\"";
        j += jsonEsc(songTitle);
        j += "\"}";

        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json; charset=utf-8", j);
    });

    server.onNotFound([]()
    {
        server.send(404, "text/plain", "Not found");
    });

    server.begin();

    Serial.println("WEB SERVER STARTED");
    Serial.print("WEB CONTROL: http://");
    Serial.println(WiFi.localIP());
}

// ============================================================
// WAZE HUD (BLE) - chi chay khi dien thoai ket noi BLE
// ============================================================
/*
  Radio la che do mac dinh, BLE TAT luc bat nguon.

  Giu GPIO4 2 giay -> setHudAvailability(true): dung radio, tat WiFi, bat BLE quang ba.
  Android (WazeMod) ket noi BLE -> enterHudMode(): hien HUD.
  Giu GPIO4 2 giay lan nua (luc cho dien thoai) hoac mat BLE qua HUD_EXIT_DELAY_MS
  -> khoi dong lai, tu vao lai radio.
*/

#if ENABLE_HUD

// UUID co dinh cua HLP (theo tai lieu WazeMod)
#define HLP_SERVICE_UUID "8a7e0001-4d6e-4c48-9a9d-484c504c0001"
#define HLP_TX_UUID      "8a7e0002-4d6e-4c48-9a9d-484c504c0001"  // Android -> HUD (write with response)
#define HLP_RX_UUID      "8a7e0003-4d6e-4c48-9a9d-484c504c0001"  // HUD -> Android (notify)

// Man hinh HUD dung U8g2 (radio dung Adafruit). Moi ben tu khoi tao lai OLED
// khi duoc dung toi nen khong xung dot. Neu man hinh bi lech/nhieu 2 pixel,
// doi sang SH1106:
// U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, U8X8_PIN_NONE, I2C_SCL_PIN, I2C_SDA_PIN);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, U8X8_PIN_NONE, I2C_SCL_PIN, I2C_SDA_PIN);

uint32_t lastRx = 0;
volatile bool soundOn = true;
volatile bool bleConnected = false;
volatile bool sendDevPending = false;
QueueHandle_t lineQ;
NimBLECharacteristic* rxChar = nullptr;   // notify -> Android

bool hudMode = false;                   // dang o che do HUD
volatile bool hudAudioReady = false;    // I2S da mo (16 kHz) cho tieng bip

bool ensureHudQueue() {
  if (lineQ != nullptr) return true;

  lineQ = xQueueCreate(4, 520);
  if (lineQ == nullptr) {
    Serial.println("ERROR: HUD COMMAND QUEUE ALLOCATION FAILED");
    return false;
  }
  return true;
}

// ---------- Gui du lieu len Android (notify, chia chunk 20 byte cho an toan voi MTU 23) ----------
void sendLine(const char* s) {
  if (!rxChar || !bleConnected) return;
  char frame[600];
  size_t len = snprintf(frame, sizeof(frame), "%s\n", s);
  for (size_t off = 0; off < len; off += 20) {
    size_t n = len - off > 20 ? 20 : len - off;
    rxChar->notify((const uint8_t*)(frame + off), n);
    delay(6);
  }
}

// Khai bao thiet bi (dev): chi xin cac field minh can
const char DEV_JSON[] =
  "{\"v\":1,\"t\":\"dev\",\"name\":\"WazeHUD-C3\",\"fw\":\"1.0.0\",\"proto\":[1],"
  "\"disp\":{\"w\":128,\"h\":64,\"color\":0},"
  "\"can\":[\"speed\",\"limit\",\"turn\",\"avgzone\",\"alerts\"],"
  "\"want\":{\"rate\":4,\"fields\":[\"nav\",\"spd\",\"lim\",\"over\",\"trn\",\"dst\","
  "\"exit\",\"avg\",\"avgL\",\"alr\",\"alrD\",\"alrV\"]}}";

// ---------- BLE ----------
class WriteCB : public NimBLECharacteristicCallbacks {
  char buf[520];
  size_t n = 0;
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    const char* d = (const char*)v.data();
    for (size_t i = 0; i < v.size(); i++) {
      char ch = d[i];
      if (ch == '\r') continue;
      if (ch == '\n') {
        buf[n] = 0;
        if (n) xQueueSend(lineQ, buf, 0);   // day sang loop() de parse
        n = 0;
      } else if (n < sizeof(buf) - 1) {
        buf[n++] = ch;
      } else {
        n = 0;                              // tran -> bo dong, dong bo lai o LF ke
      }
    }
  }
};

class RxCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t subValue) override {
    if (subValue & 1) sendDevPending = true;   // Android vua bat notify -> gui dev
  }
};

class SrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& i) override { bleConnected = true; }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& i, int reason) override {
    bleConnected = false;
    sendDevPending = false;
  }
};

bool startBle() {
  if (!ensureHudQueue()) return false;

  if (!bleInitialized) {
    NimBLEDevice::init("WazeHUD");
    NimBLEServer* bleServer = NimBLEDevice::createServer();
    bleServer->setCallbacks(new SrvCB());
    bleServer->advertiseOnDisconnect(true);

    NimBLEService* svc = bleServer->createService(HLP_SERVICE_UUID);
    NimBLECharacteristic* tx = svc->createCharacteristic(HLP_TX_UUID, NIMBLE_PROPERTY::WRITE);
    tx->setCallbacks(new WriteCB());
    rxChar = svc->createCharacteristic(HLP_RX_UUID, NIMBLE_PROPERTY::NOTIFY);
    rxChar->setCallbacks(new RxCB());
    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(HLP_SERVICE_UUID);
    adv->enableScanResponse(true);
    adv->setName("WazeHUD");
    // Quang ba cham de giam anh huong Wi-Fi/radio khi HUD duoc bat.
    adv->setMinInterval(800);    // 500 ms
    adv->setMaxInterval(1600);   // 1000 ms
    bleInitialized = true;
  }

  NimBLEDevice::getAdvertising()->start();
  bleEnabled = true;
  Serial.printf("HUD BLE ON - HEAP FREE: %u bytes\n", ESP.getFreeHeap());
  return true;
}

void setHudAvailability(bool enable) {
 oledWake();
  if (enable) {
    // Chuyen han sang che do cho HUD truoc khi khoi tao BLE.
    // Khoi tao NimBLE trong luc Wi-Fi dang stream MP3 co the lam treo vong lap.
    Serial.println("GPIO4 HOLD: preparing HUD BLE mode...");
    Serial.flush();

    stopStream();
    channelChangeRequested = false;

    if (wifiSetupMode) {
      dnsServer.stop();
      setupServer.stop();
      WiFi.softAPdisconnect(true);
      wifiSetupMode = false;
    }
    if (normalServerStarted) {
      server.stop();
      MDNS.end();
      normalServerStarted = false;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(150); // cho Wi-Fi driver giai phong radio truoc khi khoi tao NimBLE

    showStatus("STARTING BLE...", "PLEASE WAIT");
    if (!startBle()) {
      showStatus("HUD ERROR", "RESTARTING RADIO");
      Serial.println("HUD BLE START FAILED -> RESTART");
      Serial.flush();
      delay(150);
      ESP.restart();
      return;
    }
    showStatus("HUD BLE ON", "WAITING FOR PHONE");
    Serial.println("GPIO4 HOLD: HUD BLE ENABLED; waiting for WazeMod");
  } else {
    if (bleInitialized && !bleConnected) {
      NimBLEDevice::getAdvertising()->stop();
    }
    bleEnabled = false;
    showStatus("HUD BLE OFF", "RESTART RADIO...");
    Serial.println("GPIO4 HOLD: HUD BLE DISABLED; restarting radio mode");
    Serial.flush();
    delay(150);
    ESP.restart(); // phuc hoi Wi-Fi + radio sach, BLE tat mac dinh sau reboot
  }
}

// ---------- Am thanh: tieng bip qua chinh I2S cua radio ("out") ----------
// ESP32-C3 chi co 1 cong I2S va radio dang giu no, nen HUD khong tao driver rieng:
// luc vao HUD ta out->stop() roi mo lai 16 kHz / 16-bit / stereo, bip bang
// out->ConsumeSample().

struct Note { uint16_t f; uint16_t ms; };            // f = 0: im lang
const Note PAT_CAMERA[] = {{1800, 90}, {0, 70}, {1800, 90}};
const Note PAT_POLICE[] = {{1800, 90}, {0, 70}, {1800, 90}, {0, 70}, {1800, 90}};
const Note PAT_DANGER[] = {{700, 450}};
const Note PAT_OVER[]   = {{2100, 100}};
const Note PAT_TURN[]   = {{1320, 110}, {0, 30}, {1760, 150}};
const Note PAT_ON[]     = {{1000, 80}, {0, 40}, {1500, 80}};
struct Pattern { const Note* n; uint8_t len; };
#define NOTES(a) a, (uint8_t)(sizeof(a) / sizeof(a[0]))
enum { P_NONE = 0, P_CAMERA, P_POLICE, P_DANGER, P_OVER, P_TURN, P_ON };
const Pattern PATTERNS[] = {
  {nullptr, 0}, {NOTES(PAT_CAMERA)}, {NOTES(PAT_POLICE)}, {NOTES(PAT_DANGER)},
  {NOTES(PAT_OVER)}, {NOTES(PAT_TURN)}, {NOTES(PAT_ON)}
};

static int16_t sineTab[256];
QueueHandle_t soundQ;

// Tao tung mau sine roi day vao I2S. ConsumeSample ghi khong cho (timeout 0):
// DMA day thi nhuong 1 tick roi thu lai, bo cuoc sau ~100 ms.
static void playNote(uint16_t f, uint16_t ms) {
  static uint32_t phase = 0;
  const int total = (HUD_SAMPLE_RATE / 1000) * ms;
  const int ramp = HUD_SAMPLE_RATE / 200;            // 5 ms vao/ra de khong bi tach
  const uint32_t inc = f ? (uint32_t)(((uint64_t)f << 32) / HUD_SAMPLE_RATE) : 0;
  for (int pos = 0; pos < total; pos++) {
    int env = 256;
    if (pos < ramp) env = 256 * pos / ramp;
    else if (total - pos < ramp) env = 256 * (total - pos) / ramp;
    int16_t v = f ? (int16_t)((sineTab[phase >> 24] * env) >> 8) : 0;
    phase += inc;
    int16_t s[2] = { v, v };                         // trai, phai (MAX98357A tron L+R)
    int tries = 0;
    while (hudAudioReady && !out->ConsumeSample(s)) {
      if (++tries > 100) return;
      vTaskDelay(1);
    }
  }
}

static void audioTask(void*) {
  uint8_t id;
  for (;;) {
    if (xQueueReceive(soundQ, &id, portMAX_DELAY) == pdTRUE && id < sizeof(PATTERNS) / sizeof(PATTERNS[0])) {
      const Pattern& p = PATTERNS[id];
      for (uint8_t i = 0; i < p.len; i++) playNote(p.n[i].f, p.n[i].ms);
      playNote(0, 200);                              // xa im lang de DMA khong lap lai am cuoi
    }
  }
}

void playPattern(uint8_t id) {
  if (soundOn && hudAudioReady && soundQ) xQueueSend(soundQ, &id, 0);
}

static void hudAudioStart() {
  for (int i = 0; i < 256; i++) sineTab[i] = (int16_t)(sinf(2.0f * PI * i / 256.0f) * 32767.0f * BEEP_VOL);

  out->stop();                       // tra I2S cua radio (neu con giu)
  out->SetGain(1.0f);                // do to cua bip chinh bang BEEP_VOL
  out->SetRate(HUD_SAMPLE_RATE);     // ESP8266Audio luon xuat 16-bit
  out->SetChannels(2);
  hudAudioReady = out->begin();
  if (!hudAudioReady) Serial.println("HUD: I2S BEGIN FAILED (se khong co tieng bip)");

  soundQ = xQueueCreate(4, sizeof(uint8_t));
  xTaskCreate(audioTask, "hudaudio", 4096, NULL, 2, NULL);
}

// ---------- Parse HLP/1 ----------
void handleLine(const char* s) {
  JsonDocument doc;
  if (deserializeJson(doc, s)) return;
  if ((doc["v"] | 0) != 1) return;
  const char* t = doc["t"] | "";

  if (strcmp(t, "ping") == 0) {            // tra pong ngay
    sendLine("{\"v\":1,\"t\":\"pong\"}");
  } else if (strcmp(t, "hi") == 0) {       // phien moi: xoa state cu
    hud = Hud();
    lastRx = millis();
  } else if (strcmp(t, "bye") == 0) {
    lastRx = 0;
  } else if (strcmp(t, "s") == 0) {
    hud.nav   = doc["nav"]  | 0;
    hud.spd   = doc["spd"]  | 0;
    hud.lim   = doc["lim"]  | 0;
    hud.over  = doc["over"] | 0;
    hud.trn   = doc["trn"]  | 0;
    hud.dst   = doc["dst"]  | -1;
    hud.exitN = doc["exit"] | 0;
    hud.avg   = doc["avg"]  | 0;
    hud.avgL  = doc["avgL"] | 0;
    hud.alr   = doc["alr"]  | 0;
    hud.alrD  = doc["alrD"] | -1;
    hud.alrV  = doc["alrV"] | 0;
    lastRx = millis();
  }
}

// ---------- Ve ----------
void drawCentered(const char* s, int cx, int baseY) {
  u8g2.drawStr(cx - u8g2.getStrWidth(s) / 2, baseY, s);
}

void fmtDist(int m, char* out, size_t sz) {
  if (m < 1000) snprintf(out, sz, "%dm", m);
  else snprintf(out, sz, "%d.%dkm", m / 1000, (m % 1000) / 100);
}

// Icon canh bao 24x24 (XBM). Chi cac loai co icon moi duoc hien.
const unsigned char ICON_CAM[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0xfe,0xff,0x00,0xfe,0xff,0x01,0xfe,0xff,0x81,
  0xfe,0xff,0xe1,0x7e,0xf8,0xf9,0x3e,0xf0,0xfd,0x3e,0xf0,0xff,
  0x1e,0xf0,0xff,0x3e,0xf0,0xff,0x3e,0xf0,0xfd,0x7e,0xfc,0xf1,
  0xfe,0xff,0xe1,0xfe,0xff,0x01,0xfe,0xff,0x01,0xfc,0xbf,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
const unsigned char ICON_LIGHT[] U8X8_PROGMEM = {
  0xc0,0xff,0x03,0xc0,0xff,0x03,0xc0,0x00,0x03,0xc0,0x18,0x03,
  0xc0,0x3c,0x03,0xc0,0x7e,0x03,0xc0,0x7e,0x03,0xc0,0x3c,0x03,
  0xc0,0x18,0x03,0xc0,0x18,0x03,0xc0,0x3c,0x03,0xc0,0x66,0x03,
  0xc0,0x66,0x03,0xc0,0x3c,0x03,0xc0,0x18,0x03,0xc0,0x18,0x03,
  0xc0,0x3c,0x03,0xc0,0x66,0x03,0xc0,0x66,0x03,0xc0,0x3c,0x03,
  0xc0,0x18,0x03,0xc0,0x00,0x03,0xc0,0xff,0x03,0xc0,0xff,0x03,
};
const unsigned char ICON_POLICE[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x7e,0x00,0xc0,0xff,0x03,0xf8,0xff,0x1f,
  0xfe,0xff,0x7f,0xfe,0xff,0x7f,0xfe,0xff,0x7f,0xfe,0xe7,0x7f,
  0xfe,0xe7,0x7f,0x7e,0x00,0x7e,0xfe,0x00,0x7f,0xfe,0x81,0x7f,
  0xfe,0xc3,0x7f,0xfe,0x81,0x7f,0xfc,0xbd,0x3f,0xf8,0xff,0x1f,
  0xf0,0xff,0x0f,0xe0,0xff,0x07,0xc0,0xff,0x03,0x00,0xff,0x00,
  0x00,0x7e,0x00,0x00,0x3c,0x00,0x00,0x18,0x00,0x00,0x00,0x00,
};
const unsigned char ICON_HAZARD[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x18,0x00,
  0x00,0x3c,0x00,0x00,0x7c,0x00,0x00,0x66,0x00,0x00,0xe7,0x00,
  0x00,0xc3,0x00,0x80,0xdb,0x01,0x80,0x99,0x01,0xc0,0x99,0x03,
  0xc0,0x18,0x03,0xe0,0x18,0x07,0x60,0x18,0x06,0x70,0x18,0x0e,
  0x30,0x00,0x0c,0x18,0x18,0x18,0x1c,0x18,0x38,0x0c,0x18,0x30,
  0x0e,0x00,0x70,0xfe,0xff,0x7f,0xfe,0xff,0x7f,0x00,0x00,0x00,
};
const unsigned char ICON_CRASH[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x18,0x00,
  0x00,0x3c,0x00,0x00,0x7c,0x00,0x00,0x7e,0x00,0x00,0xff,0x00,
  0x00,0xff,0x00,0x80,0xff,0x01,0x80,0xff,0x01,0xc0,0xff,0x03,
  0xc0,0xff,0x03,0xe0,0x3c,0x07,0xe0,0x18,0x07,0xf0,0x81,0x0f,
  0xf0,0xc3,0x0f,0xf8,0xc3,0x1f,0xfc,0x81,0x3f,0xfc,0x18,0x3f,
  0xfe,0xff,0x7f,0xfe,0xff,0x7f,0xfe,0xff,0x7f,0x00,0x00,0x00,
};
const unsigned char ICON_JAM[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x3f,0x7e,0xfc,0x7f,0x7e,0xfe,
  0x7f,0x7e,0xfe,0x7f,0x7e,0xfe,0x7f,0x7e,0xfe,0x7f,0x7e,0xfe,
  0x7f,0x7e,0xfe,0x7f,0x7e,0xfe,0x7f,0x7e,0xfe,0x7f,0x7e,0xfe,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,
  0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
const unsigned char ICON_NOPASS[] U8X8_PROGMEM = {
  0x00,0x18,0x00,0x80,0xff,0x01,0xe0,0xc3,0x07,0x70,0x00,0x0e,
  0x38,0x00,0x1c,0x1c,0x00,0x38,0x0c,0x00,0x30,0x06,0x00,0x60,
  0xe6,0xe7,0x67,0xe6,0xe7,0x67,0xe2,0x67,0x44,0xe3,0x67,0xc4,
  0xe3,0x67,0xc4,0xe2,0x67,0x44,0xe6,0xe7,0x67,0xe6,0xe7,0x67,
  0x06,0x00,0x60,0x0c,0x00,0x30,0x1c,0x00,0x38,0x38,0x00,0x1c,
  0x70,0x00,0x0e,0xe0,0xc3,0x07,0x80,0xff,0x01,0x00,0x18,0x00,
};
const unsigned char ICON_TOLL[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x1c,0x00,0x38,
  0x1c,0x00,0x38,0x1c,0x00,0x38,0x1c,0x00,0x38,0x1c,0x00,0x38,
  0x1c,0x00,0x38,0x1c,0x00,0x38,0x1c,0x00,0x38,0xfc,0xff,0x3f,
  0xfc,0xff,0x3f,0x1c,0x00,0x38,0x1c,0x00,0x38,0x1c,0x00,0x38,
  0x1c,0x00,0x38,0x1c,0x00,0x38,0x1c,0x00,0x38,0x00,0x00,0x00,
};

const unsigned char ICON_KDC_START[] U8X8_PROGMEM = {
  0xfe,0xff,0x7f,0xff,0xff,0xff,0x07,0x00,0xe0,0x03,0x00,0xc0,
  0x03,0x00,0xc0,0x03,0x00,0xc0,0x03,0xc0,0xcf,0x03,0xe0,0xdf,
  0x03,0xe0,0xcf,0x83,0x60,0xc8,0xc3,0x61,0xc8,0xe3,0xe1,0xcf,
  0xf3,0xe3,0xcf,0xf3,0xe7,0xcf,0xf3,0x63,0xc8,0xf3,0x63,0xc8,
  0xf3,0xe3,0xcc,0x33,0xe3,0xcf,0x33,0xe3,0xcf,0x33,0xe3,0xdf,
  0x03,0x00,0xc8,0x07,0x00,0xe0,0xff,0xff,0xff,0xfe,0xff,0x7f,
};
const unsigned char ICON_KDC_END[] U8X8_PROGMEM = {
  0xfe,0xff,0x7f,0xff,0xff,0xf7,0x07,0x00,0xf0,0x03,0x00,0xf8,
  0x03,0x00,0x9c,0x03,0x00,0xce,0x03,0x00,0xc7,0x03,0x80,0xc3,
  0x03,0xc0,0xd9,0x83,0xe0,0xc8,0xc3,0x71,0xc8,0xe3,0x39,0xcf,
  0xf3,0x9c,0xcf,0x73,0xce,0xcf,0x33,0x67,0xc8,0x93,0x63,0xc8,
  0xc3,0xe1,0xcc,0xe3,0xe0,0xcf,0x73,0xe2,0xcf,0x39,0xe3,0xdf,
  0x1f,0x00,0xc8,0x0f,0x00,0xe0,0xef,0xff,0xff,0xfe,0xff,0x7f,
};

const unsigned char ICON_CLOSED[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0xff,0x00,0xc0,0xff,0x03,0xf0,0xff,0x0f,
  0xf8,0xff,0x1f,0xf8,0xff,0x1f,0xfc,0xff,0x3f,0xfc,0xff,0x3f,
  0xfe,0xff,0x7f,0xfe,0xff,0x7f,0x0e,0x00,0x70,0x0e,0x00,0x70,
  0x0e,0x00,0x70,0x0e,0x00,0x70,0xfe,0xff,0x7f,0xfe,0xff,0x7f,
  0xfc,0xff,0x3f,0xfc,0xff,0x3f,0xf8,0xff,0x1f,0xf8,0xff,0x1f,
  0xf0,0xff,0x0f,0xc0,0xff,0x03,0x00,0xff,0x00,0x00,0x00,0x00,
};
const unsigned char ICON_RAIL[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x00,0x0c,0x38,0x00,0x1c,
  0x7c,0x00,0x3e,0xfc,0x00,0x3f,0xf0,0x81,0x0f,0xe0,0xc3,0x07,
  0xc0,0xe7,0x03,0x80,0xff,0x01,0x00,0xff,0x00,0x00,0x7e,0x00,
  0x00,0x7e,0x00,0x00,0xff,0x00,0x80,0xff,0x01,0xc0,0xe7,0x03,
  0xe0,0xc3,0x07,0xf0,0x81,0x0f,0xfc,0x00,0x3f,0x7c,0x00,0x3e,
  0x38,0x00,0x1c,0x30,0x00,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,
};
const unsigned char ICON_NOLEFT[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0xff,0x00,0xc0,0xff,0x03,0xe0,0x00,0x07,
  0x30,0x00,0x0e,0x78,0x00,0x18,0xec,0x00,0x38,0x4c,0x01,0x30,
  0x86,0x01,0x60,0xc6,0x01,0x60,0xe6,0xff,0x60,0xf6,0xff,0x61,
  0xe6,0xff,0x61,0xc6,0xe1,0x61,0x86,0xc1,0x61,0x06,0xe1,0x61,
  0x0c,0xe0,0x31,0x1c,0xe0,0x35,0x18,0xe0,0x1d,0x70,0xe0,0x0d,
  0xe0,0xc0,0x05,0xc0,0xcf,0x00,0x00,0x1f,0x00,0x00,0x00,0x00,
};
const unsigned char ICON_NORIGHT[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0xff,0x00,0xc0,0xff,0x03,0xe0,0x00,0x07,
  0x30,0x00,0x0e,0x78,0x00,0x18,0xec,0x00,0x38,0xcc,0x81,0x30,
  0x86,0x80,0x61,0x06,0x80,0x63,0x06,0xff,0x67,0x86,0xff,0x6f,
  0x86,0xff,0x67,0x86,0x87,0x63,0x86,0x83,0x61,0x86,0xc3,0x60,
  0x8c,0x03,0x33,0x9c,0x03,0x37,0x98,0x03,0x1e,0xb0,0x03,0x0c,
  0xa0,0x03,0x07,0x00,0xf3,0x03,0x00,0xf8,0x00,0x00,0x00,0x00,
};
const unsigned char ICON_NOUTURN[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0xff,0x00,0xc0,0xff,0x03,0xe0,0x00,0x07,
  0x30,0x3c,0x0e,0x38,0xff,0x18,0xac,0xff,0x39,0x8c,0xff,0x33,
  0xc6,0xc3,0x63,0xc6,0x83,0x63,0xc6,0x89,0x63,0xc6,0x99,0x63,
  0xc6,0xe9,0x6f,0xc6,0xf1,0x6f,0xc6,0xc1,0x67,0xc6,0xc1,0x67,
  0xcc,0x81,0x33,0xdc,0x01,0x35,0xd8,0x01,0x1e,0xd0,0x01,0x0c,
  0xc0,0x01,0x07,0x00,0xfc,0x03,0x00,0xfe,0x00,0x00,0x00,0x00,
};
const unsigned char ICON_SCHOOL[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x01,0x00,0xe0,0x03,0x00,
  0xf0,0x03,0x00,0xf0,0x03,0x00,0xe0,0x01,0x00,0xc0,0x01,0x00,
  0xe0,0x01,0x07,0xe0,0x81,0x0f,0xe0,0x03,0x0f,0xe0,0x3f,0x07,
  0xe0,0xff,0x07,0xe0,0xe1,0x07,0xe0,0x01,0x07,0xe0,0x01,0x07,
  0xe0,0x01,0x07,0xe0,0x03,0x07,0xf0,0x03,0x0f,0xf0,0x03,0x0f,
  0xf0,0x87,0x0f,0x78,0x87,0x1f,0x70,0x87,0x1f,0x00,0x00,0x00,
};
const unsigned char ICON_CURVE[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0xfc,0x00,0x80,0xff,0x00,0xf0,0xff,0x00,0xf8,0x07,
  0x00,0x7e,0x00,0x00,0x1f,0x00,0x80,0x0f,0x00,0x80,0x07,0x00,
  0xc0,0x03,0xf0,0xc0,0x01,0xfc,0xe0,0x01,0xfe,0xe0,0x00,0x1f,
  0xf0,0x00,0x07,0x70,0x80,0x07,0x70,0x80,0x03,0x70,0x80,0x03,
  0x70,0x80,0x03,0x70,0xc0,0x03,0x70,0xc0,0x01,0x70,0xc0,0x01,
};
const unsigned char ICON_REST[] U8X8_PROGMEM = {
  0x00,0x00,0x00,0xfe,0xff,0x7f,0xfe,0xff,0x7f,0xfe,0xff,0x7f,
  0xfe,0xff,0x7f,0x7e,0x00,0x7f,0x7e,0x00,0x7e,0x7e,0x00,0x7c,
  0x7e,0x70,0x78,0x7e,0xf8,0x78,0x7e,0xf8,0x78,0x7e,0xf8,0x78,
  0x7e,0x70,0x78,0x7e,0x00,0x7c,0x7e,0x00,0x7e,0x7e,0x00,0x7f,
  0x7e,0xfc,0x7f,0x7e,0xfc,0x7f,0x7e,0xf8,0x7f,0xfe,0xff,0x7f,
  0xfe,0xff,0x7f,0xfe,0xff,0x7f,0xfe,0xff,0x7f,0x00,0x00,0x00,
};

// Moi canh bao: icon + khoang cach toi da (met) de bat dau hien.
// bmp = nullptr va speedNum = true -> ve bien toc do co so (alrV).
AlertIcon alertIcon(int a) {
  switch (a) {
    // --- Quan trong: hien tu xa (500 m) ---
    case 1:  return {ICON_POLICE, 500, false};
    case 2:  case 40: case 41: case 42: case 43: case 44: case 45: case 46:
             return {ICON_CAM, 500, false};
    case 3:  return {ICON_LIGHT, 500, false};
    case 4:  case 13: case 14: case 15: case 17: case 48:
             return {ICON_HAZARD, 500, false};
    case 5:  return {ICON_CRASH, 500, false};
    case 6:  return {ICON_JAM, 500, false};
    case 7:  return {ICON_CLOSED, 500, false};
    case 9:  return {ICON_NOPASS, 500, false};
    case 11: return {ICON_RAIL, 500, false};
    case 12: return {ICON_TOLL, 500, false};
    case 20: case 21: return {ICON_REST, 500, false};
    // --- Chi hien khi gan (200 m) ---
    case 8:  return {nullptr, 200, true};          // doi gioi han toc do (kem alrV)
    case 23: return {ICON_KDC_START, 200, false};
    case 24: return {ICON_KDC_END, 200, false};
    case 28: return {ICON_NOLEFT, 200, false};
    case 29: return {ICON_NORIGHT, 200, false};
    case 30: return {ICON_NOUTURN, 200, false};
    case 57: return {ICON_SCHOOL, 200, false};
    case 59: return {ICON_CURVE, 200, false};
    default: return {nullptr, 0, false};           // loai khac: khong hien, giu mui ten re
  }
}

void bx(int cx, bool m, int x, int y, int w, int h) {
  if (m) x = 2 * cx - x - w + 1;
  u8g2.drawBox(x, y, w, h);
}
void tri(int cx, bool m, int x1, int y1, int x2, int y2, int x3, int y3) {
  if (m) { x1 = 2 * cx - x1; x2 = 2 * cx - x2; x3 = 2 * cx - x3; }
  u8g2.drawTriangle(x1, y1, x2, y2, x3, y3);
}

void drawTurn(int cx, int cy, int trn, int ex) {
  bool left  = (trn == 2 || trn == 4 || trn == 6 || trn == 13 || trn == 15);
  bool right = (trn == 3 || trn == 5 || trn == 7 || trn == 14 || trn == 16);
  if (left || right) {
    bool m = right;
    bx(cx, m, cx + 2, cy - 2, 4, 14);
    bx(cx, m, cx - 6, cy - 5, 12, 4);
    tri(cx, m, cx - 12, cy - 3, cx - 4, cy - 11, cx - 4, cy + 5);
  } else if (trn == 1 || trn == 19) {                 // di thang
    u8g2.drawBox(cx - 2, cy - 4, 4, 16);
    u8g2.drawTriangle(cx, cy - 12, cx - 8, cy - 2, cx + 8, cy - 2);
  } else if (trn == 8 || trn == 9) {                  // quay dau
    u8g2.drawBox(cx + 4, cy - 4, 4, 16);
    u8g2.drawBox(cx - 7, cy - 8, 15, 4);
    u8g2.drawBox(cx - 7, cy - 8, 4, 12);
    u8g2.drawTriangle(cx - 5, cy + 12, cx - 13, cy + 3, cx + 3, cy + 3);
  } else if (trn == 10 || trn == 11 || trn == 12 || trn == 20) {  // vong xoay
    u8g2.drawCircle(cx, cy, 11);
    u8g2.drawCircle(cx, cy, 10);
    if (ex > 0) {
      char b[4];
      snprintf(b, sizeof(b), "%d", ex);
      u8g2.setFont(u8g2_font_helvB10_tn);
      drawCentered(b, cx, cy + 5);
    }
  } else if (trn == 17) {                             // den noi: la co ca-ro
    u8g2.drawBox(cx - 7, cy - 11, 2, 23);             // cot co
    u8g2.drawFrame(cx - 5, cy - 11, 13, 9);           // vien co
    for (int r = 0; r < 2; r++)
      for (int c = 0; c < 3; c++)
        if (((r + c) & 1) == 0) u8g2.drawBox(cx - 4 + c * 4, cy - 10 + r * 4, 4, 4);
  }
}

void drawSpeedSign(int cx, int cy, int v) {
  u8g2.drawCircle(cx, cy, 17);
  u8g2.drawCircle(cx, cy, 16);
  u8g2.drawCircle(cx, cy, 15);
  if (v <= 0) return;
  char b[8];
  snprintf(b, sizeof(b), "%d", v);
  if (v >= 100) {
    u8g2.setFont(u8g2_font_helvB08_tn);
    drawCentered(b, cx, cy + 4);
  } else {
    u8g2.setFont(u8g2_font_helvB10_tn);
    drawCentered(b, cx, cy + 5);
  }
}

// Phong to icon XBM 24x24 thanh 36x36 bang nearest-neighbour.
void drawScaledIcon36(const unsigned char* src, int x, int y) {
  static uint8_t scaled[36 * 5];
  memset(scaled, 0, sizeof(scaled));
  for (int dy = 0; dy < 36; dy++) {
    int sy = dy * 24 / 36;
    for (int dx = 0; dx < 36; dx++) {
      int sx = dx * 24 / 36;
      uint8_t bits = pgm_read_byte(src + sy * 3 + sx / 8);
      if (bits & (1u << (sx & 7))) {
        scaled[dy * 5 + dx / 8] |= (1u << (dx & 7));
      }
    }
  }
  u8g2.drawXBM(x, y, 36, 36, scaled);
}

void drawScreen(bool fresh) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  if (!fresh) {
    u8g2.setFont(u8g2_font_7x13B_tr);
    drawCentered("WAZE HUD", 64, 24);
    drawCentered(bleConnected ? "WAIT DATA..." : "WAIT CONNECT BLE", 64, 44);
    u8g2.sendBuffer();
    return;
  }

  char b[32];

  // Gioi han toc do: vong tron TO ben trai. Vuot toc do thi nhap nhay dac/dao mau.
  bool over = hud.over || (hud.lim > 0 && hud.spd > hud.lim);
  bool inv = over && ((millis() / 400) % 2 == 0);
  const int LX = 28, LY = 25, LR = 25;
  if (inv) u8g2.drawDisc(LX, LY, LR);
  else {
    u8g2.drawCircle(LX, LY, LR);
    u8g2.drawCircle(LX, LY, LR - 1);
    u8g2.drawCircle(LX, LY, LR - 2);
  }
  u8g2.setDrawColor(inv ? 0 : 1);
  if (hud.lim > 0) {
    snprintf(b, sizeof(b), "%d", hud.lim);
    if (strlen(b) >= 3) {
      u8g2.setFont(u8g2_font_fub17_tn);      // dam, cho so 3 chu so
      drawCentered(b, LX, LY + 8);
    } else {
      u8g2.setFont(u8g2_font_fub25_tn);      // dam, cho so 1-2 chu so
      drawCentered(b, LX, LY + 12);
    }
  } else {
    u8g2.setFont(u8g2_font_fub25_tn);
    drawCentered("--", LX, LY + 12);
  }
  u8g2.setDrawColor(1);

  // Toc do hien tai: nho gon o goc tren phai de uu tien icon + khoang cach
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(70, 7, "WAZE");
  u8g2.setFont(u8g2_font_logisoso16_tn);
  snprintf(b, sizeof(b), "%d", hud.spd);
  u8g2.drawStr(126 - u8g2.getStrWidth(b), 18, b);

  // Ben phai: icon canh bao duoc phong to tu 24x24 len 30x30
  AlertIcon ai = {nullptr, 0, false};
  int adist = -1;
  if (hud.avg) {
    ai = {ICON_NOPASS, 100000, false};
    adist = hud.avgL;
  } else if (hud.alr > 0 && hud.alrD >= 0) {
    AlertIcon t = alertIcon(hud.alr);
    if ((t.bmp || t.speedNum) && hud.alrD <= t.maxM) {
      ai = t;
      adist = hud.alrD;
    }
  }

  char d[12];
  if (ai.bmp || ai.speedNum) {
    if (ai.speedNum) {
      drawSpeedSign(95, 39, hud.alrV);
    } else {
      drawScaledIcon36(ai.bmp, 77, 20);
    }
    fmtDist(adist, d, sizeof(d));
    u8g2.setFont(u8g2_font_fub14_tr);
    int dw = u8g2.getStrWidth(d);
    u8g2.drawStr(96 - dw / 2, 63, d);
  } else if (hud.nav) {
    drawTurn(78, 37, hud.trn, hud.exitN);
    if (hud.dst >= 0) {
      fmtDist(hud.dst, d, sizeof(d));
      u8g2.setFont(u8g2_font_fub14_tr);
      int dw = u8g2.getStrWidth(d);
      u8g2.drawStr(96 - dw / 2, 63, d);
    }
  }

  if (!soundOn) {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(0, 63, "MUTE");
  }

  u8g2.sendBuffer();
}

// ---------- Khi nao keu ----------
void updateSounds(bool fresh) {
  static bool wasOver = false;
  static uint32_t lastOverBeep = 0;

  // Chi phat tieng bip khi vuot toc do; khong phat am thanh camera/CSGT/tai nan/re.
  bool over = fresh && (hud.over || (hud.lim > 0 && hud.spd > hud.lim));
  if (!over) {
    wasOver = false;
    return;
  }

  // Bip ngay khi bat dau vuot; neu van vuot, lap lai moi 5 giay.
  if (!wasOver || millis() - lastOverBeep >= 5000) {
    playPattern(P_OVER);
    lastOverBeep = millis();
  }
  wasOver = true;
}

// ---------- Demo ----------
#if DEMO_MODE
void demoStep() {
  static uint32_t t0 = 0;
  if (millis() - t0 < 700) return;
  t0 = millis();
  static int k = 0;
  k++;
  const int turns[] = {1, 2, 3, 8, 10, 17};
  hud.nav = 1;
  hud.spd = 40 + (k * 3) % 40;
  hud.lim = 60;
  hud.trn = turns[(k / 6) % 6];
  hud.exitN = 2;
  hud.dst = 1500 - (k * 37) % 1400;
  hud.avg = (k / 10) % 2;
  hud.avgL = 300;
  const int alerts[] = {2, 3, 1, 5, 6, 12, 4, 7, 8, 11, 20, 23, 24, 28, 29, 30, 57, 59};
  hud.alr = ((k / 7) % 2) ? alerts[(k / 14) % 18] : 0;
  hud.alrD = 180;
  hud.alrV = 40;
  lastRx = millis();
}
#endif

// ---------- Vao / ra che do HUD ----------
void enterHudMode()
{   oledWake();
    if (!ensureHudQueue())
    {
        Serial.println("HUD MODE START FAILED -> RESTART");
        Serial.flush();
        delay(150);
        ESP.restart();
        return;
    }

    Serial.println("BLE CONNECTED -> HUD MODE");

    hudMode = true;

    // 1) Dung radio (giai phong HTTP stream + buffer am thanh)
    channelChangeRequested = false;
    stopStream();
    Serial.println("HUD step 1: radio stopped");
    Serial.flush();

    // 2) Tat WiFi + web: nhuong song cho BLE, giai phong RAM
    if (wifiSetupMode)
    {
        dnsServer.stop();
        setupServer.stop();
        WiFi.softAPdisconnect(true);
        wifiSetupMode = false;
    }

    if (normalServerStarted)
    {
        server.stop();
        MDNS.end();
        normalServerStarted = false;
    }

    if (WiFi.getMode() != WIFI_OFF)
    {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    }
    Serial.println("HUD step 2: wifi off");
    Serial.flush();

    // 3) Man hinh: U8g2 khoi tao lai OLED (radio dung Adafruit)
    u8g2.begin();
    u8g2.setBusClock(400000);
    Serial.println("HUD step 3: display ok");
    Serial.flush();

    // 4) Am thanh: dung lai I2S cua radio cho tieng bip
    hudAudioStart();
    Serial.println("HUD step 4: audio ok");
    Serial.flush();

    hud = Hud();
    lastRx = 0;

    Serial.printf("HUD READY - HEAP FREE: %u bytes\n", ESP.getFreeHeap());
}

void loopHud()
{
    static char line[520];

    if (sendDevPending) {                    // Android vua bat notify
        sendDevPending = false;
        sendLine(DEV_JSON);
    }
    if (lineQ != nullptr)
        while (xQueueReceive(lineQ, line, 0) == pdTRUE) handleLine(line);

#if DEMO_MODE
    demoStep();
#endif

    // Nut GPIO4 trong HUD: tat/bat tieng bip
    static bool lastBtn = true;
    static uint32_t tBtn = 0;
    bool btn = digitalRead(BUTTON_PIN);
    if (!btn && lastBtn && millis() - tBtn > 250) {
        soundOn = !soundOn;
        tBtn = millis();
    }
    lastBtn = btn;

    bool fresh = lastRx != 0 && (millis() - lastRx < 3000);
    updateSounds(fresh);
    drawScreen(fresh);

#if !DEMO_MODE
    // Mat BLE qua lau -> khoi dong lai de quay ve radio
    // (don gian va sach: WiFi, web, I2S, man hinh deu khoi tao lai tu dau)
    static uint32_t bleLostAt = 0;

    if (bleConnected) {
        bleLostAt = 0;
    } else if (bleLostAt == 0) {
        bleLostAt = millis();
    } else if (millis() - bleLostAt >= HUD_EXIT_DELAY_MS) {
        Serial.println("BLE LOST -> RESTART TO RADIO");

        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_7x13B_tr);
        drawCentered("BACK TO RADIO", 64, 36);
        u8g2.sendBuffer();

        delay(300);
        ESP.restart();
    }
#endif

    delay(80);
}

#endif  // ENABLE_HUD

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);

    // USB CDC: cho Serial Monitor ket noi (toi da 3 giay) de khong mat log luc boot
    unsigned long serialWait = millis();

    while (!Serial && millis() - serialWait < 3000)
        delay(10);

    delay(200);
    Serial.println();
    Serial.println("SERIAL OK");
    Serial.println("=== ESP32-C3 INTERNET RADIO ===");

    // Ly do reset lan truoc: SOFTWARE RESTART = ESP.restart(), PANIC = crash
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char* why =
            rr == ESP_RST_POWERON  ? "POWER ON" :
            rr == ESP_RST_SW       ? "SOFTWARE RESTART (ESP.restart)" :
            rr == ESP_RST_PANIC    ? "PANIC (CRASH)" :
            rr == ESP_RST_INT_WDT  ? "INTERRUPT WATCHDOG" :
            rr == ESP_RST_TASK_WDT ? "TASK WATCHDOG" :
            rr == ESP_RST_WDT      ? "WATCHDOG" :
            rr == ESP_RST_BROWNOUT ? "BROWNOUT (thieu nguon)" : "OTHER";
        Serial.printf("RESET REASON: %s (%d)\n", why, (int)rr);
    }

    // Kenh cuoi cung
    radioPrefs.begin(PREF_NAMESPACE, false);

    loadCustomStations();

    currentStation = radioPrefs.getUChar(PREF_STATION, 0);

    if (currentStation >= totalStations())
        currentStation = 0;

    Serial.printf("LAST STATION: %u\n", currentStation + 1);

    // Nut
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(VOLUME_BUTTON_PIN, INPUT_PULLUP);

    buttonStableState = digitalRead(BUTTON_PIN);
    buttonLastReading = buttonStableState;
    if (buttonStableState == LOW)
        buttonPressStarted = millis();

    volumeButtonStableState = digitalRead(VOLUME_BUTTON_PIN);
    volumeButtonLastReading = volumeButtonStableState;

    // OLED
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(400000);

    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
    {
        Serial.println("OLED ERROR");

        while (true)
            delay(1000);
    }

    oledReady = true;

    display.setRotation(2);   // man hinh lap nguoc 180 do

    showStatus("INTERNET RADIO", "STARTING...");

    // I2S + MP3 (tao truoc WiFi de startStream luon co out hop le)
    out = new RadioOutputI2S();

    if (out == nullptr)
    {
        showStatus("I2S ERROR", "RESTART REQUIRED");

        while (true)
            delay(1000);
    }

    out->SetPinout(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO);
    out->SetGain(AUDIO_GAIN);

    mp3 = new AudioGeneratorMP3();

    if (mp3 == nullptr)
    {
        showStatus("MP3 ERROR", "RESTART REQUIRED");

        while (true)
            delay(1000);
    }

#if ENABLE_HUD
#if DEMO_MODE
    // Thu man hinh HUD: vao HUD ngay, du lieu gia, khong WiFi/radio/BLE
    enterHudMode();
    return;
#else
    // Mac dinh BLE tat de uu tien do muot radio; giu GPIO4 2 giay de bat HUD BLE.
    bleEnabled = false;
    Serial.println("HUD BLE OFF AT BOOT - hold GPIO4 for 2 seconds to enable HUD");
#endif
#endif

    // WiFi
    showStatus("CONNECTING WIFI", "PLEASE WAIT...");
    wifiPrefs.begin("wifi", false);
    savedSSID = wifiPrefs.getString("ssid", "");
    savedPASS = wifiPrefs.getString("pass", "");

    bool wifiConnected = connectSavedWiFi();
    if (!wifiConnected) {
        // Khong co WiFi hop le: vao che do cai dat WiFi.
        // loop() se xu ly web setup cho den khi nguoi dung luu WiFi.
        if (!bleEnabled)
            startWiFiSetupAP();
    } else {
        // WiFi da ket noi: mo web control va bat dau radio.
        // Tat tiet kiem dien WiFi: stream 128k khong bi giat luc moi mo kenh
        WiFi.setSleep(false);

        startNormalWebServer();

        // Hien dia chi web control tren OLED de khong can Serial
        showStatus("WEB CONTROL", WiFi.localIP().toString().c_str());

        showStatus("OPENING RADIO", stName(currentStation));

        if (!startStream())
            noteRadioFailure();
    }

    lastOLEDUpdate = 0;

    Serial.println("SETUP COMPLETE");
}


// ============================================================
// LOOP  (khong delay - mp3->loop() can chay lien tuc)
// ============================================================


const char* wifiSetupHTML = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<meta name="theme-color" content="#0b0f14">
<title>ESP32 Radio Wi-Fi Setup</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#0b0f14;color:#eef2f7;font-family:Arial,sans-serif;padding:22px 14px}
.wrap{max-width:460px;margin:auto}
.card{background:#121820;border:1px solid #202a35;border-radius:22px;padding:20px;box-shadow:0 12px 35px rgba(0,0,0,.28)}
.brand{font-size:12px;letter-spacing:2px;color:#8d9aaa;font-weight:bold}
h1{font-size:27px;margin:18px 0 8px}
p{color:#9ca8b6;font-size:14px;line-height:1.5}
label{display:block;color:#9ca8b6;font-size:12px;margin:18px 0 7px}
input{width:100%;padding:14px;border-radius:13px;border:1px solid #2a3541;background:#171f28;color:#fff;font-size:16px;outline:none}
input:focus{border-color:#617386}
button{width:100%;margin-top:20px;padding:15px;border:0;border-radius:14px;background:#e9edf2;color:#10151b;font-size:16px;font-weight:800}
.note{margin-top:15px;font-size:11px;color:#657181;text-align:center}
</style>
</head>
<body>
<div class="wrap">
<div class="card">
<div class="brand">ESP32 INTERNET RADIO</div>
<h1>Wi-Fi Setup</h1>
<p>ESP32 không kết nối được Wi-Fi đã lưu. Nhập Wi-Fi mới bên dưới, sau đó ESP32 sẽ lưu và khởi động lại.</p>
<form action="/save" method="get">
<label>TÊN WI-FI (SSID)</label>
<input name="ssid" id="ssid" maxlength="32" required autocomplete="off" autocapitalize="none">
<button type="button" id="scanBtn" onclick="scanWifi()" style="background:#202a35;color:#fff;margin-top:10px">QUÉT WI-FI</button>
<div id="nets" style="margin-top:10px"></div>
<label>MẬT KHẨU</label>
<input name="pass" type="password" maxlength="63" autocomplete="off">
<button type="submit">LƯU & KẾT NỐI</button>
</form>
<div class="note">Sau khi lưu, hãy chờ khoảng 10–20 giây rồi kết nối lại điện thoại vào Wi-Fi mới.</div>
</div>
</div>
<script>
async function scanWifi(){
 var b=document.getElementById('scanBtn'),box=document.getElementById('nets');
 b.disabled=true;b.textContent='ĐANG QUÉT...';
 for(var i=0;i<20;i++){
  try{
   var r=await (await fetch('/scan')).json();
   if(!r.scanning){
    var m={};
    r.nets.forEach(function(n){if(n.ssid&&(!(n.ssid in m)||n.rssi>m[n.ssid]))m[n.ssid]=n.rssi});
    box.innerHTML='';
    Object.keys(m).sort(function(x,y){return m[y]-m[x]}).forEach(function(s){
     var o=document.createElement('button');
     o.type='button';
     o.textContent=s+'  ('+m[s]+' dBm)';
     o.style.cssText='margin-top:6px;padding:11px;background:#171f28;color:#fff;font-weight:600;font-size:14px;text-align:left';
     o.onclick=function(){document.getElementById('ssid').value=s;document.querySelector('input[name=pass]').focus()};
     box.appendChild(o);
    });
    b.textContent=box.children.length?'QUÉT LẠI':'KHÔNG THẤY WI-FI - QUÉT LẠI';
    b.disabled=false;return;
   }
  }catch(e){}
  await new Promise(function(f){setTimeout(f,1000)});
 }
 b.textContent='QUÉT THẤT BẠI - THỬ LẠI';b.disabled=false;
}
</script>
</body>
</html>
)rawliteral";

bool connectSavedWiFi()
{
    String targetSSID = savedSSID;
    String targetPASS = savedPASS;

    if (targetSSID.length() == 0) {
        Serial.println("NO WIFI CREDENTIALS");
        return false;
    }

    Serial.print("WIFI CONNECT: ");
    Serial.println(targetSSID);

    Serial.println("Using saved Wi-Fi credentials");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false);
    delay(100);
    WiFi.begin(targetSSID.c_str(), targetPASS.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
        handleButton();
        if (bleEnabled)
            return false;
        delay(10);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WIFI OK, IP: ");
        Serial.print(WiFi.localIP());
        Serial.print("  RSSI: ");
        Serial.println(WiFi.RSSI());

        wifiSetupMode = false;
        return true;
    }

    Serial.println("WIFI FAILED -> START SETUP AP");
    WiFi.disconnect(false);
    delay(100);
    return false;
}

String jsonEsc(const String& s)
{
    String o;

    for (size_t i = 0; i < s.length(); i++)
    {
        char c = s[i];

        if (c == '"' || c == '\\')
        {
            o += '\\';
            o += c;
        }
        else if ((uint8_t)c < 0x20)
            o += ' ';
        else
            o += c;
    }

    return o;
}

void startWiFiSetupAP()
{   oledWake();
    wifiSetupMode = true;
    setupLastCheck = millis();
    setupLastRetry = millis();

    // Web control dang chay (mat WiFi giua chung) thi dong lai de nhuong cong 80
    if (normalServerStarted)
    {
        server.stop();
        MDNS.end();
        normalServerStarted = false;
    }

    // AP + STA: van giu STA de thu ket noi lai WiFi da luu
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false);
    delay(200);
    WiFi.mode(WIFI_AP_STA);
    delay(200);

    bool apOK = WiFi.softAP("ESP32-RADIO");

    // Captive portal: moi ten mien deu tro ve ESP32
    dnsServer.start(53, "*", WiFi.softAPIP());

    showSetupScreen();

    Serial.println("================================");
    Serial.print("AP START: ");
    Serial.println(apOK ? "OK" : "FAILED");
    Serial.println("WIFI SETUP MODE");
    Serial.println("AP: ESP32-RADIO");
    Serial.print("SETUP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.println("Connect phone to ESP32-RADIO");
    Serial.println("Then open http://192.168.4.1");
    Serial.println("================================");

    setupServer.on("/", []() {
        setupServer.send(200, "text/html; charset=utf-8", wifiSetupHTML);
    });

    setupServer.on("/scan", []() {
        int n = WiFi.scanComplete();

        if (n == WIFI_SCAN_FAILED) {
            WiFi.scanNetworks(true);   // quet bat dong bo
            setupServer.send(200, "application/json", "{\"scanning\":true}");
            return;
        }

        if (n == WIFI_SCAN_RUNNING) {
            setupServer.send(200, "application/json", "{\"scanning\":true}");
            return;
        }

        String j = "{\"scanning\":false,\"nets\":[";

        for (int i = 0; i < n; i++) {
            if (i) j += ',';
            j += "{\"ssid\":\"";
            j += jsonEsc(WiFi.SSID(i));
            j += "\",\"rssi\":";
            j += String(WiFi.RSSI(i));
            j += "}";
        }

        j += "]}";

        WiFi.scanDelete();

        setupServer.send(200, "application/json", j);
    });

    // Captive portal: URL la deu chuyen ve trang cai dat
    setupServer.onNotFound([]() {
        setupServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
        setupServer.send(302, "text/plain", "");
    });

    setupServer.on("/save", []() {
        if (!setupServer.hasArg("ssid")) {
            setupServer.send(400, "text/plain", "Missing SSID");
            return;
        }

        String newSSID = setupServer.arg("ssid");
        String newPASS = setupServer.hasArg("pass") ? setupServer.arg("pass") : "";

        newSSID.trim();

        if (newSSID.length() == 0 || newSSID.length() > 32) {
            setupServer.send(400, "text/plain; charset=utf-8", "Ten WiFi khong hop le (1-32 ky tu)");
            return;
        }

        if (newPASS.length() != 0 && (newPASS.length() < 8 || newPASS.length() > 63)) {
            setupServer.send(400, "text/plain; charset=utf-8", "Mat khau phai tu 8 den 63 ky tu (hoac de trong neu mang mo)");
            return;
        }

        wifiPrefs.putString("ssid", newSSID);
        wifiPrefs.putString("pass", newPASS);

        String msg =
            "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{background:#0b0f14;color:#eef2f7;font-family:Arial;text-align:center;padding:45px}"
            "div{background:#121820;padding:25px;border-radius:20px;max-width:420px;margin:auto}"
            "</style></head><body><div><h2>Đã lưu Wi-Fi</h2>"
            "<p>ESP32 đang khởi động lại và thử kết nối mạng mới.</p>"
            "</div></body></html>";

        setupServer.send(200, "text/html; charset=utf-8", msg);
        delay(1200);
        ESP.restart();
    });

    setupServer.begin();
    Serial.println("SETUP WEB SERVER STARTED");
}

void startNormalWebServer()
{
    startWebServer();
    normalServerStarted = true;

    // Truy cap bang ten: http://radio.local
    if (MDNS.begin("radio")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("MDNS: http://radio.local");
    }
    Serial.println("WEB SERVER STARTED");
    Serial.print("WEB CONTROL: http://");
    Serial.println(WiFi.localIP());
}

void handleWiFiSetupMode()
{
    if (!wifiSetupMode)
        return;

    dnsServer.processNextRequest();
    setupServer.handleClient();

    unsigned long now = millis();

    bool nobodyConnected = (WiFi.softAPgetStationNum() == 0);

    // WiFi da luu ket noi lai duoc va khong ai dang cai dat -> quay ve che do radio
    if (now - setupLastCheck >= 3000) {
        setupLastCheck = now;

        if (WiFi.status() == WL_CONNECTED && nobodyConnected) {
            Serial.println("SAVED WIFI BACK -> RESTART");
            delay(200);
            ESP.restart();
        }
    }

    // Dinh ky thu lai WiFi da luu, chi khi khong co ai dang dung trang cai dat
    // (vi thu ket noi se quet kenh va co the lam rot ket noi cua dien thoai)
    if (now - setupLastRetry >= 20000 && nobodyConnected &&
        WiFi.status() != WL_CONNECTED && savedSSID.length() > 0) {
        setupLastRetry = now;

        Serial.println("SETUP MODE: TRY SAVED WIFI");
        WiFi.begin(savedSSID.c_str(), savedPASS.c_str());
    }
}

void loop()
{
#if ENABLE_HUD
    // Dang o che do HUD: chi chay HUD (thoat bang khoi dong lai)
    if (hudMode) {
        loopHud();
        return;
    }

    // Chi chuyen HUD neu nguoi dung da bat BLE va dien thoai ket noi
    if (bleEnabled && bleConnected) {
        enterHudMode();
        return;
    }

    // Dang cho dien thoai ket noi: khong chay lai radio/Wi-Fi/OLED radio,
    // tranh cac ham nen ghi de man hinh cho BLE hoac lam BLE bi treo.
    if (bleEnabled && !bleConnected) {
        handleButton(); // van cho phep giu GPIO4 2 giay de tat BLE
        delay(2);
        return;
    }
#endif

    if (wifiSetupMode) {
        handleButton();
        handleWiFiSetupMode();
        delay(2);
        return;
    }

    server.handleClient();

    // In lai dia chi web control moi 15 giay (khong lo log luc boot)
    static unsigned long lastAddressPrint = 0;

    if (WiFi.status() == WL_CONNECTED && millis() - lastAddressPrint >= 30000UL)
    {
        lastAddressPrint = millis();

        Serial.print("WEB CONTROL: http://");
        Serial.print(WiFi.localIP());
        Serial.println("  (hoac http://radio.local)");
                Serial.printf("HEAP: free=%u  min=%u  maxblock=%u\n",
                      ESP.getFreeHeap(),
                      ESP.getMinFreeHeap(),
                      ESP.getMaxAllocHeap());
    }

    handleButton();
    handleVolumeButton();
    handleChannelChange();
    handleRadio();
    saveSelectedStation();
    handleWiFi();
    updateOLED();
}
