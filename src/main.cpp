#include <Arduino.h>
#include <WebServer.h>
#include <AutoConnect.h>
#include <MFRC522.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

namespace {
// User settings: update this section to match your board and installation.
constexpr uint8_t LED_PIN = 2;
constexpr char API_TOKEN[] = "123";
constexpr uint8_t RFID_SS_PIN = 4;
constexpr uint8_t RFID_RST_PIN = 16;
constexpr uint8_t RFID_SCK_PIN = 18;
constexpr uint8_t RFID_MISO_PIN = 19;
constexpr uint8_t RFID_MOSI_PIN = 23;
constexpr uint8_t OLED_SDA_PIN = 21;
constexpr uint8_t OLED_SCL_PIN = 22;
constexpr uint8_t OLED_WIDTH = 128;
constexpr uint8_t OLED_HEIGHT = 64;
constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr uint8_t MAX_CARD_COUNT = 20;
constexpr uint16_t WEB_SERVER_PORT = 80;
constexpr char AUTOCONNECT_AP_SSID[] = "ESP32-Setup";
constexpr char AUTOCONNECT_AP_PASSWORD[] = "esp32setup";
constexpr bool AUTOCONNECT_AUTO_RECONNECT = true;
constexpr uint8_t AUTOCONNECT_RECONNECT_INTERVAL = 10;

// RC522 signals and power must use 3.3V logic; do not connect it to 5V.

WebServer server(WEB_SERVER_PORT);
AutoConnect portal(server);
AutoConnectConfig portalConfig;
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Preferences cardPreferences;
bool ledOn = false;
bool displayReady = false;
bool cardStorageReady = false;
String cardIds[MAX_CARD_COUNT];
uint8_t cardCount = 0;
String serialCommand;

void scanI2cBus() {
    Serial.printf("Memindai perangkat I2C pada GPIO%u (SDA) dan GPIO%u (SCL)...\n",
                  OLED_SDA_PIN, OLED_SCL_PIN);
    uint8_t foundDevices = 0;
    for (uint8_t address = 1; address < 127; ++address) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission() == 0) {
            Serial.printf("  Perangkat I2C ditemukan di alamat 0x%02X\n", address);
            ++foundDevices;
        }
    }

    if (foundDevices == 0) {
        Serial.println("  PERINGATAN: tidak ada perangkat I2C. Periksa daya, GND, SDA, dan SCL OLED.");
    } else if (foundDevices > 1) {
        Serial.printf("  Info: ditemukan %u perangkat I2C.\n", foundDevices);
    }
}

String normalizeCardId(String cardId) {
    cardId.trim();
    cardId.toUpperCase();
    if (cardId.length() != 11 && cardId.length() != 20 && cardId.length() != 29) return "";

    for (uint8_t i = 0; i < cardId.length(); ++i) {
        if (i % 3 == 2) {
            if (cardId[i] != ':') return "";
        } else if (!((cardId[i] >= '0' && cardId[i] <= '9') ||
                     (cardId[i] >= 'A' && cardId[i] <= 'F'))) {
            return "";
        }
    }
    return cardId;
}

bool isCardRegistered(const String& cardId) {
    for (uint8_t i = 0; i < cardCount; ++i) {
        if (cardIds[i] == cardId) return true;
    }
    return false;
}

void showCardResult(const String& cardId, bool authorized) {
    if (!displayReady) return;

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(authorized ? "KARTU VALID" : "KARTU TIDAK VALID");
    display.println(authorized ? "Akses lolos" : "Akses ditolak");
    display.println(cardId);
    display.display();
}

String serializeCardList(const String* cards, uint8_t count) {
    String serialized;
    serialized.reserve(count * 15);
    for (uint8_t i = 0; i < count; ++i) {
        if (i > 0) serialized += ',';
        serialized += cards[i];
    }
    return serialized;
}

bool persistCardList(const String* cards, uint8_t count) {
    if (!cardStorageReady) return false;
    if (count == 0) return cardPreferences.remove("cards");
    String serialized = serializeCardList(cards, count);
    return cardPreferences.putString("cards", serialized) > 0;
}

