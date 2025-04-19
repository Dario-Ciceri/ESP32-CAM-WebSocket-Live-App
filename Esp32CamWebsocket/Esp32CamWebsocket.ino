#include "Arduino.h"
#include <WiFi.h>
#include <WebSocketsServer.h>
#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <ESPmDNS.h>
#include <mbedtls/sha256.h>

// Configurazione WiFi
const char* ssid = "test";
const char* password = "12345678";

// Token di sicurezza (deve essere lo stesso utilizzato nell'app)
const char* securityToken = "Uh7X$zPq2L9Tb@RvN";

// WebSocket Server
WebSocketsServer webSocket(81);

// Configurazione modulo camera OV2640
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// Array per memorizzare i client autenticati
bool authenticatedClients[WEBSOCKETS_SERVER_CLIENT_MAX];
bool streaming = false;

// Funzione per calcolare l'hash SHA-256 di una stringa
String calculateSHA256(const String& input) {
  byte shaResult[32];
  mbedtls_sha256_context ctx;

  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);  // 0 per SHA-256
  mbedtls_sha256_update(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_sha256_finish(&ctx, shaResult);

  String hashStr = "";
  for (int i = 0; i < 32; i++) {
    char hex[3];
    sprintf(hex, "%02x", shaResult[i]);
    hashStr += hex;
  }

  return hashStr;
}

// Verifica se il token fornito è valido
bool isValidToken(const String& token) {
  String expectedHash = calculateSHA256(String(securityToken));
  return token == expectedHash;
}

// Estrae il parametro token dall'URL
String extractTokenFromURL(const String& url) {
  int tokenPos = url.indexOf("token=");
  if (tokenPos == -1) {
    return "";
  }

  int tokenStart = tokenPos + 6;  // lunghezza di "token="
  int tokenEnd = url.indexOf('&', tokenStart);

  if (tokenEnd == -1) {
    return url.substring(tokenStart);
  } else {
    return url.substring(tokenStart, tokenEnd);
  }
}

void setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // if (psramFound()) {
  //   config.frame_size = FRAMESIZE_UXGA;  // Risoluzione alta con PSRAM
  //   config.jpeg_quality = 10;            // 10-63 (numero più basso = qualità più alta)
  //   config.fb_count = 2;
  //   Serial.println("High Quality Stream (PSRAM found)");
  // } else {
  config.frame_size = FRAMESIZE_VGA;  // Risoluzione più bassa senza PSRAM
  config.jpeg_quality = 20;
  config.fb_count = 2;
  Serial.println("Low Quality Stream (No PSRAM)");
  //}

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    ESP.restart();
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_brightness(s, 0);                  // -2 to 2
  s->set_contrast(s, 0);                    // -2 to 2
  s->set_saturation(s, 0);                  // -2 to 2
  s->set_special_effect(s, 0);              // 0 to 6 (0 - No Effect, 1 - Negative, 2 - Grayscale, 3 - Red Tint, 4 - Green Tint, 5 - Blue Tint, 6 - Sepia)
  s->set_whitebal(s, 1);                    // 0 = disable, 1 = enable
  s->set_awb_gain(s, 1);                    // 0 = disable, 1 = enable
  s->set_wb_mode(s, 0);                     // 0 to 4 - if awb_gain enabled (0 - Auto, 1 - Sunny, 2 - Cloudy, 3 - Office, 4 - Home)
  s->set_exposure_ctrl(s, 1);               // 0 = disable, 1 = enable
  s->set_aec2(s, 0);                        // 0 = disable, 1 = enable
  s->set_ae_level(s, 0);                    // -2 to 2
  s->set_aec_value(s, 300);                 // 0 to 1200
  s->set_gain_ctrl(s, 1);                   // 0 = disable, 1 = enable
  s->set_agc_gain(s, 0);                    // 0 to 30
  s->set_gainceiling(s, (gainceiling_t)0);  // 0 to 6
  s->set_bpc(s, 0);                         // 0 = disable, 1 = enable
  s->set_wpc(s, 1);                         // 0 = disable, 1 = enable
  s->set_raw_gma(s, 1);                     // 0 = disable, 1 = enable
  s->set_lenc(s, 1);                        // 0 = disable, 1 = enable
  s->set_hmirror(s, 0);                     // 0 = disable, 1 = enable
  s->set_vflip(s, 0);                       // 0 = disable, 1 = enable
  s->set_dcw(s, 1);                         // 0 = disable, 1 = enable
  s->set_colorbar(s, 0);                    // 0 = disable, 1 = enable
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Client connesso: %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);

        // Estrai il token dall'URL
        String url = String((char*)payload);
        String token = extractTokenFromURL(url);

        // Verifica il token
        if (isValidToken(token)) {
          Serial.printf("[%u] Client autenticato con successo\n", num);
          authenticatedClients[num] = true;

          // Controlla se c'è almeno un client autenticato
          bool hasAuthenticatedClient = false;
          for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
            if (authenticatedClients[i]) {
              hasAuthenticatedClient = true;
              break;
            }
          }

          // Abilita lo streaming se c'è almeno un client autenticato
          if (hasAuthenticatedClient) {
            streaming = true;
          }
        } else {
          Serial.printf("[%u] Autenticazione fallita, token non valido\n", num);
          authenticatedClients[num] = false;
          webSocket.disconnect(num);
        }
      }
      break;

    case WStype_DISCONNECTED:
      Serial.printf("[%u] Client disconnesso\n", num);
      authenticatedClients[num] = false;

      // Controlla se ci sono ancora client autenticati
      bool hasAuthenticatedClient = false;
      for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        if (authenticatedClients[i]) {
          hasAuthenticatedClient = true;
          break;
        }
      }

      // Disabilita lo streaming se non ci sono più client autenticati
      if (!hasAuthenticatedClient) {
        streaming = false;
      }
      break;
  }
}

void setup() {
  Serial.begin(115200);
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // Disabilita il brownout detector

  // Inizializza array dei client autenticati
  for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
    authenticatedClients[i] = false;
  }

  // Connessione alla rete WiFi
  WiFi.begin(ssid, password);

  // Attendi la connessione
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connesso");
  Serial.print("Indirizzo IP: ");
  Serial.println(WiFi.localIP());

  // Inizializza mDNS
  if (!MDNS.begin("ESP32-CAM")) {
    Serial.println("Errore durante l'avvio di mDNS");
    ESP.restart();
  }

  // Registra il servizio mDNS
  MDNS.addService("_esp32cam", "_tcp", 81);
  Serial.println("Servizio mDNS registrato");

  // Inizializza la fotocamera
  setupCamera();

  // Avvia il server WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("Server WebSocket avviato");
}

void loop() {
  webSocket.loop();
  // MDNS.update() non è necessario nelle versioni recenti dell'ESP32 Arduino Core

  if (streaming) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      // Invia il frame solo ai client autenticati
      for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
        if (authenticatedClients[i]) {
          webSocket.sendBIN(i, fb->buf, fb->len);
        }
      }
      esp_camera_fb_return(fb);
    }
    // Piccolo ritardo per non sovraccaricare il sistema
    delay(10);
  } else {
    // Se non ci sono client, aspetta un po' di più per risparmiare energia
    delay(100);
  }
}