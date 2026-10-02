#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_bt.h>
#include <esp_sleep.h>

// ---- Pin definitions (ESP32-C3 Super Mini) ----
// Strapping pins (GPIO2/8/9) are avoided. GPIO8 is also the onboard LED.
// GPIO5 has internal board leakage (even on a breadboard) - do not use.
#define DHT_PIN     7      // DHT22 data (module has internal pull-up)
#define DHT_TYPE    DHT22
#define I2C_SDA     0      // OLED SDA
#define I2C_SCL     10     // OLED SCL

// ---- OLED ----
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C

// The last two parameters define I2C speed (during / after transfer). Library
// defaults to 400kHz; without external pull-up resistors (only ESP's ~45k weak internal
// pull-ups), the signal edges cannot rise in time at that speed. 100kHz leaves a safe margin.
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET, 100000UL, 100000UL);

DHT dht(DHT_PIN, DHT_TYPE);

const unsigned long READ_INTERVAL_MS = 3000;   // DHT22 can be sampled at ~0.5Hz max
const unsigned long USB_DEBUG_GRACE_MS = 15000; // allow native USB time to enumerate after boot
unsigned long lastReadMillis = 0;

// Periodic retry if OLED is missing - allows the display to come online
// if a loose cable is reconnected without needing a board reboot.
const unsigned long OLED_RETRY_INTERVAL_MS = 5000;
unsigned long nextOledRetryMillis = 0;

float lastTemp = NAN;
float lastHum = NAN;
bool lastReadOk = false;
bool oledOk = false;

// ---------------------------------------------------------------------------
// Bit-bang I2C Probe
//
// display.begin() does not verify ACK: it returns "success" even with no display
// connected and locks up for ~12 seconds timing out on every command. Additionally,
// the ESP I2C peripheral can hang indefinitely on a faulty bus despite setTimeOut.
// Therefore, we probe "is the device present?" manually at the GPIO level -
// this routine never locks up under any circumstance.
// ---------------------------------------------------------------------------
#define BB_DELAY_US 5

static void bbRelease(uint8_t pin) { pinMode(pin, INPUT_PULLUP); }
static void bbPullLow(uint8_t pin) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }

bool bitBangAck(uint8_t addr) {
    bbRelease(I2C_SDA); bbRelease(I2C_SCL); delayMicroseconds(50);
    if (!digitalRead(I2C_SDA) || !digitalRead(I2C_SCL)) return false;  // dead bus

    bbPullLow(I2C_SDA); delayMicroseconds(BB_DELAY_US);   // START
    bbPullLow(I2C_SCL); delayMicroseconds(BB_DELAY_US);

    uint8_t data = addr << 1;                             // address + write bit
    for (int i = 7; i >= 0; i--) {
        if (data & (1 << i)) bbRelease(I2C_SDA); else bbPullLow(I2C_SDA);
        delayMicroseconds(BB_DELAY_US);
        bbRelease(I2C_SCL); delayMicroseconds(BB_DELAY_US);
        bbPullLow(I2C_SCL); delayMicroseconds(BB_DELAY_US);
    }

    bbRelease(I2C_SDA); delayMicroseconds(BB_DELAY_US);   // ACK bit
    bbRelease(I2C_SCL); delayMicroseconds(BB_DELAY_US);
    bool ack = (digitalRead(I2C_SDA) == LOW);
    bbPullLow(I2C_SCL); delayMicroseconds(BB_DELAY_US);

    bbPullLow(I2C_SDA); delayMicroseconds(BB_DELAY_US);   // STOP
    bbRelease(I2C_SCL); delayMicroseconds(BB_DELAY_US);
    bbRelease(I2C_SDA); delayMicroseconds(BB_DELAY_US);
    return ack;
}

// Wire is never initialized until the display is actually found.
// This prevents peripheral operations on an empty bus and avoids Wire.end()/begin() loops.
bool tryInitOled() {
    bbRelease(I2C_SDA); bbRelease(I2C_SCL); delayMicroseconds(50);
    int sda = digitalRead(I2C_SDA);
    int scl = digitalRead(I2C_SCL);
    if (!sda || !scl) {
        Serial.printf("[I2C Bus Error] SDA=%d, SCL=%d (Line stuck LOW! Check loose wire or short)\n", sda, scl);
        return false;
    }
    if (!bitBangAck(OLED_ADDR)) {
        Serial.println("[I2C Probe] SDA & SCL HIGH, but device 0x3C gave no ACK (unpowered or wrong address)");
        return false;
    }

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(100000);
    Wire.setTimeOut(50);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) return false;
    display.setRotation(2);   // rotate display 180 degrees
    return true;
}

