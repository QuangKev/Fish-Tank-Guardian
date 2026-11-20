#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"

/*
 * UART ESP32 <-> S32K144
 *
 * ESP32 GPIO17 TX2 -> S32K PTD6 / LPUART2_RX
 * ESP32 GPIO16 RX2 <- S32K PTD7 / LPUART2_TX
 * ESP32 GND        -> S32K GND
 */
#define ESP32_RX2 16
#define ESP32_TX2 17

HardwareSerial S32K_Serial(2);

/* ================== CONFIG (kept out of git) ==================
 * Copy secrets.example.h to secrets.h and fill in your own values.
 * secrets.h is listed in .gitignore and must never be committed.
 */
#include "secrets.h"

/* ================== MQTT ================== */

WiFiClient mqttClient;
Adafruit_MQTT_Client mqtt(&mqttClient,
                          AIO_SERVER,
                          AIO_SERVERPORT,
                          AIO_USERNAME,
                          AIO_KEY);

/*
 * Change this feed name if your Adafruit IO feed is not "quat-den".
 */
Adafruit_MQTT_Subscribe command_sub =
  Adafruit_MQTT_Subscribe(&mqtt, AIO_USERNAME "/feeds/" AIO_FEED_NAME);

WiFiClientSecure discordClient;

/* ================== FEED STATE ================== */

enum FeedState {
  FEED_IDLE,
  FEED_WAIT_ACK,
  FEED_RUNNING
};

FeedState feedState = FEED_IDLE;

bool s32kReady = false;
bool readyReported = false;

unsigned long feedTime = 0;
unsigned long lastFeedSendTime = 0;
uint8_t feedSendCount = 0;

const unsigned long FEED_ACK_TIMEOUT_MS  = 8000;
const unsigned long FEED_DONE_TIMEOUT_MS = 30000;
const unsigned long FEED_RESEND_MS       = 800;
const uint8_t FEED_MAX_SEND_COUNT        = 8;

/* ================== TDS STATE ================== */

bool waitingTDS = false;

unsigned long tdsTime = 0;
unsigned long lastTDSSendTime = 0;

const unsigned long TDS_TIMEOUT_MS = 8000;
const unsigned long TDS_RESEND_MS = 700;
const uint8_t TDS_MAX_SEND_COUNT = 6;

uint8_t tdsSendCount = 0;

/* ================== RECOVERY STATE ================== */

unsigned long recoveryUntil = 0;
const unsigned long RECOVERY_DELAY_MS = 2000;

/* ================== FUNCTION DECLARATIONS ================== */

void connectWiFi();
void MQTT_connect();

void sendToDiscord(String message);
String escapeJson(String input);
String sanitizeAscii(String input);

void handleAdafruitMessage(String message);
void handleSerialMonitorMessage(String message);

void requestFeed();
void requestTDS();

void feedTask();
void tdsTask();

void sendCommandToS32K(const String &cmd);
void readS32KLines();
void handleS32KLine(String line);
void clearS32KBuffer();

bool isFeedCommand(String message);
bool isTDSCommand(String message);

/* ================== SETUP ================== */

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(ESP32_RX2, INPUT_PULLUP);

  /*
   * This baudrate must match the S32K LPUART2 baudrate.
   */
  S32K_Serial.begin(9600, SERIAL_8N1, ESP32_RX2, ESP32_TX2);

  delay(500);
  clearS32KBuffer();

  Serial.println();
  Serial.println("--- ESP32 FISHTANK GATEWAY STARTED ---");

  discordClient.setInsecure();

  connectWiFi();

  mqtt.subscribe(&command_sub);

  Serial.println("Subscribed to Adafruit IO feed.");
  sendToDiscord("ESP32 Gateway online. Waiting for S32K.");
}

/* ================== LOOP ================== */

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  MQTT_connect();

  feedTask();
  tdsTask();

  /*
   * Serial Monitor test commands:
   * feed
   * tds
   * ping
   */
  if (Serial.available() > 0) {
    String serialMessage = Serial.readStringUntil('\n');
    serialMessage.trim();

    if (serialMessage.length() > 0) {
      handleSerialMonitorMessage(serialMessage);
    }
  }

  Adafruit_MQTT_Subscribe *subscription;

  while ((subscription = mqtt.readSubscription(10))) {
    if (subscription == &command_sub) {
      String message = (char *)command_sub.lastread;
      message.trim();

      if (message.length() > 0) {
        handleAdafruitMessage(message);
      }
    }
  }

  readS32KLines();
}

/* ================== WIFI ================== */

void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WLAN_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WLAN_SSID, WLAN_PASS);

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");

    if (millis() - startTime > 20000) {
      Serial.println();
      Serial.println("WiFi timeout. Retrying...");

      WiFi.disconnect();
      delay(1000);
      WiFi.begin(WLAN_SSID, WLAN_PASS);

      startTime = millis();
    }
  }

  Serial.println();
  Serial.println("WiFi connected.");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

/* ================== MQTT ================== */

