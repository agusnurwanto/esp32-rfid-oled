#include <Arduino.h>
#include <WebServer.h>
#include <AutoConnect.h>

namespace {
constexpr uint8_t LED_PIN = 2;
constexpr char API_TOKEN[] = "CHANGE_THIS_TO_A_LONG_RANDOM_TOKEN";

WebServer server(80);
AutoConnect portal(server);
bool ledOn = false;

bool isAuthorized() {
    String authorization = server.header("Authorization");
    return authorization == String("Bearer ") + API_TOKEN;
}

void sendUnauthorized() {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
}

void handleControlPage() {
    static const char page[] PROGMEM = R"HTML(
<!doctype html><html lang="id"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 LED</title>
<style>
body{font-family:system-ui,sans-serif;max-width:32rem;margin:3rem auto;padding:0 1rem;background:#eef2f3;color:#172126}
main{background:#fff;padding:2rem;border-radius:12px;box-shadow:0 8px 24px #17212622}
button,input{font:inherit;padding:.7rem;margin:.25rem 0;border:1px solid #b7c2c5;border-radius:6px}
button{cursor:pointer;background:#176b87;color:#fff;border:0;min-width:8rem}button.off{background:#b33a3a}
input{box-sizing:border-box;width:100%}#status{margin:1rem 0;font-weight:600}
</style></head><body><main><h1>ESP32 LED</h1>
<label>API token<input id="token" type="password" autocomplete="off"></label>
<p id="status">Status belum dibaca.</p>
<button onclick="setLed(true)">Nyalakan</button>
<button class="off" onclick="setLed(false)">Matikan</button>
<script>
const token=document.querySelector('#token');
async function request(path,options={}){options.headers={...(options.headers||{}),Authorization:'Bearer '+token.value};const r=await fetch(path,options);if(!r.ok)throw Error(await r.text());return r.json()}
async function refresh(){try{const s=await request('/api/status');document.querySelector('#status').textContent=`LED: ${s.led?'ON':'OFF'} | Wi-Fi: ${s.ip}`}catch(e){document.querySelector('#status').textContent='Token salah atau ESP32 tidak dapat dihubungi.'}}
async function setLed(value){try{await request('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({on:value})});refresh()}catch(e){alert('Perintah ditolak. Periksa token.')}}
token.addEventListener('change',refresh);
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

    const char* headerKeys[] = {"Authorization"};
    server.collectHeaders(headerKeys, 1);

        server.on("/control", HTTP_GET, handleControlPage);
        server.on("/api/status", HTTP_GET, handleStatus);
        server.on("/api/led", HTTP_POST, handleLed);

        AutoConnectConfig config;
        config.apid = "ESP32-Setup";
        config.psk = "esp32setup";
        config.autoReconnect = true;
        config.reconnectInterval = 10;
        portal.config(config);

        if (portal.begin()) {
                Serial.print("Wi-Fi connected. Control page: http://");
                Serial.print(WiFi.localIP());
                Serial.println("/control");
        } else {
                Serial.println("Wi-Fi setup failed; restarting...");
                delay(3000);
                ESP.restart();
        }
}

void loop() {
        portal.handleClient();
}