#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_APDS9960.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

// ==========================================
// CONFIGURAÇÕES DE REDE E FIREBASE
// ==========================================
#define WIFI_SSID "uaifai-brum"
#define WIFI_PASSWORD "bemvindoaocesar"

#define FIREBASE_URL "https://mpes-20261-default-rtdb.firebaseio.com/"
#define FIREBASE_TOKEN "YOUR KEY"

// ==========================================
// MAPEAMENTO DE PINOS E LIMITES
// ==========================================
const int LEDS[] = {17, 16, 4, 13};
const int RGB_R = 19;
const int RGB_G = 23;
const int RGB_B = 18;
const int BTN_PIN = 27;
const int SLIDER_PIN = 39;
const int MIC_PIN = 36;

#define NOISE_THRESHOLD 2500
#define LIGHT_THRESHOLD 50
#define PROX_THRESHOLD 3

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_APDS9960 apds;
WebSocketsServer webSocket = WebSocketsServer(81);

// ==========================================
// VARIÁVEIS DE ESTADO E TEMPO
// ==========================================
enum SystemState { SELECT_FOCUS_TYPE, CHECK_LIGHT, SETUP_FOCUS, SETUP_BREAK, RUN_FOCUS, RUN_BREAK, ASK_NOISE_ACTION, ALERT };
SystemState currentState = SELECT_FOCUS_TYPE;
SystemState lastState = SELECT_FOCUS_TYPE;

enum FocusType { EXERCISE, WORK, STUDY, MEDITATION };
FocusType currentFocusType = EXERCISE;

int focusTimeMin = 5;
int breakTimeMin = 1;
unsigned long timerStartMillis = 0;
unsigned long currentDurationMillis = 0;
unsigned long pausedRemainingMillis = 0;

int currentPomodoroCycle = 1;
String sessionId = "";

bool buttonClickEvent = false;
bool buttonDoubleClickEvent = false;
bool ignoreMicForThisCycle = false;

int leaveCount = 0;
bool isUserPresent = true;
uint16_t lastLuminosity = 0;
bool noiseDetectedThisSession = false;
bool remoteOverride = false;

// Variáveis para mic não bloqueante
int micMax = 0;
unsigned long lastMicSample = 0;

// ==========================================
// DECLARAÇÃO DE FUNÇÕES
// ==========================================
FocusType select_focus_type();
int configure_timer(int mode);
void updateProgressLEDs(float progress);
void setRGB(int r, int g, int b);
void updateDisplay(String title, String timeStr, int textSize = 3);
void processButton();
bool isButtonPressed();
bool isButtonDoubleClicked();
void checkMic();
bool wasTooLoud();
void connectWiFi();
void sendSessionDataToFirebase();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);
void handleCommand(String json);
void broadcastStatus();

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(9600);
  delay(500);
  Serial.println("\n[SISTEMA] Iniciando...");

  for (int i = 0; i < 4; i++) {
    pinMode(LEDS[i], OUTPUT);
    digitalWrite(LEDS[i], LOW);
  }

  pinMode(RGB_R, OUTPUT);
  pinMode(RGB_G, OUTPUT);
  pinMode(RGB_B, OUTPUT);
  setRGB(0, 0, 0);

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(SLIDER_PIN, INPUT);
  pinMode(MIC_PIN, INPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("[ERRO] Falha SSD1306"));
  } else {
    display.clearDisplay();
    display.setTextColor(WHITE);
    updateDisplay("POMODORO", "Pronto!", 2);
  }

  if(!apds.begin()) Serial.println("[AVISO] APDS9960 ausente");
  else { apds.enableColor(true); apds.enableProximity(true); }

  connectWiFi();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.print("[SERVER] IP: "); Serial.println(WiFi.localIP());
}