void MQTT_connect() {
  if (mqtt.connected()) {
    return;
  }

  Serial.println("Connecting to Adafruit IO...");

  int8_t ret;

  while ((ret = mqtt.connect()) != 0) {
    Serial.print("MQTT connection failed. Error code: ");
    Serial.println(ret);

    mqtt.disconnect();

    Serial.println("Retrying MQTT in 2 seconds...");
    delay(2000);
  }

  Serial.println("Adafruit IO connected.");
}

/* ================== DISCORD ================== */

void sendToDiscord(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Cannot send Discord message.");
    return;
  }

  message = sanitizeAscii(message);

  HTTPClient https;

  if (!https.begin(discordClient, DISCORD_WEBHOOK_URL)) {
    Serial.println("Discord HTTPS begin failed.");
    return;
  }

  https.addHeader("Content-Type", "application/json");

  String jsonPayload = "{\"content\":\"" + escapeJson(message) + "\"}";

  int httpResponseCode = https.POST(jsonPayload);

  Serial.print("Discord response code: ");
  Serial.println(httpResponseCode);

  if (httpResponseCode < 200 || httpResponseCode >= 300) {
    Serial.println("Discord message failed.");
    Serial.println(https.getString());
  }

  https.end();
}

/* ================== COMMAND HANDLERS ================== */

void handleSerialMonitorMessage(String message) {
  message = sanitizeAscii(message);
  message.trim();

  Serial.print("[Serial Monitor]: ");
  Serial.println(message);

  if (isFeedCommand(message)) {
    requestFeed();
  }
  else if (isTDSCommand(message)) {
    requestTDS();
  }
  else if (message == "ping" || message == "P") {
    sendCommandToS32K("P");
  }
}

void handleAdafruitMessage(String message) {
  message = sanitizeAscii(message);
  message.trim();

  Serial.print("[Adafruit IO -> ESP32]: ");
  Serial.println(message);

  if (isFeedCommand(message)) {
    requestFeed();
  }
  else if (isTDSCommand(message)) {
    requestTDS();
  }
}

/* ================== FEED CONTROL ================== */

void requestFeed() {
  if (millis() < recoveryUntil) {
    sendToDiscord("System is recovering. Please wait a moment.");
    return;
  }

  if (!s32kReady) {
    sendToDiscord("S32K is not ready. Please wait.");
    return;
  }

  if (feedState != FEED_IDLE) {
    sendToDiscord("System is feeding. Please wait.");
    return;
  }

  if (waitingTDS) {
    sendToDiscord("TDS request is running. Please wait.");
    return;
  }

  clearS32KBuffer();

  feedState = FEED_WAIT_ACK;
  feedTime = millis();
  lastFeedSendTime = 0;
  feedSendCount = 0;

  /*
   * Do not send Discord message here.
   * This keeps ESP32 focused on UART acknowledgement.
   */
}

void feedTask() {
  if (feedState == FEED_IDLE) {
    return;
  }

  if (feedState == FEED_WAIT_ACK) {
    if ((millis() - feedTime) > FEED_ACK_TIMEOUT_MS) {
      feedState = FEED_IDLE;
      feedSendCount = 0;
      recoveryUntil = millis() + RECOVERY_DELAY_MS;
      clearS32KBuffer();

      sendToDiscord("Feed failed: S32K did not acknowledge the command. Please try again.");
      return;
    }

    if ((millis() - lastFeedSendTime) >= FEED_RESEND_MS) {
      if (feedSendCount < FEED_MAX_SEND_COUNT) {
        sendCommandToS32K("F");
        feedSendCount++;
        lastFeedSendTime = millis();
      }
    }
  }
  else if (feedState == FEED_RUNNING) {
    if ((millis() - feedTime) > FEED_DONE_TIMEOUT_MS) {
      feedState = FEED_IDLE;
      feedSendCount = 0;
      recoveryUntil = millis() + RECOVERY_DELAY_MS;
      clearS32KBuffer();

      sendToDiscord("Feed timeout: motor did not confirm completion. Please try again.");
      return;
    }
  }
}

/* ================== TDS CONTROL ================== */

void requestTDS() {
  if (millis() < recoveryUntil) {
    sendToDiscord("System is recovering. Please wait a moment.");
    return;
  }

  if (!s32kReady) {
    sendToDiscord("S32K is not ready. Please wait.");
    return;
  }

  if (feedState != FEED_IDLE) {
    sendToDiscord("System is feeding. Try TDS after feeding.");
    return;
  }

  if (waitingTDS) {
    sendToDiscord("TDS request is already running.");
    return;
  }

  clearS32KBuffer();

  waitingTDS = true;
  tdsTime = millis();
  lastTDSSendTime = 0;
  tdsSendCount = 0;

  /*
   * Do not send Discord message here.
   * The result will be sent when TDS is received.
   */
}

