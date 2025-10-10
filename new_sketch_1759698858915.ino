

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <HTTPClient.h> // Cliente HTTP Nativo
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <AudioFileSource.h>     // 🚨 Inclusión para resolver dependencias


// --- Pines ---
#define RELE_PIN 17
#define CONTACT_PIN 13
#define I2S_LRC 18
#define I2S_BCK 19
#define I2S_DOUT 21

// --- Wi-Fi ---
const char *ssids[] = { "UBUNTU", "Tenda_5B0940" };
const char *passwords[] = { "-------", "-------" };
bool isWiFiConnected = false;

// --- Telegram ---
const String BOT_TOKEN = "--------";
const String CHAT_ID = "----------";
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

// --- Audios dinámicos ---
#include <vector>
std::vector<String> messages; // arreglo dinámico de URLs
int current_message_index = 0;
AudioGeneratorMP3 mp3;
AudioFileSourceStream *fileSource = nullptr; // ⚠️ TIPO CORRECTO
AudioOutputI2S i2s;

// --- Variables para el nuevo sistema de audio ---
WiFiClientSecure audioClient; 
HTTPClient http;

// --- Estado ---
unsigned long tiempoPuertaAbierta = 0;
bool alarmaActivaTelegram = false;
bool alarmaActivaAudio = false;
unsigned long bot_last_time = 0;
const int BOT_MTBS = 1000;
long lastUpdateId = 0;

// --- URL del JSON en GitHub Pages ---
const String JSON_URL = "https://jaime-munera.github.io/alarm-repository/audios.json";


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
 * Función para descargar el JSON desde GitHub Pages.
 */
bool descargarJSON() {
    WiFiClientSecure httpsClient;
    httpsClient.setInsecure();
    
    const char* host = "jaime-munera.github.io";
    
    if (!httpsClient.connect(host, 443)) {
        Serial.println("❌ Error conectando a GitHub Pages Host");
        return false;
    }

    httpsClient.print(String("GET ") + JSON_URL + " HTTP/1.1\r\n" +
                        "Host: " + host + "\r\n" +
                        "Connection: close\r\n\r\n");

    // Esperar la respuesta
    while (httpsClient.connected() && !httpsClient.available()) delay(100);
    
    String line;
    bool json_started = false;
    String jsonString = "";
    
    // Leer la respuesta, buscando la línea que empieza con '{'
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

/**
 * Función de reproducción REEMPLAZADA: Usa HTTPClient + AudioFileSourceStream para mayor control SSL.
 */
void reproducirAudio() {
    // 1. Limpieza de la reproducción anterior
    if (fileSource) {
        if (mp3.isRunning()) mp3.stop();
        delete fileSource;
        fileSource = nullptr;
    }

    if (messages.size() == 0) return;

    String urlToPlay = messages[current_message_index];
    Serial.printf("🔗 Intentando reproducir URL con HTTPClient: %s\n", urlToPlay.c_str());

    // 2. Configuración de la Conexión HTTPS (ignora el certificado para evitar fallos)
    audioClient.setInsecure();
    
    // 3. Iniciar HTTPClient con el cliente seguro
    if (!http.begin(audioClient, urlToPlay)) {
        Serial.println("❌ ERROR: No se pudo iniciar HTTPClient.");
        return;
    }
    
    // 4. Establecer un Timeout
    http.setConnectTimeout(10000); // 10 segundos

    // 5. Enviar la solicitud HTTP GET
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) { // Código 200 (Éxito)
        Serial.printf("✅ Conexión HTTP OK. Código: %d\n", httpCode);

        // 6. Crear el AudioFileSourceStream con la conexión abierta
        fileSource = new AudioFileSourceStream(&audioClient);
        
        // 7. Iniciar la reproducción
        if (mp3.begin(fileSource, &i2s)) {
            Serial.printf("🎶 Reproduciendo con nuevo cliente: %s\n", urlToPlay.c_str());
            alarmaActivaAudio = true;
        } else {
            Serial.println("❌ Error al iniciar reproducción con AudioFileSourceStream.");
            alarmaActivaAudio = false;
        }
    } else {
        Serial.printf("❌ Error al obtener el archivo. Código HTTP: %d\n", httpCode);
        Serial.printf("❌ Error de conexión (HTTPClient.GET): %s\n", http.errorToString(httpCode).c_str());
        alarmaActivaAudio = false;
    }
    
    // 8. Limpiar recursos si la reproducción NO inició
    if (!alarmaActivaAudio) {
        if (fileSource) {
            delete fileSource;
            fileSource = nullptr;
        }
        http.end(); 
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
    if (isWiFiConnected) client.setInsecure(); // Cliente de Telegram

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

            // mp3.loop() mantiene el streaming y la reproducción
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
            // Pasa al siguiente audio en el arreglo
            current_message_index = (current_message_index + 1) % messages.size();
            
            // 🚨 LIMPIEZA FINAL: Cierra explícitamente la conexión HTTP después de detener la reproducción
            if (http.connected()) {
                http.end();
            }
            if (fileSource) {
                delete fileSource;
                fileSource = nullptr;
            }
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