// ==========================================
// LOOP PRINCIPAL
// ==========================================
void loop() {
  static unsigned long lastBroadcast = 0;
  static unsigned long stateEntryMillis = 0;
  static SystemState lastKnownState = (SystemState)-1;

  if (currentState != lastKnownState) {
    stateEntryMillis = millis();
    lastKnownState = currentState;
  }

  webSocket.loop();
  processButton();
  checkMic();

  if (WiFi.status() != WL_CONNECTED && (millis() % 15000 < 50)) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  if (millis() - lastBroadcast > 1500) {
    broadcastStatus();
    lastBroadcast = millis();
  }

  switch (currentState) {
    case SELECT_FOCUS_TYPE:
      currentFocusType = select_focus_type();
      if (currentState == SELECT_FOCUS_TYPE) {
        if (currentFocusType == STUDY) currentState = CHECK_LIGHT;
        else currentState = SETUP_FOCUS;
      }
      broadcastStatus();
      break;

    case CHECK_LIGHT: {
      uint16_t r, g, b;
      if (apds.colorDataReady()) {
        apds.getColorData(&r, &g, &b, &lastLuminosity);
        if (lastLuminosity < LIGHT_THRESHOLD && lastLuminosity > 0) {
          if ((millis() / 250) % 2 == 0) setRGB(255, 100, 0);
          else setRGB(0, 0, 0);
          updateDisplay("LUZ BAIXA", "Botao p/ ok", 2);
          if (isButtonPressed() || isButtonDoubleClicked()) {
            setRGB(0, 0, 0);
            currentState = SETUP_FOCUS;
            broadcastStatus();
            delay(300);
          }
        } else {
          setRGB(0, 0, 0);
          currentState = SETUP_FOCUS;
          broadcastStatus();
        }
      } else if (millis() - stateEntryMillis > 500) {
        // Timeout do sensor, pula para setup
        currentState = SETUP_FOCUS;
        broadcastStatus();
      }
      break;
    }

    case SETUP_FOCUS:
      focusTimeMin = configure_timer(1);
      if (currentState == SETUP_FOCUS) currentState = SETUP_BREAK;
      broadcastStatus();
      break;

    case SETUP_BREAK:
      breakTimeMin = configure_timer(2);
      if (currentState == SETUP_BREAK) {
        currentPomodoroCycle = 1;
        // Geração de SessionID mais robusta
        char sid[32];
        snprintf(sid, sizeof(sid), "sess_%08lx%04x", (unsigned long)time(NULL), (unsigned int)random(0xFFFF));
        sessionId = String(sid);

        currentState = RUN_FOCUS;
        ignoreMicForThisCycle = false;
        leaveCount = 0;
        isUserPresent = true;
        noiseDetectedThisSession = false;
        timerStartMillis = millis();
        currentDurationMillis = (unsigned long)focusTimeMin * 60000ULL;
        setRGB(255, 0, 0);
      }
      broadcastStatus();
      break;

    case RUN_FOCUS:
    case RUN_BREAK: {
      buttonClickEvent = false;
      buttonDoubleClickEvent = false;
      unsigned long elapsed = millis() - timerStartMillis;
      unsigned long remaining = (elapsed < currentDurationMillis) ? (currentDurationMillis - elapsed) : 0;

      if (currentState == RUN_FOCUS && (currentFocusType == STUDY || currentFocusType == MEDITATION)) {
        if (!ignoreMicForThisCycle && wasTooLoud()) {
          noiseDetectedThisSession = true;
          pausedRemainingMillis = remaining;
          currentState = ASK_NOISE_ACTION;
          setRGB(0, 0, 0);
          broadcastStatus();
          break;
        }
        uint8_t proximity = apds.readProximity();
        if (proximity < PROX_THRESHOLD && isUserPresent) {
          isUserPresent = false;
          leaveCount++;
        } else if (proximity >= PROX_THRESHOLD && !isUserPresent) {
          isUserPresent = true;
        }
      }

      float progress = 1.0 - ((float)elapsed / (float)currentDurationMillis);
      updateProgressLEDs(progress);

      int mins = remaining / 60000;
      int secs = (remaining % 60000) / 1000;
      char timeBuf[6];
      sprintf(timeBuf, "%02d:%02d", mins, secs);

      String title = (currentState == RUN_FOCUS) ? ">>> FOCO " : "--- PAUSA ";
      title += String(currentPomodoroCycle) + "/4";
      updateDisplay(title, String(timeBuf), 3);

      if (remaining == 0) {
        lastState = currentState;
        if (currentState == RUN_FOCUS) {
          updateDisplay("FINALIZADO", "Salvando...", 2);
          sendSessionDataToFirebase();
          currentState = ALERT;
        } else {
          if (currentPomodoroCycle < 4) {
            currentPomodoroCycle++;
            currentState = RUN_FOCUS;
            timerStartMillis = millis();
            currentDurationMillis = (unsigned long)focusTimeMin * 60000ULL;
            setRGB(255, 0, 0);
          } else {
            currentState = ALERT;
          }
        }
        broadcastStatus();
        updateProgressLEDs(0.0);
      }
      break;
    }

    case ASK_NOISE_ACTION:
      if ((millis() / 250) % 2 == 0) setRGB(255, 255, 0);
      else setRGB(0, 0, 0);
      updateDisplay("RUIDO!", "2x:Sim 1x:Sair", 2);
      if (isButtonDoubleClicked()) {
        ignoreMicForThisCycle = true;
        currentState = RUN_FOCUS;
        currentDurationMillis = pausedRemainingMillis;
        timerStartMillis = millis();
        setRGB(255, 0, 0);
        broadcastStatus();
        delay(300);
      } else if (isButtonPressed()) {
        currentState = SELECT_FOCUS_TYPE;
        setRGB(0, 0, 0);
        broadcastStatus();
      }
      break;

    case ALERT:
      if ((millis() / 500) % 2 == 0) setRGB(0, 0, 255);
      else setRGB(0, 0, 0);
      updateDisplay("TEMPO", "ESGOTADO!", 2);
      if (isButtonPressed() || isButtonDoubleClicked()) {
        if (lastState == RUN_FOCUS) {
          currentState = RUN_BREAK;
          timerStartMillis = millis();
          currentDurationMillis = (unsigned long)breakTimeMin * 60000ULL;
          setRGB(0, 255, 0);
        } else {
          currentState = SELECT_FOCUS_TYPE;
          setRGB(0, 0, 0);
        }
        broadcastStatus();
      }
      break;
  }
}