// Draws centered text horizontally at row y.
void drawCentered(const char* text, uint8_t size, int16_t y) {
    display.setTextSize(size);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, y);
    display.print(text);
}

// Activity indicator: small bar moving down one step on each refresh along the right edge.
// Moves even when temp/humidity remain constant - if it stops, you know immediately
// that the display is frozen (e.g. I2C disconnected).
// Size 3 text at widest ("-10.5C", 6 chars = 108px) centered leaves ~10px padding on each side,
// bar at x=125..127 never overlaps text.
#define HEARTBEAT_STEPS 8
uint8_t heartbeatFrame = 0;

static void drawHeartbeat() {
    int16_t y = (heartbeatFrame % HEARTBEAT_STEPS) * (SCREEN_HEIGHT / HEARTBEAT_STEPS);
    display.fillRect(125, y, 3, 6, SSD1306_WHITE);
    heartbeatFrame++;
}

// Screen layout (128x64): temperature and humidity centered full screen
void drawReadings(float temp, float hum, bool ok) {
    if (!oledOk) return;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    if (!ok) {
        drawCentered("DHT22", 2, 16);
        drawCentered("ERROR!", 2, 36);
    } else {
        char tempStr[8];
        snprintf(tempStr, sizeof(tempStr), "%.1fC", temp);
        char humStr[8];
        snprintf(humStr, sizeof(humStr), "%.1f%%", hum);
        drawCentered(tempStr, 3, 4);
        drawCentered(humStr, 3, 36);
    }

    drawHeartbeat();
    display.display();
}

void setup() {
    Serial.begin(115200);
    Serial.println("=== Boot started ===");

    // Disable BLE to save battery
    btStop();
    esp_bt_controller_disable();

    setCpuFrequencyMhz(80);   // ESP32-C3: 80/40/20/10 MHz options

    // Wire is not initialized here - tryInitOled() initializes it if the display is found.
    oledOk = tryInitOled();
    Serial.printf("OLED: %s\n", oledOk ? "FOUND" : "NOT FOUND");
    if (oledOk) {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("Starting...");
        display.display();
    }

    delay(1500);   // DHT22 needs ~1 s after board power-up before the first read
    dht.begin();
}

void loop() {
    unsigned long now = millis();

    if (!oledOk && (long)(now - nextOledRetryMillis) >= 0) {
        nextOledRetryMillis = now + OLED_RETRY_INTERVAL_MS;
        if (tryInitOled()) {
            oledOk = true;
            Serial.println("OLED found later, display active.");
        }
    }

    if (now - lastReadMillis >= READ_INTERVAL_MS) {
        lastReadMillis = now;

        float h = dht.readHumidity();
        float t = dht.readTemperature();

        Serial.printf("[status] OLED: %-9s | ", oledOk ? "OK" : "NOT FOUND");
        if (isnan(h) || isnan(t)) {
            lastReadOk = false;
            Serial.println("DHT22 read error (NaN)");
        } else {
            lastReadOk = true;
            lastTemp = t;
            lastHum = h;
            Serial.printf("Temperature: %.1f C  Humidity: %.1f %%\n", t, h);
        }

        drawReadings(lastTemp, lastHum, lastReadOk);
    }

    // On battery, sleep until the next scheduled sensor read or OLED retry.
    // The SSD1306 retains its current image while the ESP32-C3 is asleep.
    // Keep the CPU awake while a USB serial host is connected for reliable debugging.
    if (millis() >= USB_DEBUG_GRACE_MS && !Serial) {
        now = millis();
        unsigned long untilRead = (now - lastReadMillis >= READ_INTERVAL_MS)
            ? 1
            : READ_INTERVAL_MS - (now - lastReadMillis);
        unsigned long sleepMs = untilRead;

        if (!oledOk) {
            long untilRetry = (long)(nextOledRetryMillis - now);
            unsigned long retryMs = untilRetry <= 0 ? 1 : (unsigned long)untilRetry;
            if (retryMs < sleepMs) sleepMs = retryMs;
        }

        esp_sleep_enable_timer_wakeup((uint64_t)sleepMs * 1000ULL);
        esp_light_sleep_start();
    }
}
