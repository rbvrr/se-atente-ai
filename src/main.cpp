#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_APDS9960.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ==========================================
// CONFIGURAÇÕES DE REDE E FIREBASE
// ==========================================
#define WIFI_SSID "uaifai-brum"
#define WIFI_PASSWORD "bemvindoaocesar"

// Coloque a URL do seu Realtime Database (sem o fecho de barra / no final) e seu token
#define FIREBASE_URL "https://mpes-20261-default-rtdb.firebaseio.com/"
#define FIREBASE_TOKEN "TOKEN"

// ==========================================
// CONFIGURAÇÕES DO DISPLAY OLED E SENSOR I2C
// ==========================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_APDS9960 apds;

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

// ==========================================
// VARIÁVEIS DE ESTADO E TEMPO
// ==========================================
enum SystemState { SELECT_FOCUS_TYPE, CHECK_LIGHT, SETUP_FOCUS, SETUP_BREAK, RUN_FOCUS, RUN_BREAK, ASK_NOISE_ACTION, ALERT };
SystemState currentState = SELECT_FOCUS_TYPE;
SystemState lastState = SELECT_FOCUS_TYPE; // Lembra qual foi a última etapa antes do alerta

enum FocusType { EXERCISE, WORK, STUDY, MEDITATION };
FocusType currentFocusType = EXERCISE;

int focusTimeMin = 5;
int breakTimeMin = 1;
unsigned long timerStartMillis = 0;
unsigned long currentDurationMillis = 0;
unsigned long pausedRemainingMillis = 0;

int currentPomodoroCycle = 1; // Contador de 1 a 4 ciclos

bool buttonClickEvent = false;
bool buttonDoubleClickEvent = false;
bool ignoreMicForThisCycle = false;

// Variáveis de métricas da sessão
int leaveCount = 0;
bool isUserPresent = true;
uint16_t lastLuminosity = 0;
bool noiseDetectedThisSession = false;

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
bool isTooLoud();
void connectWiFi();
void sendSessionDataToFirebase();

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

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
    Serial.println(F("Falha ao alocar SSD1306"));
    for (;;);
  }

  display.clearDisplay();
  display.setTextColor(WHITE);

  if(!apds.begin()){
    Serial.println("Falha ao inicializar APDS9960!");
  } else {
    apds.enableColor(true);
    apds.enableProximity(true);
  }

  connectWiFi();
}