// ==========================================
// FUNÇÕES DE REDE
// ==========================================
void connectWiFi() {
  updateDisplay("WIFI", "Conectando", 2);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int att = 0;
  while (WiFi.status() != WL_CONNECTED && att < 20) {
    delay(500);
    att++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    updateDisplay("WIFI", "OK!", 2);
    WiFi.setAutoReconnect(true);
  } else updateDisplay("WIFI", "ERRO!", 2);
}

void sendSessionDataToFirebase() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "%s/sessoes.json?auth=%s", FIREBASE_URL, FIREBASE_TOKEN);
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    const char* names[] = {"Exercicio", "Trabalho", "Estudo", "Meditacao"};
    JsonDocument doc;
    doc["tipo"] = names[currentFocusType];
    doc["minutos"] = focusTimeMin;
    doc["ts"] = millis();
    doc["sessionId"] = sessionId;
    doc["ciclo"] = currentPomodoroCycle;
    doc["interrupcoes"] = leaveCount;
    doc["ruido"] = noiseDetectedThisSession;

    String payload;
    serializeJson(doc, payload);
    http.POST(payload);
    http.end();
  }
}

void broadcastStatus() {
  JsonDocument doc;
  doc["event"] = "status";
  const char* states[] = {"idle", "idle", "idle", "idle", "focus", "break", "paused", "alert"};
  doc["phase"] = states[currentState];
  const char* modes[] = {"exercise", "work", "study", "meditation"};
  doc["mode"] = modes[currentFocusType];
  unsigned long rem = 0;
  if (currentState == RUN_FOCUS || currentState == RUN_BREAK) {
    unsigned long elapsed = millis() - timerStartMillis;
    rem = (elapsed < currentDurationMillis) ? (currentDurationMillis - elapsed) : 0;
  } else if (currentState == ASK_NOISE_ACTION) rem = pausedRemainingMillis;
  else rem = (unsigned long)focusTimeMin * 60000ULL;
  doc["rem"] = rem / 1000;
  doc["cycle"] = currentPomodoroCycle;
  doc["focusMins"] = focusTimeMin;
  doc["breakMins"] = breakTimeMin;
  String output;
  serializeJson(doc, output);
  webSocket.broadcastTXT(output);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if (type == WStype_CONNECTED) broadcastStatus();
  else if (type == WStype_TEXT) handleCommand(String((const char*)payload, length));
}

void handleCommand(String json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  String cmd = doc["cmd"];
  if (cmd == "hello") broadcastStatus();
  else if (cmd == "config") {
    if (doc["mode"].is<String>()) {
      String m = doc["mode"];
      if (m == "study") currentFocusType = STUDY;
      else if (m == "work") currentFocusType = WORK;
      else if (m == "meditation") currentFocusType = MEDITATION;
      else if (m == "exercise") currentFocusType = EXERCISE;
      remoteOverride = true;
    }
    if (doc["focusMins"].is<int>()) {
      int val = doc["focusMins"];
      if (val >= 2) { focusTimeMin = val; remoteOverride = true; }
    }
    if (doc["breakMins"].is<int>()) {
      int val = doc["breakMins"];
      if (val >= 1) { breakTimeMin = val; remoteOverride = true; }
    }
    broadcastStatus();
  } else if (cmd == "start") {
    if (doc["duration"].is<int>()) {
      int d = doc["duration"].as<int>() / 60;
      if (d >= 2) focusTimeMin = d;
    }
    currentState = RUN_FOCUS;
    timerStartMillis = millis();
    currentDurationMillis = (unsigned long)focusTimeMin * 60000ULL;
    setRGB(255, 0, 0);
    broadcastStatus();
  } else if (cmd == "break") {
    if (doc["duration"].is<int>()) {
      int d = doc["duration"].as<int>() / 60;
      if (d >= 1) breakTimeMin = d;
    }
    currentState = RUN_BREAK;
    timerStartMillis = millis();
    currentDurationMillis = (unsigned long)breakTimeMin * 60000ULL;
    setRGB(0, 255, 0);
    broadcastStatus();
  } else if (cmd == "pause") {
    if (currentState == RUN_FOCUS || currentState == RUN_BREAK) {
      pausedRemainingMillis = currentDurationMillis - (millis() - timerStartMillis);
      lastState = currentState;
      currentState = ASK_NOISE_ACTION;
      setRGB(0, 0, 0);
      broadcastStatus();
    }
  } else if (cmd == "stop") {
    currentState = SELECT_FOCUS_TYPE;
    setRGB(0, 0, 0);
    broadcastStatus();
  }
}

