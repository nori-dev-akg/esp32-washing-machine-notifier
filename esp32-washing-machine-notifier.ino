#include <driver/rtc_io.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <base64.h> // for http basic auth
#include <FS.h>
#include <SPIFFS.h>
#include <esp8266-google-home-notifier.h>
#include "myWiFi.h"

// VoiceText Web API
const String tts_url = "https://api.voicetext.jp/v1/tts";
const String tts_user = "xxxxxxxxxxxxxxxx";   // set your id
const String tts_pass = "";   // ここは空で良い
const String mp3file = "test.mp3";
String tts_parms = "&speaker=hikari&volume=200&speed=120&format=mp3";

// google-home-notifier
GoogleHomeNotifier ghn;
const char displayName[] = "ダイニング ルーム"; // GoogleHomeの名前に変える
const char speekText[] = "洗濯が終わりました";  // 洗濯終了時の発声

WebServer server(80);
String head = "<meta charset=\"utf-8\">";

#define HALL_SENSOR_PIN     GPIO_NUM_33     // 磁気センサー
#define HALL_SENSOR_BITMASK 0x200000000     // 磁気センサー 2^33 in hex
#define LED_PIN             GPIO_NUM_13     // LED
#define WASHTIME_SEC        35 * 60         // 洗濯時間 35 min 
#define NOTIFYAGAIN_SEC      5 * 60         // 再通知間隔 5 min
#define NOTIFYAGAIN_CNT      3              // 最大再通知回数

esp_sleep_wakeup_cause_t wakeup_reason;     // deep sleep 復帰理由
bool handlePlayDone = false;                // 発声終了待ちフラグ

RTC_DATA_ATTR int notifyAgain = 0;          // 再通知カウント

// OTA
#define OTA_PIN GPIO_NUM_26
bool OTA_FALG = false;

