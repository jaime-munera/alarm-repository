#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include "AudioGeneratorMP3.h"
#include "AudioFileSourceHTTPStream.h"
#include "AudioOutputI2S.h"

// --- Pines ---
#define RELE_PIN 17
#define CONTACT_PIN 13
#define I2S_LRC 18
#define I2S_BCK 19
#define I2S_DOUT 21

// --- Wi-Fi ---
const char *ssids[] = { "UBUNTU", "Tenda_5B0940" };
const char *passwords[] = { "Qweasd2022#", "Qweasd2025" };
bool isWiFiConnected = false;

// --- Telegram ---
const String BOT_TOKEN = "7609327599:AAGPAdBOG1Mx9q8QZ8ReIbVfkQO0pvYU-5o";
const String CHAT_ID = "8150043713";
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// --- Audios dinámicos ---
#include <vector>
std::vector<String> messages;  // arreglo dinámico de URLs
int current_message_index = 0;
AudioGeneratorMP3 mp3;
AudioFileSourceHTTPStream *fileSource = nullptr;
AudioOutputI2S i2s;

// --- Estado ---
unsigned long tiempoPuertaAbierta = 0;
bool alarmaActivaTelegram = false;
bool alarmaActivaAudio = false;
unsigned long bot_last_time = 0;
const int BOT_MTBS = 1000;
long lastUpdateId = 0;

// --- URL del JSON en Dropbox (Se asume que esta URL es correcta y directa) ---
const String JSON_URL =  "https://dl.dropboxusercontent.com/scl/fi/iz7jd6zqwk4mw447ytxep/audios.json?rlkey=n5035cbeihv9fprjga2hr9gyi&st=jyqss3xt&dl=1";


// --- Funciones ---
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  for (int i = 0; i < 2; i++) {
    WiFi.begin(ssids[i], passwords[i]);
    Serial.printf("🔌 Conectando a %s...\n", ssids[i]);
    for (int j = 0; j < 20; j++) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("✅ Conectado a %s\n", ssids[i]);
        isWiFiConnected = true;
        return;
      }
      delay(500);
    }
    WiFi.disconnect();
  }
  Serial.println("❌ No se pudo conectar a ninguna red");
}

/**
 * Función corregida para descargar el JSON directamente desde Dropbox Content Host.
 */
bool descargarJSON() {
  WiFiClientSecure httpsClient;
  httpsClient.setInsecure();
  
  // 🚨 CORRECCIÓN 1: Conectar al host de contenido directo, no al host de la web.
  const char* host = "dl.dropboxusercontent.com";
  if (!httpsClient.connect(host, 443)) {
    Serial.println("❌ Error conectando a Dropbox Content Host");
    return false;
  }

  // 🚨 CORRECCIÓN 2: Usar el Host de contenido directo en la cabecera HTTP.
  httpsClient.print(String("GET ") + JSON_URL + " HTTP/1.1\r\n" +
                    "Host: " + host + "\r\n" + 
                    "Connection: close\r\n\r\n");

  // Esperar la respuesta
  while (httpsClient.connected() && !httpsClient.available()) delay(100);
  
  String line;
  bool json_started = false;
  String jsonString = "";
  
  // Leer la respuesta, buscando la línea que empieza con '{' (el inicio del JSON)
  while (httpsClient.available()) {
    line = httpsClient.readStringUntil('\n');
    if (line.startsWith("{")) json_started = true;
    if (json_started) jsonString += line;
  }

  httpsClient.stop();

  if (jsonString.length() == 0) {
    Serial.println("❌ Se recibió una respuesta vacía o sin JSON.");
    return false;
  }

  // Parsear JSON
  DynamicJsonDocument doc(2048);
  auto error = deserializeJson(doc, jsonString);
  if (error) {
    Serial.printf("❌ Error parseando JSON: %s\n", error.c_str());
    return false;
  }

  messages.clear();
  for (JsonVariant v : doc["audios"].as<JsonArray>()) {
    messages.push_back(v.as<String>());
  }

  Serial.printf("✅ Se cargaron %d audios desde JSON\n", messages.size());
  return messages.size() > 0;
}