void loadCardList() {
    if (!cardStorageReady) return;

    String stored = cardPreferences.getString("cards", "");
    int start = 0;
    while (start < stored.length() && cardCount < MAX_CARD_COUNT) {
        int separator = stored.indexOf(',', start);
        int end = separator < 0 ? stored.length() : separator;
        String cardId = normalizeCardId(stored.substring(start, end));
        if (cardId.length() == 0 || isCardRegistered(cardId)) {
            Serial.println("Peringatan: data UID kartu tersimpan tidak valid atau duplikat.");
        } else {
            cardIds[cardCount++] = cardId;
        }
        if (separator < 0) break;
        start = separator + 1;
    }
    Serial.printf("%u UID kartu dimuat dari penyimpanan ESP32.\n", cardCount);
}

String cardListJson(const String* cards, uint8_t count) {
    String json = "{\"cards\":[";
    for (uint8_t i = 0; i < count; ++i) {
        if (i > 0) json += ',';
        json += '"';
        json += cards[i];
        json += '"';
    }
    json += "]}";
    return json;
}

bool findCard(const String& cardId, uint8_t& index) {
    for (uint8_t i = 0; i < cardCount; ++i) {
        if (cardIds[i] == cardId) {
            index = i;
            return true;
        }
    }
    return false;
}

bool isAuthorized();
void sendUnauthorized();

void handleCardsGet() {
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!cardStorageReady) {
        server.send(500, "application/json", "{\"error\":\"card storage unavailable\"}");
        return;
    }
    server.send(200, "application/json", cardListJson(cardIds, cardCount));
}

void handleCardCreate() {
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!cardStorageReady) {
        server.send(500, "application/json", "{\"error\":\"card storage unavailable\"}");
        return;
    }
    String cardId = normalizeCardId(server.arg("id"));
    if (cardId.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"invalid card ID format\"}");
        return;
    }
    if (isCardRegistered(cardId)) {
        server.send(409, "application/json", "{\"error\":\"card ID already exists\"}");
        return;
    }
    if (cardCount >= MAX_CARD_COUNT) {
        server.send(409, "application/json", "{\"error\":\"card storage is full\"}");
        return;
    }

    String updated[MAX_CARD_COUNT];
    for (uint8_t i = 0; i < cardCount; ++i) updated[i] = cardIds[i];
    updated[cardCount] = cardId;
    if (!persistCardList(updated, cardCount + 1)) {
        Serial.println("Gagal menyimpan UID kartu baru ke Preferences.");
        server.send(500, "application/json", "{\"error\":\"failed to save card ID\"}");
        return;
    }
    cardIds[cardCount++] = cardId;
    server.send(201, "application/json", cardListJson(cardIds, cardCount));
}

void handleCardUpdate() {
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!cardStorageReady) {
        server.send(500, "application/json", "{\"error\":\"card storage unavailable\"}");
        return;
    }
    String cardId = normalizeCardId(server.arg("id"));
    String newCardId = normalizeCardId(server.arg("newId"));
    if (cardId.length() == 0 || newCardId.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"invalid card ID format\"}");
        return;
    }

    uint8_t index;
    if (!findCard(cardId, index)) {
        server.send(404, "application/json", "{\"error\":\"card ID not found\"}");
        return;
    }
    uint8_t duplicateIndex;
    if (findCard(newCardId, duplicateIndex) && duplicateIndex != index) {
        server.send(409, "application/json", "{\"error\":\"card ID already exists\"}");
        return;
    }

    String updated[MAX_CARD_COUNT];
    for (uint8_t i = 0; i < cardCount; ++i) updated[i] = cardIds[i];
    updated[index] = newCardId;
    if (!persistCardList(updated, cardCount)) {
        Serial.println("Gagal memperbarui UID kartu di Preferences.");
        server.send(500, "application/json", "{\"error\":\"failed to update card ID\"}");
        return;
    }
    cardIds[index] = newCardId;
    server.send(200, "application/json", cardListJson(cardIds, cardCount));
}