// ==========================================
// LOOP PRINCIPAL
// ==========================================
void loop() {
  processButton();

  switch (currentState) {
    case SELECT_FOCUS_TYPE:
      currentFocusType = select_focus_type();

      if (currentFocusType == STUDY) currentState = CHECK_LIGHT;
      else currentState = SETUP_FOCUS;

      delay(300);
      break;

    case CHECK_LIGHT: {
      uint16_t r, g, b;
      while(!apds.colorDataReady()) { delay(5); }
      apds.getColorData(&r, &g, &b, &lastLuminosity);

      if (lastLuminosity < LIGHT_THRESHOLD) {
        if ((millis() / 250) % 2 == 0) setRGB(255, 100, 0);
        else setRGB(0, 0, 0);

        updateDisplay("AVISO DE LUZ", "MUITO\nESCURO!", 2);

        if (isButtonPressed() || isButtonDoubleClicked()) {
          setRGB(0, 0, 0);
          currentState = SETUP_FOCUS;
          delay(300);
        }
      } else {
        setRGB(0, 0, 0);
        currentState = SETUP_FOCUS;
      }
      break;
    }

    case SETUP_FOCUS:
      focusTimeMin = configure_timer(1);
      currentState = SETUP_BREAK;
      delay(300);
      break;

    case SETUP_BREAK:
      breakTimeMin = configure_timer(2);

      // Início do ciclo completo de Pomodoro
      currentPomodoroCycle = 1;
      currentState = RUN_FOCUS;
      ignoreMicForThisCycle = false;
      leaveCount = 0;
      isUserPresent = true;
      noiseDetectedThisSession = false;

      if (currentFocusType == MEDITATION) {
         uint16_t r, g, b;
         if (apds.colorDataReady()) apds.getColorData(&r, &g, &b, &lastLuminosity);
      }

      timerStartMillis = millis();
      currentDurationMillis = (unsigned long)focusTimeMin * 60000ULL;
      setRGB(255, 0, 0);
      delay(300);
      break;

    case RUN_FOCUS:
    case RUN_BREAK: {
      buttonClickEvent = false;
      buttonDoubleClickEvent = false;

      unsigned long elapsed = millis() - timerStartMillis;
      unsigned long remaining = (elapsed < currentDurationMillis) ? (currentDurationMillis - elapsed) : 0;

      if (currentState == RUN_FOCUS && (currentFocusType == STUDY || currentFocusType == MEDITATION)) {
        if (!ignoreMicForThisCycle && isTooLoud()) {
          noiseDetectedThisSession = true;
          pausedRemainingMillis = remaining;
          currentState = ASK_NOISE_ACTION;
          setRGB(0, 0, 0);
          break;
        }

        uint8_t proximity = apds.readProximity();
        if (proximity < PROX_THRESHOLD && isUserPresent) {
          isUserPresent = false;
          leaveCount++;
        }
        else if (proximity >= PROX_THRESHOLD && !isUserPresent) {
          isUserPresent = true;
        }
      }

      float progress = 1.0 - ((float)elapsed / (float)currentDurationMillis);
      updateProgressLEDs(progress);

      int mins = remaining / 60000;
      int secs = (remaining % 60000) / 1000;
      char timeBuf[6];
      sprintf(timeBuf, "%02d:%02d", mins, secs);

      // Título inteligente: mostra o ciclo e (se houver) saídas
      String title = "";
      if (currentState == RUN_FOCUS) {
        title = "FOCO " + String(currentPomodoroCycle) + "/4";
        if (currentFocusType == STUDY || currentFocusType == MEDITATION) {
          title += " S:" + String(leaveCount); // 'S:' abrevia Saídas para caber na tela
        }
      } else {
        title = "PAUSA " + String(currentPomodoroCycle) + "/4";
      }

      updateDisplay(title, String(timeBuf), 3);

      if (remaining == 0) {
        lastState = currentState; // Guarda se acabou de sair do FOCO ou da PAUSA

        if (currentState == RUN_FOCUS) {
          updateDisplay("SALVANDO...", "Enviando\nNuvem", 2);
          sendSessionDataToFirebase();
        }

        currentState = ALERT;
        updateProgressLEDs(0.0);
      }
      break;
    }

    case ASK_NOISE_ACTION:
      if ((millis() / 250) % 2 == 0) setRGB(255, 255, 0);
      else setRGB(0, 0, 0);

      updateDisplay("RUIDO! Continuar?", "1x:Nao\n2x:Sim", 2);

      if (isButtonDoubleClicked()) {
        ignoreMicForThisCycle = true;
        currentState = RUN_FOCUS;
        currentDurationMillis = pausedRemainingMillis;
        timerStartMillis = millis();
        setRGB(255, 0, 0);
        delay(300);
      }
      else if (isButtonPressed()) {
        currentState = SELECT_FOCUS_TYPE;
        setRGB(0, 0, 0);
      }
      break;

    case ALERT:
      if ((millis() / 500) % 2 == 0) setRGB(0, 0, 255);
      else setRGB(0, 0, 0);

      if (lastState == RUN_FOCUS && (currentFocusType == STUDY || currentFocusType == MEDITATION)) {
        updateDisplay("ESGOTADO!", "Saidas: " + String(leaveCount), 2);
      } else {
        updateDisplay("TEMPO", "ESGOTADO!", 2);
      }

      // Máquina de transição inteligente (4 ciclos)
      if (isButtonPressed() || isButtonDoubleClicked()) {
        if (lastState == RUN_FOCUS) {
          // Se acabou o foco, vai pra pausa obrigatoriamente
          currentState = RUN_BREAK;
          timerStartMillis = millis();
          currentDurationMillis = (unsigned long)breakTimeMin * 60000ULL;
          setRGB(0, 255, 0);
        } else {
          // Se acabou a pausa, verifica se já foram 4 ciclos
          if (currentPomodoroCycle < 4) {
            currentPomodoroCycle++;

            // Reseta variáveis pro novo foco
            currentState = RUN_FOCUS;
            ignoreMicForThisCycle = false;
            leaveCount = 0;
            isUserPresent = true;
            noiseDetectedThisSession = false;

            timerStartMillis = millis();
            currentDurationMillis = (unsigned long)focusTimeMin * 60000ULL;
            setRGB(255, 0, 0);
          } else {
            // Se fechou 4 ciclos (Pomodoro Completo), volta pro menu principal
            currentState = SELECT_FOCUS_TYPE;
            setRGB(0, 0, 0);
          }
        }
      }
      break;
  }
}

// ==========================================
// FUNÇÕES DE COMUNICAÇÃO DE REDE
// ==========================================
void connectWiFi() {
  updateDisplay("WIFI", "Conectando...", 2);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    updateDisplay("WIFI", "Conectado!", 2);
  } else {
    updateDisplay("WIFI", "Falhou! :(", 2);
  }
  delay(1500);
}

void sendSessionDataToFirebase() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    String url = String(FIREBASE_URL) + "/sessoes.json?auth=" + String(FIREBASE_TOKEN);

    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    String focusName = "";
    if (currentFocusType == EXERCISE) focusName = "Exercicio";
    else if (currentFocusType == WORK) focusName = "Trabalho";
    else if (currentFocusType == STUDY) focusName = "Estudo";
    else if (currentFocusType == MEDITATION) focusName = "Meditacao";

    String payload = "{";
    payload += "\"tipo_foco\":\"" + focusName + "\",";
    payload += "\"tempo_minutos\":" + String(focusTimeMin) + ",";
    payload += "\"ciclo\":" + String(currentPomodoroCycle) + ","; // Número do ciclo de 1 a 4 adicionado!

    if (currentFocusType == STUDY || currentFocusType == MEDITATION) {
      payload += "\"saidas\":" + String(leaveCount) + ",";
      payload += "\"luminosidade_inicial\":" + String(lastLuminosity) + ",";
      payload += "\"teve_ruido\":" + String(noiseDetectedThisSession ? "true" : "false");
    } else {
      payload += "\"saidas\":0,";
      payload += "\"luminosidade_inicial\":0,";
      payload += "\"teve_ruido\":false";
    }
    payload += "}";

    int httpResponseCode = http.POST(payload);

    Serial.print("HTTP Code: ");
    Serial.println(httpResponseCode);
    Serial.println(payload);

    http.end();
  } else {
    Serial.println("Erro: Wi-Fi desconectado, impossível enviar.");
  }
}