void enviarAlarmaTelegram() {
  bot.sendMessage(CHAT_ID, "⚠️ ¡ALARMA! La puerta ha estado ABIERTA por más de 15 segundos.", "Markdown");
  bot.sendMessage(CHAT_ID, "Escucha el sonido de la alarma: " + messages[current_message_index], "");
}

void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = bot.messages[i].chat_id;
    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    if (text == "/activar_porton" || text == "/cerrar_porton") {
      bot.sendMessage(chat_id, "Activando portón...", "");
      digitalWrite(RELE_PIN, LOW);
      delay(3000);
      digitalWrite(RELE_PIN, HIGH);
      bot.sendMessage(chat_id, "Portón activado.", "");
    } else if (text == "/estado_porton") {
      int estado = digitalRead(CONTACT_PIN);
      bot.sendMessage(chat_id, estado == LOW ? "Portón ABIERTO ⚠️" : "Portón CERRADO ✅", "");
    } else if (text == "/start" || text == "/help") {
      String msg = "Hola, " + from_name + ".\n";
      msg += "/estado_porton - Ver estado\n";
      msg += "/activar_porton - Activar mecanismo\n";
      bot.sendMessage(chat_id, msg, "");
    } else {
      bot.sendMessage(chat_id, "Comando desconocido. Usa /help.", "");
    }
  }
}

void reproducirAudio() {
  if (fileSource) {
    if (mp3.isRunning()) mp3.stop();
    delete fileSource;
    fileSource = nullptr;
  }

  if (messages.size() == 0) return;

  fileSource = new AudioFileSourceHTTPStream(messages[current_message_index].c_str());
  if (mp3.begin(fileSource, &i2s)) {
    Serial.printf("🎶 Reproduciendo: %s\n", messages[current_message_index].c_str());
    alarmaActivaAudio = true;
  } else {
    Serial.println("❌ Error al iniciar reproducción");
  }
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("🚀 Iniciando ESP32-S3 con audios dinámicos desde JSON...");

  pinMode(RELE_PIN, OUTPUT);
  pinMode(CONTACT_PIN, INPUT_PULLUP);
  digitalWrite(RELE_PIN, HIGH);

  i2s.SetPinout(I2S_BCK, I2S_LRC, I2S_DOUT);
  i2s.SetGain(0.8);

  conectarWiFi();
  if (isWiFiConnected) client.setInsecure();

  if (!descargarJSON()) {
    Serial.println("❌ No se pudieron cargar audios desde JSON");
  }
}

// --- Loop ---
void loop() {
  int estado = digitalRead(CONTACT_PIN);
  Serial.printf("📍 Estado del contacto: %s\n", estado == LOW ? "ABIERTO" : "CERRADO");

  if (estado == LOW) {
    if (tiempoPuertaAbierta == 0) tiempoPuertaAbierta = millis();

    if (millis() - tiempoPuertaAbierta > 15000) {
      if (isWiFiConnected && !alarmaActivaTelegram) {
        enviarAlarmaTelegram();
        alarmaActivaTelegram = true;
      }

      if (!alarmaActivaAudio) {
        reproducirAudio();
      }

      if (alarmaActivaAudio && mp3.isRunning()) {
        mp3.loop();
      }
    }
  } else {
    tiempoPuertaAbierta = 0;
    alarmaActivaTelegram = false;

    if (alarmaActivaAudio) {
      Serial.println("🔕 Deteniendo audio...");
      alarmaActivaAudio = false;
      mp3.stop();
      current_message_index = (current_message_index + 1) % messages.size();
    }
  }

  if (isWiFiConnected && millis() - bot_last_time > BOT_MTBS) {
    int numNewMessages = bot.getUpdates(lastUpdateId + 1);
    if (numNewMessages) {
      handleNewMessages(numNewMessages);
      lastUpdateId = bot.messages[numNewMessages - 1].update_id;
    }
    bot_last_time = millis();
  }

  delay(1000);
}