void handleCardDelete() {
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!cardStorageReady) {
        server.send(500, "application/json", "{\"error\":\"card storage unavailable\"}");
        return;
    }
    String cardId = normalizeCardId(server.arg("id"));
    if (cardId.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"invalid card ID format\"}");
        return;
    }

    uint8_t index;
    if (!findCard(cardId, index)) {
        server.send(404, "application/json", "{\"error\":\"card ID not found\"}");
        return;
    }

    String updated[MAX_CARD_COUNT];
    uint8_t updatedCount = 0;
    for (uint8_t i = 0; i < cardCount; ++i) {
        if (i != index) updated[updatedCount++] = cardIds[i];
    }
    if (!persistCardList(updated, updatedCount)) {
        Serial.println("Gagal menghapus UID kartu dari Preferences.");
        server.send(500, "application/json", "{\"error\":\"failed to delete card ID\"}");
        return;
    }
    for (uint8_t i = 0; i < updatedCount; ++i) cardIds[i] = updated[i];
    cardCount = updatedCount;
    server.send(200, "application/json", cardListJson(cardIds, cardCount));
}

void readRfidCard() {
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

    String cardId;
    for (byte i = 0; i < rfid.uid.size; ++i) {
        if (i > 0) cardId += ':';
        if (rfid.uid.uidByte[i] < 0x10) cardId += '0';
        cardId += String(rfid.uid.uidByte[i], HEX);
    }
    cardId.toUpperCase();

    Serial.print("ID kartu: ");
    Serial.println(cardId);
    const bool authorized = isCardRegistered(cardId);
    Serial.println(authorized ? "Validasi kartu lolos." : "Validasi kartu ditolak.");
    showCardResult(cardId, authorized);

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

bool isAuthorized() {
    String authorization = server.header("Authorization");
    return authorization == String("Bearer ") + API_TOKEN;
}

void sendUnauthorized() {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
}

void printWifiStatus() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi tidak sedang terhubung.");
        return;
    }

    Serial.print("SSID Wi-Fi terhubung: ");
    Serial.println(WiFi.SSID());
    Serial.print("Password Wi-Fi terhubung: ");
    Serial.println(WiFi.psk());
    Serial.print("IP ESP32: ");
    Serial.println(WiFi.localIP());
    Serial.println("Catatan: password ditampilkan di Serial Monitor; jangan membagikan log ini.");
}

void handleSerialCommand(String command) {
    command.trim();
    command.toLowerCase();

    if (command == "wifi status") {
        printWifiStatus();
    } else if (command == "wifi disconnect") {
        portalConfig.autoReconnect = false;
        portal.config(portalConfig);
        WiFi.setAutoReconnect(false);
        if (WiFi.status() == WL_CONNECTED) {
            if (!WiFi.disconnect(false, false)) {
                Serial.println("Gagal mengirim perintah putus Wi-Fi.");
                return;
            }
            const uint32_t disconnectStarted = millis();
            while (WiFi.status() == WL_CONNECTED && millis() - disconnectStarted < 3000) {
                delay(10);
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("Wi-Fi masih terhubung setelah menunggu 3 detik; reconnect sudah dinonaktifkan.");
                return;
            }
        }
        Serial.println("Wi-Fi diputus dan AutoConnect reconnect dinonaktifkan sampai ESP32 restart.");
        Serial.println("Kredensial Wi-Fi tersimpan tidak dihapus.");
    } else if (command.length() > 0) {
        Serial.println("Perintah tidak dikenal. Gunakan: wifi status atau wifi disconnect");
    }
}

void handleSerialInput() {
    while (Serial.available() > 0) {
        const char input = static_cast<char>(Serial.read());
        if (input == '\r') continue;
        if (input == '\n') {
            handleSerialCommand(serialCommand);
            serialCommand = "";
        } else if (serialCommand.length() < 64) {
            serialCommand += input;
        } else {
            serialCommand = "";
            Serial.println("Perintah serial terlalu panjang; input dibuang.");
        }
    }
}