FocusType select_focus_type() {
  int sel = (int)currentFocusType;
  int lastS = -1;
  while (true) {
    webSocket.loop();
    processButton();
    checkMic();
    int val = analogRead(SLIDER_PIN);
    if (abs(val - lastS) > 200) {
      sel = map(val, 0, 4095, 0, 3);
      lastS = val;
      remoteOverride = false;
    }
    if (remoteOverride) sel = (int)currentFocusType;
    const char* names[] = {"Exercicio", "Trabalho", "Estudo", "Meditacao"};
    updateDisplay("FOCO", names[sel], 2);
    if (isButtonPressed() || isButtonDoubleClicked() || currentState != SELECT_FOCUS_TYPE) break;
    delay(20);
  }
  remoteOverride = false;
  return (FocusType)sel;
}

int configure_timer(int mode) {
  int sel = (mode == 1) ? focusTimeMin : breakTimeMin;
  int lastS = -1;
  int minVal = (mode == 1) ? 2 : 1;
  int maxVal = (mode == 1) ? 60 : 20;

  while (true) {
    webSocket.loop();
    processButton();
    checkMic();
    int val = analogRead(SLIDER_PIN);
    if (abs(val - lastS) > 200) {
      sel = map(val, 0, 4095, minVal, maxVal);
      lastS = val;
      remoteOverride = false;
    }
    if (remoteOverride) sel = (mode == 1) ? focusTimeMin : breakTimeMin;
    updateDisplay(mode == 1 ? "SET FOCO" : "SET PAUSA", String(sel) + " min", 2);
    if (isButtonPressed() || isButtonDoubleClicked() || (mode == 1 && currentState != SETUP_FOCUS) || (mode == 2 && currentState != SETUP_BREAK)) break;
    delay(20);
  }
  remoteOverride = false;
  return sel;
}

void processButton() {
  static unsigned long start = 0;
  static unsigned long lastR = 0;
  static bool hold = false;
  static int clicks = 0;
  static bool waitD = false;
  bool now = (digitalRead(BTN_PIN) == LOW);
  if (now && !hold) {
    delay(20);
    if (digitalRead(BTN_PIN) == LOW) {
      hold = true;
      start = millis();
    }
  } else if (!now && hold) {
    hold = false;
    unsigned long dur = millis() - start;
    if (dur > 20) {
      clicks++;
      lastR = millis();
      waitD = true;
    }
  }
  if (waitD && !hold && (millis() - lastR > 300)) {
    if (clicks == 1) {
      buttonClickEvent = true;
      webSocket.broadcastTXT("{\"event\":\"btn\",\"type\":\"click\"}");
    } else if (clicks >= 2) {
      buttonDoubleClickEvent = true;
      webSocket.broadcastTXT("{\"event\":\"btn\",\"type\":\"double_click\"}");
    }
    clicks = 0;
    waitD = false;
  }
  if (hold && (millis() - start >= 10000)) ESP.restart();
}

bool isButtonPressed() {
  if (buttonClickEvent) {
    buttonClickEvent = false;
    return true;
  }
  return false;
}

bool isButtonDoubleClicked() {
  if (buttonDoubleClickEvent) {
    buttonDoubleClickEvent = false;
    return true;
  }
  return false;
}

void checkMic() {
  if (millis() - lastMicSample > 2) { // 500Hz sampling
    int v = analogRead(MIC_PIN);
    if (v > micMax) micMax = v;
    lastMicSample = millis();
  }
}

bool wasTooLoud() {
  bool loud = (micMax > NOISE_THRESHOLD);
  micMax = 0; // Reset after checking
  return loud;
}

void updateProgressLEDs(float p) {
  int n = round(p * 4.0);
  for (int i = 0; i < 4; i++) digitalWrite(LEDS[i], (i < n) ? HIGH : LOW);
}

void setRGB(int r, int g, int b) {
  analogWrite(RGB_R, r);
  analogWrite(RGB_G, g);
  analogWrite(RGB_B, b);
}

void updateDisplay(String t, String m, int s) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(t);
  display.setTextSize(s);
  display.setCursor(0, 25);
  display.println(m);
  display.display();
}