void tdsTask() {
  if (!waitingTDS) {
    return;
  }

  if ((millis() - tdsTime) > TDS_TIMEOUT_MS) {
    waitingTDS = false;
    tdsSendCount = 0;
    recoveryUntil = millis() + RECOVERY_DELAY_MS;
    clearS32KBuffer();

    sendToDiscord("TDS timeout. Please try again.");
    return;
  }

  if ((millis() - lastTDSSendTime) >= TDS_RESEND_MS) {
    if (tdsSendCount < TDS_MAX_SEND_COUNT) {
      sendCommandToS32K("T");
      tdsSendCount++;
      lastTDSSendTime = millis();
    }
  }
}

/* ================== COMMAND CHECK ================== */

bool isFeedCommand(String message) {
  message.toLowerCase();
  message.trim();

  if (message == "feed") return true;
  if (message == "!feed") return true;
  if (message == "f") return true;
  if (message == "on") return true;
  if (message == "1") return true;
  if (message == "light_on") return true;
  if (message == "cho an") return true;
  if (message == "cho ăn") return true;

  return false;
}

bool isTDSCommand(String message) {
  message.toLowerCase();
  message.trim();

  if (message == "tds") return true;
  if (message == "!tds") return true;
  if (message == "t") return true;
  if (message == "water") return true;
  if (message == "nuoc") return true;
  if (message == "nước") return true;

  return false;
}

/* ================== UART ================== */

void sendCommandToS32K(const String &cmd) {
  Serial.print("[ESP32 -> S32K144]: ");
  Serial.println(cmd);

  S32K_Serial.print(cmd);
  S32K_Serial.print('\n');
  S32K_Serial.flush();
}

void readS32KLines() {
  static String line = "";

  while (S32K_Serial.available() > 0) {
    char c = (char)S32K_Serial.read();

    if (c == '\n' || c == '\r') {
      line.trim();

      if (line.length() > 0) {
        handleS32KLine(line);
        line = "";
      }
    }
    else {
      line += c;

      if (line.length() > 160) {
        Serial.println("S32K line too long. Cleared.");
        line = "";
      }
    }
  }
}

void handleS32KLine(String line) {
  line = sanitizeAscii(line);
  line.trim();

  Serial.print("[S32K144 -> ESP32]: ");
  Serial.println(line);

  if (line == "READY") {
    s32kReady = true;

    if (!readyReported) {
      readyReported = true;
      sendToDiscord("S32K144 is ready.");
    }
  }
  else if (line == "PONG") {
    sendToDiscord("S32K UART OK.");
  }
  else if (line == "FEED_ACK") {
    feedState = FEED_RUNNING;
    feedTime = millis();
    feedSendCount = 0;
    sendToDiscord("Feeding started.");
  }
  else if (line == "FEED_DONE") {
    feedState = FEED_IDLE;
    feedSendCount = 0;
    sendToDiscord("Feeding completed.");
  }
  else if (line == "BUTTON_FEED_ACK") {
    sendToDiscord("Physical button feeding started.");
  }
  else if (line == "BUTTON_FEED_DONE") {
    sendToDiscord("Physical button feeding completed.");
  }
  else if (line == "TDS_ACK") {
    Serial.println("S32K acknowledged TDS request.");
  }
  else if (line.startsWith("BUSY:")) {
    feedState = FEED_IDLE;
    feedSendCount = 0;
    waitingTDS = false;
    tdsSendCount = 0;
    recoveryUntil = millis() + RECOVERY_DELAY_MS;
    clearS32KBuffer();

    sendToDiscord("S32K busy. Please wait.");
  }
  else if (line.startsWith("TDS:")) {
    waitingTDS = false;
    tdsSendCount = 0;

    int firstColon = line.indexOf(':');
    int secondColon = line.indexOf(':', firstColon + 1);

    if (firstColon >= 0 && secondColon > firstColon) {
      String value = line.substring(firstColon + 1, secondColon);
      String status = line.substring(secondColon + 1);

      sendToDiscord("Current TDS: " + value + " ppm | Status: " + status);
    }
    else {
      sendToDiscord("TDS report: " + line);
    }
  }
}

void clearS32KBuffer() {
  while (S32K_Serial.available() > 0) {
    S32K_Serial.read();
  }
}

/* ================== STRING HELPERS ================== */

String sanitizeAscii(String input) {
  String output = "";

  for (uint16_t i = 0; i < input.length(); i++) {
    char c = input[i];

    if (c == '\n' || c == '\r') {
      output += c;
    }
    else if (c >= 32 && c <= 126) {
      output += c;
    }
  }

  if (output.length() == 0) {
    output = "Empty message";
  }

  return output;
}

String escapeJson(String input) {
  String output = "";

  for (uint16_t i = 0; i < input.length(); i++) {
    char c = input[i];

    if (c == '\"') {
      output += "\\\"";
    }
    else if (c == '\\') {
      output += "\\\\";
    }
    else if (c == '\n') {
      output += "\\n";
    }
    else if (c == '\r') {
      output += "\\r";
    }
    else {
      output += c;
    }
  }

  return output;
}