void handleControlPage() {
    static const char page[] PROGMEM = R"HTML(
<!doctype html><html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kontrol ESP32</title>
<style>
body{font-family:system-ui,sans-serif;max-width:32rem;margin:3rem auto;padding:0 1rem;background:#eef2f3;color:#172126}
main{background:#fff;padding:2rem;border-radius:12px;box-shadow:0 8px 24px #17212622}
button,input,textarea{font:inherit;padding:.7rem;margin:.25rem 0;border:1px solid #b7c2c5;border-radius:6px}
button{cursor:pointer;background:#176b87;color:#fff;border:0;min-width:8rem}button.off{background:#b33a3a}
input,textarea{box-sizing:border-box;width:100%}textarea{min-height:4rem;resize:vertical}
#status,#cardMessage{margin:1rem 0;font-weight:600}.card{display:flex;align-items:center;justify-content:space-between;gap:.5rem;border-top:1px solid #d8e0e2;padding:.5rem 0}
.card button{min-width:auto;padding:.45rem}.actions{display:flex;flex-wrap:wrap;gap:.35rem}
</style></head><body><main><h1>ESP32 LED</h1>
<label>API token<input id="token" type="password" autocomplete="off"></label>
<p id="status">Status belum dibaca.</p>
<button onclick="setLed(true)">Nyalakan</button>
<button class="off" onclick="setLed(false)">Matikan</button>
<hr><h2>Kelola ID kartu RFID</h2>
<label>ID kartu<textarea id="cardId" placeholder="Contoh: 04:A1:B2:C3" maxlength="29"></textarea></label>
<p>Satu UID per input, format byte heksadesimal dipisahkan titik dua. Maksimal 20 kartu.</p>
<div class="actions"><button onclick="createCard()">Tambah</button><button onclick="updateCard()">Perbarui pilihan</button><button class="off" onclick="deleteCard()">Hapus pilihan</button></div>
<p id="cardMessage">Masukkan token untuk memuat daftar kartu.</p><div id="cards"></div>
<script>
const token=document.querySelector('#token');
const cardInput=document.querySelector('#cardId');
let selectedCard='';
async function request(path,options={}){options.headers={...(options.headers||{}),Authorization:'Bearer '+token.value};const r=await fetch(path,options);if(!r.ok)throw Error(await r.text());return r.json()}
async function cardRequest(method,data){return request('/api/cards',{method,headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)})}
async function refresh(){try{const s=await request('/api/status');document.querySelector('#status').textContent=`LED: ${s.led?'ON':'OFF'} | Wi-Fi: ${s.ip}`}catch(e){document.querySelector('#status').textContent='Token salah atau ESP32 tidak dapat dihubungi.'}}
async function setLed(value){try{await request('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({on:value})});refresh()}catch(e){alert('Perintah ditolak. Periksa token.')}}
function showCardError(e){document.querySelector('#cardMessage').textContent='Gagal: '+e.message}
function renderCards(list){const container=document.querySelector('#cards');container.replaceChildren();for(const id of list){const row=document.createElement('div');row.className='card';const label=document.createElement('span');label.textContent=id;const edit=document.createElement('button');edit.textContent='Pilih';edit.onclick=()=>{selectedCard=id;cardInput.value=id;document.querySelector('#cardMessage').textContent='Dipilih: '+id};row.append(label,edit);container.append(row)}}
async function refreshCards(){try{const result=await request('/api/cards');renderCards(result.cards);document.querySelector('#cardMessage').textContent=result.cards.length?`${result.cards.length} kartu tersimpan.`:'Belum ada kartu tersimpan.'}catch(e){showCardError(e)}}
async function createCard(){try{const result=await cardRequest('POST',{id:cardInput.value});selectedCard='';renderCards(result.cards);cardInput.value='';document.querySelector('#cardMessage').textContent='ID kartu berhasil ditambahkan.'}catch(e){showCardError(e)}}
async function updateCard(){if(!selectedCard){document.querySelector('#cardMessage').textContent='Pilih kartu yang akan diperbarui.';return}try{const result=await cardRequest('PUT',{id:selectedCard,newId:cardInput.value});selectedCard='';renderCards(result.cards);document.querySelector('#cardMessage').textContent='ID kartu berhasil diperbarui.'}catch(e){showCardError(e)}}
async function deleteCard(){if(!selectedCard){document.querySelector('#cardMessage').textContent='Pilih kartu yang akan dihapus.';return}try{const result=await cardRequest('DELETE',{id:selectedCard});selectedCard='';cardInput.value='';renderCards(result.cards);document.querySelector('#cardMessage').textContent='ID kartu berhasil dihapus.'}catch(e){showCardError(e)}}
token.addEventListener('change',()=>{refresh();refreshCards()});
</script></main></body></html>)HTML";
    server.send(200, "text/html; charset=utf-8", page);
}

void handleStatus() {
    if (!isAuthorized()) { sendUnauthorized(); return; }
    String json = String("{\"led\":") + (ledOn ? "true" : "false") +
                                ",\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    server.send(200, "application/json", json);
}

void handleLed() {
    if (!isAuthorized()) { sendUnauthorized(); return; }
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"JSON body required\"}");
        return;
    }

    String body = server.arg("plain");
    if (body.indexOf("\"on\":true") >= 0) ledOn = true;
    else if (body.indexOf("\"on\":false") >= 0) ledOn = false;
    else {
        server.send(400, "application/json", "{\"error\":\"on must be boolean\"}");
        return;
    }

    digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
    server.send(200, "application/json", ledOn ? "{\"led\":true}" : "{\"led\":false}");
}
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    cardStorageReady = cardPreferences.begin("rfid-cards", false);
    if (cardStorageReady) {
        loadCardList();
    } else {
        Serial.println("Gagal membuka penyimpanan UID kartu pada Preferences.");
    }

    const char* headerKeys[] = {"Authorization"};
    server.collectHeaders(headerKeys, 1);

    server.on("/control", HTTP_GET, handleControlPage);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/led", HTTP_POST, handleLed);
    server.on("/api/cards", HTTP_GET, handleCardsGet);
    server.on("/api/cards", HTTP_POST, handleCardCreate);
    server.on("/api/cards", HTTP_PUT, handleCardUpdate);
    server.on("/api/cards", HTTP_DELETE, handleCardDelete);

    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    scanI2cBus();

    displayReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
    if (displayReady) {
        Serial.printf("OLED SSD1306 siap di alamat 0x%02X.\n", OLED_ADDRESS);
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.println("Tempelkan kartu");
        display.println("RFID...");
        display.display();
    } else {
        Serial.printf("Gagal menginisialisasi OLED di alamat 0x%02X.\n", OLED_ADDRESS);
        Serial.printf("Periksa alamat OLED, kabel SDA->GPIO%u, SCL->GPIO%u, VCC, dan GND.\n",
                      OLED_SDA_PIN, OLED_SCL_PIN);
    }

    Serial.printf("Menginisialisasi RC522 pada SPI: SS=GPIO%u, SCK=GPIO%u, MISO=GPIO%u, MOSI=GPIO%u, RST=GPIO%u.\n",
                  RFID_SS_PIN, RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_RST_PIN);
    SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
    rfid.PCD_Init();
    const byte rfidVersion = rfid.PCD_ReadRegister(rfid.VersionReg);
    if (rfidVersion == 0x00 || rfidVersion == 0xFF) {
        Serial.printf("RC522 tidak merespons (VersionReg=0x%02X).\n", rfidVersion);
        Serial.printf("Periksa SS->GPIO%u, SCK->GPIO%u, MISO->GPIO%u, MOSI->GPIO%u, RST->GPIO%u, VCC=3V3, dan GND.\n",
                      RFID_SS_PIN, RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_RST_PIN);
    } else {
        Serial.printf("RC522 merespons (VersionReg=0x%02X). Tempelkan kartu 13,56 MHz yang kompatibel.\n", rfidVersion);
    }

    portalConfig.apid = AUTOCONNECT_AP_SSID;
    portalConfig.psk = AUTOCONNECT_AP_PASSWORD;
    portalConfig.autoReconnect = AUTOCONNECT_AUTO_RECONNECT;
    portalConfig.reconnectInterval = AUTOCONNECT_RECONNECT_INTERVAL;
    portal.config(portalConfig);

    if (portal.begin()) {
        Serial.print("Wi-Fi connected. Control page: http://");
        Serial.print(WiFi.localIP());
        Serial.println("/control");
        printWifiStatus();
    } else {
        Serial.println("Wi-Fi setup failed; restarting...");
        delay(3000);
        ESP.restart();
    }
}

void loop() {
    portal.handleClient();
    handleSerialInput();
    readRfidCard();
}