void setup()
{
    Serial.begin(115200);

    pinMode(HALL_SENSOR_PIN, INPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(OTA_PIN, INPUT);

    digitalWrite(LED_PIN, HIGH);    // LED ON
    
    // OTA mode
    OTA_FALG = (digitalRead(OTA_PIN) == HIGH) ? true : false;
    if (OTA_FALG)
    {
        Serial.println("Run OTA mode...");
        if(connectWiFi()) {
            // Hostname defaults to esp3232-[MAC]
            ArduinoOTA.setHostname("esp32 WASH");
            ArduinoOTA
                .onStart([]() {
                    String type;
                    if (ArduinoOTA.getCommand() == U_FLASH)
                        type = "sketch";
                    else // U_SPIFFS
                        type = "filesystem";
    
                    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
                    Serial.println("Start updating " + type);
                })
                .onEnd([]() {
                    Serial.println("\nEnd");
                })
                .onProgress([](unsigned int progress, unsigned int total) {
                    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
                })
                .onError([](ota_error_t error) {
                    Serial.printf("Error[%u]: ", error);
                    if (error == OTA_AUTH_ERROR)
                        Serial.println("Auth Failed");
                    else if (error == OTA_BEGIN_ERROR)
                        Serial.println("Begin Failed");
                    else if (error == OTA_CONNECT_ERROR)
                        Serial.println("Connect Failed");
                    else if (error == OTA_RECEIVE_ERROR)
                        Serial.println("Receive Failed");
                    else if (error == OTA_END_ERROR)
                        Serial.println("End Failed");
                });
            ArduinoOTA.begin();
        }
        return;
    }

    // wakeup reason
    wakeup_reason = esp_sleep_get_wakeup_cause();
    switch(wakeup_reason)
    {
        case ESP_SLEEP_WAKEUP_UNDEFINED:    // 初回起動時
            Serial.println("Wakeup caused by wasnot deep sleep");
            if(connectWiFi()) {
                text2mp3(speekText);
                startDeepSeep(wakeup_reason);
             }
            break;
        case ESP_SLEEP_WAKEUP_EXT1:         // センサーが変化した時
            Serial.println("Wakeup caused by external signal using RTC_CNTL");
            digitalRead(HALL_SENSOR_PIN);
            delay(1000);    // for LED
            startDeepSeep(wakeup_reason);
            break;
        case ESP_SLEEP_WAKEUP_TIMER:        // タイマー復帰した時
            Serial.println("Wakeup caused by timer");
            if(connectWiFi()) {
                handlePlayDone = false;
                speech2googlehome();
            }
            break;
    }
}

void startDeepSeep(esp_sleep_wakeup_cause_t wakeup_reason)
{
    int sensor = digitalRead(HALL_SENSOR_PIN);

    disconnectWiFi();
    
    Serial.printf("startDeepSeep: sensor=%d ", sensor);
    
    if(sensor) {   // 蓋が閉じている
        if(wakeup_reason == ESP_SLEEP_WAKEUP_TIMER && notifyAgain < 3) {
            esp_sleep_enable_timer_wakeup(NOTIFYAGAIN_SEC * 1000000LL); // 再通知 sec
            Serial.printf("notify again: sec=%d ", NOTIFYAGAIN_SEC);
            Serial.printf("notifyAgain=%d ", notifyAgain);
            notifyAgain++;
        }
        else if(wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
            esp_sleep_enable_timer_wakeup(WASHTIME_SEC * 1000000LL);    // 洗濯時間 sec
            Serial.printf("start wash: sec=%d ", WASHTIME_SEC);
            notifyAgain = 0;
        }
        esp_sleep_enable_ext1_wakeup(HALL_SENSOR_BITMASK, ESP_EXT1_WAKEUP_ALL_LOW);
        Serial.println("and enable_ext1");
    }
    else {  // 蓋が開いている
        esp_sleep_enable_ext1_wakeup(HALL_SENSOR_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
        Serial.println("only enable_ext1");
    }
    
    esp_deep_sleep_start();
}


void loop()
{
    if (OTA_FALG)
    {
        ArduinoOTA.handle();
        digitalWrite(LED_PIN,HIGH);
        delay(200);
        digitalWrite(LED_PIN,LOW);
        delay(200);
        return;
    }

    server.handleClient();

    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER && handlePlayDone)
    {
        delay(1000);
        startDeepSeep(wakeup_reason);
    }
}

void handlePlay()
{
    Serial.println("handleVoice play.");
    String path = server.uri();
    SPIFFS.begin(true);
    if (SPIFFS.exists(path))
    {
        Serial.println("handlePlay: sending " + path);
        File file = SPIFFS.open(path, "r");
        server.streamFile(file, "audio/mp3");
        file.close();
        Serial.println("handlePlay SPIFFS file exists");
    }
    else
    {
        server.send(200, "text/html", head + "<h1>text2speech</h1><br>SPIFFS file not found:" + path);
        Serial.println("handlePlay SPIFFS file not found ");
    }
    SPIFFS.end();
    handlePlayDone = true;
}

bool speech2googlehome()
{
    // WebServer
    server.on("/" + mp3file, handlePlay);
    server.begin();

    // google-home-notifier
    Serial.println("connecting to Google Home...");
    if (ghn.device(displayName, "ja") != true)
    {
        Serial.println(ghn.getLastError());
        return false;
    }
    Serial.print("found Google Home(");
    Serial.print(ghn.getIPAddress());
    Serial.print(":");
    Serial.print(ghn.getPort());
    Serial.println(")");

    String mp3url = "http://" + WiFi.localIP().toString() + "/" + mp3file;
    Serial.println("GoogleHomeNotifier.play() start");
    if (ghn.play(mp3url.c_str()) != true)
    {
        Serial.print("GoogleHomeNotifier.play() error:");
        Serial.println(ghn.getLastError());
        return false;
    }
    return true;
}


// text to mp3
bool text2mp3(String text)
{
    Serial.println("text to mp3:" + text);

    bool ret = true;

    if ((WiFi.status() == WL_CONNECTED))
    { //Check the current connection status

        HTTPClient http; // Initialize the client library
        size_t size = 0; // available streaming data size

        http.begin(tts_url); //Specify the URL

        Serial.println();
        Serial.println("Starting connection to tts server...");

        //request header for VoiceText Web API
        String auth = base64::encode(tts_user + ":" + tts_pass);
        http.addHeader("Authorization", "Basic " + auth);
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        String request = String("text=") + URLEncode(text.c_str()) + tts_parms;
        http.addHeader("Content-Length", String(request.length()));

        SPIFFS.begin(true);
        File f = SPIFFS.open("/" + mp3file, FILE_WRITE);
        if (f)
        {
            //Make the request
            int httpCode = http.POST(request);
            if (httpCode > 0)
            {
                if (httpCode == HTTP_CODE_OK)
                {
                    http.writeToStream(&f);
                }
            }
            else
            {
                Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
                ret = false;
            }
            f.close();
        }
        else
        {
            Serial.println("SPIFFS open error!!");
            ret = false;
        }
        http.end();
        SPIFFS.end();
    }
    else
    {
        Serial.println("Error in WiFi connection");
        ret = false;
    }
    Serial.println("text2mp3() end");
    return ret;
}

// from http://hardwarefun.com/tutorials/url-encoding-in-arduino
// modified by chaeplin
String URLEncode(const char *msg)
{
    const char *hex = "0123456789ABCDEF";
    String encodedMsg = "";

    while (*msg != '\0')
    {
        if (('a' <= *msg && *msg <= 'z') || ('A' <= *msg && *msg <= 'Z') ||
            ('0' <= *msg && *msg <= '9') || *msg == '-' || *msg == '_' || *msg == '.' || *msg == '~')
        {
            encodedMsg += *msg;
        }
        else
        {
            encodedMsg += '%';
            encodedMsg += hex[*msg >> 4];
            encodedMsg += hex[*msg & 0xf];
        }
        msg++;
    }
    return encodedMsg;
}