// ==========================================
// FUNÇÕES DE MENUS E HARDWARE
// ==========================================
FocusType select_focus_type() {
  bool confirmed = false;
  int selectedIndex = 0;
  String typeNames[] = {"Exercicio", "Trabalho", "Estudo", "Meditacao"};

  buttonClickEvent = false;
  buttonDoubleClickEvent = false;

  while (!confirmed) {
    processButton();
    int sliderVal = analogRead(SLIDER_PIN);
    selectedIndex = map(sliderVal, 0, 4095, 0, 3);

    if (selectedIndex < 0) selectedIndex = 0;
    if (selectedIndex > 3) selectedIndex = 3;

    updateDisplay("Tipo de Foco", typeNames[selectedIndex], 2);

    if (isButtonPressed() || isButtonDoubleClicked()) {
      confirmed = true;
    }
    delay(50);
  }
  return (FocusType)selectedIndex;
}

int configure_timer(int mode) {
  bool confirmed = false;
  int selectedTime = 0;

  buttonClickEvent = false;
  buttonDoubleClickEvent = false;

  while (!confirmed) {
    processButton();
    int sliderVal = analogRead(SLIDER_PIN);

    if (mode == 1) {
      selectedTime = map(sliderVal, 0, 4095, 5, 60);
      updateDisplay("Set Foco", String(selectedTime) + " min", 2);
    } else {
      selectedTime = map(sliderVal, 0, 4095, 1, 20);
      updateDisplay("Set Pausa", String(selectedTime) + " min", 2);
    }

    if (isButtonPressed() || isButtonDoubleClicked()) {
      confirmed = true;
    }
    delay(50);
  }
  return selectedTime;
}

void processButton() {
  static unsigned long buttonPressStartTime = 0;
  static unsigned long lastReleaseTime = 0;
  static bool isButtonHolding = false;
  static bool longPressTriggered = false;
  static int clickCount = 0;
  static bool waitingForDoubleClick = false;

  bool isPressedNow = (digitalRead(BTN_PIN) == LOW);

  if (isPressedNow && !isButtonHolding) {
    delay(20);
    if (digitalRead(BTN_PIN) == LOW) {
      isButtonHolding = true;
      buttonPressStartTime = millis();
      longPressTriggered = false;
    }
  }
  else if (!isPressedNow && isButtonHolding) {
    isButtonHolding = false;
    unsigned long holdTime = millis() - buttonPressStartTime;

    if (!longPressTriggered && holdTime > 20) {
      clickCount++;
      lastReleaseTime = millis();
      waitingForDoubleClick = true;
    }
  }

  if (waitingForDoubleClick && !isButtonHolding) {
    if (millis() - lastReleaseTime > 300) {
      if (clickCount == 1) buttonClickEvent = true;
      else if (clickCount >= 2) buttonDoubleClickEvent = true;

      clickCount = 0;
      waitingForDoubleClick = false;
    }
  }

  if (isButtonHolding && !longPressTriggered) {
    if (millis() - buttonPressStartTime >= 10000) {
      longPressTriggered = true;
      clickCount = 0;
      waitingForDoubleClick = false;

      display.clearDisplay();
      display.setTextSize(2);
      display.setCursor(0, 15);
      display.println("RESETANDO");
      display.println("SISTEMA...");
      display.display();

      setRGB(0, 0, 0);
      for (int i = 0; i < 4; i++) digitalWrite(LEDS[i], LOW);

      delay(2000);
      ESP.restart();
    }
  }
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

bool isTooLoud() {
  int maxVal = 0;
  for(int i = 0; i < 150; i++) {
    int val = analogRead(MIC_PIN);
    if(val > maxVal) maxVal = val;
  }
  return (maxVal > NOISE_THRESHOLD);
}

void updateProgressLEDs(float progress) {
  int ledsOn = round(progress * 4.0);
  for (int i = 0; i < 4; i++) {
    if (i < ledsOn) digitalWrite(LEDS[i], HIGH);
    else digitalWrite(LEDS[i], LOW);
  }
}

void setRGB(int r, int g, int b) {
  analogWrite(RGB_R, r);
  analogWrite(RGB_G, g);
  analogWrite(RGB_B, b);
}

void updateDisplay(String title, String mainText, int textSize) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(title);
  display.setTextSize(textSize);
  display.setCursor(0, 25);
  display.println(mainText);
  display.display();
}
