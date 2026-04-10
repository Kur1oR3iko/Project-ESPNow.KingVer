#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <FS.h>
#include <SD.h>
#include <time.h>

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(ETOUCH_CS, TOUCH_IRQ);
Preferences prefs;

// --- SD 卡独立 SPI ---
SPIClass sdSPI(HSPI);
const int SD_CS_PIN = 5;
const int SD_SCK_PIN = 18;
const int SD_MISO_PIN = 19;
const int SD_MOSI_PIN = 23;

// --- RGB LED ---
const int PIN_RED = 22;
const int PIN_GREEN = 17;
const int PIN_BLUE = 16;
#define LED_ON  LOW
#define LED_OFF HIGH

// --- 通信指令集 ---
#define CMD_DRAW        1
#define CMD_CLEAR       2
#define CMD_HEARTBEAT   3
#define CMD_REQ_SYNC    4
#define CMD_SYNC_DATA   5
#define CMD_SYNC_CLEAR  6

typedef struct sync_data {
    uint8_t cmd;           
    char sender[16];       
    int16_t x0, y0;        
    int16_t x1, y1;        
    uint16_t color;        
    uint8_t t;             
    uint8_t mac[6]; 
} sync_data;

sync_data outData;
QueueHandle_t dataQueue;

// --- 状态、时间与休眠变量 ---
unsigned long lastFlashTime = 0;
unsigned long lastRecvTime = 0; 
unsigned long lastHeartbeat = 0;
bool flashState = false;
bool sdReady = false; 

unsigned long lastTouchTime = 0;    // 记录最后一次触控时间
unsigned long sleepTimeout = 0;     // 熄屏时间 (毫秒)，0为常亮
bool isScreenOff = false;           // 屏幕当前是否已熄灭

// --- 历史记录缓存 (1500 步，完美平衡内存与画作长度) ---
#define MAX_HISTORY 1500
sync_data history[MAX_HISTORY];
int historyCount = 0;
bool shouldSendSync = false;
unsigned long syncDelayTime = 0;

// --- 在线用户追踪 ---
#define MAX_PEERS 20
struct PeerNode {
    char name[16];
    uint8_t mac[6]; 
    unsigned long lastSeen;
};
PeerNode peers[MAX_PEERS];
int lastOnlineCount = -1;

// --- UI 状态 ---
bool isNaming = false;
bool isViewingOnline = false; 
bool isViewingGallery = false; 
char customName[16] = "";
int nameLen = 0;

#define MAX_BMP_FILES 50
String bmpFiles[MAX_BMP_FILES];
int bmpCount = 0;
int currentBmpIndex = 0;

const char keys[5][8] = {
  {'1','2','3','4','5','6','7','8'},
  {'9','0','A','B','C','D','E','F'},
  {'G','H','I','J','K','L','M','N'},
  {'O','P','Q','R','S','T','U','V'},
  {'W','X','Y','Z','-',' ','<','*'} 
};

// --- 画板状态 ---
uint16_t currentColor = TFT_WHITE;
int currentThickness = 2;
int lastX = -1; int lastY = -1;
bool wasTouched = false;

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;

void drawMainUI();
void showBmp(int index);

// ----------------------------------------------------
// 工具函数
// ----------------------------------------------------
void setRGB(bool r, bool g, bool b) {
    digitalWrite(PIN_RED, r ? LED_ON : LED_OFF);
    digitalWrite(PIN_GREEN, g ? LED_ON : LED_OFF);
    digitalWrite(PIN_BLUE, b ? LED_ON : LED_OFF);
}

uint16_t wheel(byte WheelPos) {
    WheelPos = 255 - WheelPos;
    if (WheelPos < 85) return tft.color565(255 - WheelPos * 3, 0, WheelPos * 3);
    if (WheelPos < 170) { WheelPos -= 85; return tft.color565(0, WheelPos * 3, 255 - WheelPos * 3); }
    WheelPos -= 170; return tft.color565(WheelPos * 3, 255 - WheelPos * 3, 0);
}

void drawThicknessSlider() {
    tft.fillRect(80, 210, 60, 30, TFT_DARKGREY); tft.drawRect(80, 210, 60, 30, TFT_WHITE);
    tft.fillTriangle(85, 230, 135, 230, 135, 215, TFT_WHITE);
    int curX = map(currentThickness, 2, 12, 85, 135);
    tft.fillCircle(curX, 222, 4, TFT_RED);
}

// ----------------------------------------------------
// 虚拟内存无损纯净截屏
// ----------------------------------------------------
void captureScreen() {
    if (!sdReady) {
        setRGB(true, false, false);
        tft.fillRect(80, 0, 35, 20, TFT_RED);
        tft.setTextColor(TFT_WHITE); tft.setTextDatum(MC_DATUM); tft.drawString("ERR", 97, 10);
        delay(1000); setRGB(false, false, false); drawMainUI(); return;
    }
    struct tm timeinfo; char filename[32];
    if (getLocalTime(&timeinfo, 10)) strftime(filename, sizeof(filename), "/%Y%m%d_%H%M%S.bmp", &timeinfo);
    else sprintf(filename, "/IMG_%lu.bmp", millis() / 1000);

    File f = SD.open(filename, FILE_WRITE);
    if (!f) return;

    tft.fillRect(80, 0, 35, 20, TFT_WHITE); tft.setTextColor(TFT_BLACK); tft.drawString("...", 97, 10);
    setRGB(true, true, true); 

    uint32_t w = 320; uint32_t h = 190;
    uint32_t fileSize = 54 + (w * h * 3);
    uint8_t header[54] = {
        'B', 'M', (uint8_t)(fileSize), (uint8_t)(fileSize >> 8), (uint8_t)(fileSize >> 16), (uint8_t)(fileSize >> 24),
        0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, (uint8_t)(w), (uint8_t)(w >> 8), 0, 0,
        (uint8_t)(h), (uint8_t)(h >> 8), 0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    f.write(header, 54);

    int chunkHeight = 10; int chunks = h / chunkHeight; 
    TFT_eSprite spr = TFT_eSprite(&tft); spr.setColorDepth(16); spr.createSprite(w, chunkHeight);
    uint8_t rgb[3];

    for (int chunk = chunks - 1; chunk >= 0; chunk--) {
        int canvasYStart = 20 + chunk * chunkHeight; 
        spr.fillSprite(TFT_BLACK);

        for (int i = 0; i < historyCount; i++) {
            int x0 = history[i].x0; int y0 = history[i].y0 - canvasYStart;
            int x1 = history[i].x1; int y1 = history[i].y1 - canvasYStart;
            int t = history[i].t; uint16_t c = history[i].color;
            int dx = abs(x1 - x0); int dy = abs(y1 - y0); int dist = max(dx, dy);
            
            if (dist == 0) spr.fillCircle(x0, y0, t, c);
            else {
                int stepSize = max(1, t / 2); int steps = dist / stepSize;
                if (steps == 0) steps = 1;
                for (int s = 0; s <= steps; s++) spr.fillCircle(x0 + (x1 - x0) * s / steps, y0 + (y1 - y0) * s / steps, t, c);
            }
        }
        for (int sy = chunkHeight - 1; sy >= 0; sy--) {
            for (int sx = 0; sx < w; sx++) {
                uint16_t color = spr.readPixel(sx, sy);
                rgb[0] = (color & 0x001F) << 3; rgb[1] = (color & 0x07E0) >> 3; rgb[2] = (color & 0xF800) >> 8;
                f.write(rgb, 3);
            }
        }
    }
    spr.deleteSprite(); f.close();
    setRGB(false, false, false); drawMainUI(); 
}

// ----------------------------------------------------
// 相册逻辑
// ----------------------------------------------------
void loadBmpList() {
    bmpCount = 0; File root = SD.open("/"); if (!root) return;
    File file = root.openNextFile();
    while (file && bmpCount < MAX_BMP_FILES) {
        String name = file.name();
        if (!file.isDirectory() && name.endsWith(".bmp")) {
            if (!name.startsWith("/")) name = "/" + name;
            bmpFiles[bmpCount++] = name;
        }
        file = root.openNextFile();
    }
}

void showBmp(int index) {
    tft.fillScreen(TFT_BLACK);
    if (bmpCount == 0) {
        tft.setTextColor(TFT_WHITE); tft.setTextSize(2); tft.setTextDatum(MC_DATUM);
        tft.drawString("NO IMAGES ON SD", 160, 120);
    } else {
        File f = SD.open(bmpFiles[index], FILE_READ);
        if (f) {
            f.seek(18); uint32_t bmpW = 0, bmpH = 0; f.read((uint8_t*)&bmpW, 4); f.read((uint8_t*)&bmpH, 4); f.seek(54); 
            uint16_t *lineBuffer = new uint16_t[bmpW]; uint8_t *rgbBuffer = new uint8_t[bmpW * 3];
            int startY = 30; if (bmpH < 240) startY = (240 - bmpH) / 2;

            for (int y = bmpH - 1; y >= 0; y--) {
                f.read(rgbBuffer, bmpW * 3);
                for (int x = 0; x < bmpW; x++) lineBuffer[x] = tft.color565(rgbBuffer[x*3+2], rgbBuffer[x*3+1], rgbBuffer[x*3]);
                tft.pushImage(0, startY + y, bmpW, 1, lineBuffer);
            }
            delete[] lineBuffer; delete[] rgbBuffer; f.close();
            
            tft.fillRect(0, 185, 320, 15, TFT_DARKGREY);
            tft.setTextColor(TFT_YELLOW); tft.setTextSize(1); tft.setTextDatum(MC_DATUM);
            tft.drawString(String(index + 1) + " / " + String(bmpCount) + " : " + bmpFiles[index], 160, 192);
        }
    }
    
    tft.fillRect(0, 0, 320, 30, TFT_BLACK);
    tft.fillRect(260, 0, 60, 30, TFT_RED); tft.drawRect(260, 0, 60, 30, TFT_WHITE);
    tft.setTextColor(TFT_WHITE); tft.setTextSize(1); tft.setTextDatum(MC_DATUM); tft.drawString("DEL", 290, 15);

    tft.fillRect(0, 200, 100, 40, TFT_DARKGREY); tft.drawRect(0, 200, 100, 40, TFT_WHITE);
    tft.setTextColor(TFT_WHITE); tft.setTextSize(2); tft.setTextDatum(MC_DATUM); tft.drawString("< PREV", 50, 220);
    tft.fillRect(100, 200, 120, 40, TFT_RED); tft.drawRect(100, 200, 120, 40, TFT_WHITE); tft.drawString("CLOSE", 160, 220);
    tft.fillRect(220, 200, 100, 40, TFT_DARKGREY); tft.drawRect(220, 200, 100, 40, TFT_WHITE); tft.drawString("NEXT >", 270, 220);
}

// ----------------------------------------------------
// 网络支撑逻辑
// ----------------------------------------------------
void updatePeer(const char* name, const uint8_t* mac) {
    unsigned long now = millis();
    for(int i=0; i<MAX_PEERS; i++) {
        if (strcmp(peers[i].name, name) == 0) { peers[i].lastSeen = now; memcpy(peers[i].mac, mac, 6); return; }
    }
    int oldestIdx = 0;
    for(int i=0; i<MAX_PEERS; i++) {
        if (peers[i].name[0] == '\0') {
            strcpy(peers[i].name, name); memcpy(peers[i].mac, mac, 6); peers[i].lastSeen = now; return;
        }
        if (peers[i].lastSeen < peers[oldestIdx].lastSeen) oldestIdx = i;
    }
    strcpy(peers[oldestIdx].name, name); memcpy(peers[oldestIdx].mac, mac, 6); peers[oldestIdx].lastSeen = now;
}

int getOnlineCount() {
    int count = 0; unsigned long now = millis();
    for(int i=0; i<MAX_PEERS; i++) { if (peers[i].name[0] != '\0' && (now - peers[i].lastSeen < 8000)) count++; }
    return count;
}

void addToHistory(sync_data& data) {
    if (historyCount < MAX_HISTORY) history[historyCount++] = data;
    else { memmove(&history[0], &history[1], sizeof(sync_data) * (MAX_HISTORY - 1)); history[MAX_HISTORY - 1] = data; }
}

// 修复连线渲染
void drawSmoothLine(int x0, int y0, int x1, int y1, int t, uint16_t c) {
    int dx = abs(x1 - x0); int dy = abs(y1 - y0); int dist = max(dx, dy);
    if (dist == 0) { tft.fillCircle(x0, y0, t, c); return; }
    int stepSize = max(1, t / 2); int steps = dist / stepSize;
    if (steps == 0) steps = 1;
    for (int i = 0; i <= steps; i++) {
        int cx = x0 + (x1 - x0) * i / steps; int cy = y0 + (y1 - y0) * i / steps;
        tft.fillCircle(cx, cy, t, c);
    }
}

// ----------------------------------------------------
// UI 组件
// ----------------------------------------------------
void drawKeyboard() {
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(0, 0, 320, 40, TFT_DARKGREY);
    tft.setTextColor(TFT_YELLOW); tft.setTextSize(2); tft.setTextDatum(MC_DATUM);
    if (nameLen == 0) tft.drawString("ENTER YOUR NAME", 160, 20); else tft.drawString(customName, 160, 20);
    tft.setTextColor(TFT_WHITE);
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 8; c++) {
            int kx = c * 40 + 2; int ky = 40 + r * 32; char k = keys[r][c];
            if (k != '*') {
                tft.drawRect(kx, ky, 36, 28, TFT_WHITE); char str[3] = {k, '\0'};
                if (k == '<') strcpy(str, "<-"); if (k == ' ') strcpy(str, "_");
                tft.drawString(str, kx + 18, ky + 14);
            }
        }
    }
    tft.fillRect(0, 200, 320, 40, TFT_GREEN); tft.drawRect(0, 200, 320, 40, TFT_WHITE);
    tft.setTextColor(TFT_BLACK); tft.drawString("JOIN CLASS", 160, 220);
}

void drawOnlineList() {
    tft.fillScreen(TFT_BLACK); tft.fillRect(0, 0, 320, 30, TFT_DARKGREY);
    tft.setTextColor(TFT_YELLOW); tft.setTextSize(2); tft.setTextDatum(MC_DATUM);
    tft.drawString("Class Roster (Online)", 160, 15);
    tft.setTextSize(1); tft.setTextDatum(ML_DATUM);
    int yPos = 40; unsigned long now = millis();
    for(int i=0; i<MAX_PEERS; i++) {
        if (peers[i].name[0] != '\0' && (now - peers[i].lastSeen < 8000)) {
            char macStr[20]; sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", peers[i].mac[0], peers[i].mac[1], peers[i].mac[2], peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
            String displayStr = String(peers[i].name) + "   |   " + macStr;
            tft.setTextColor(TFT_GREEN); tft.drawString(displayStr, 20, yPos); yPos += 15;
            if (yPos > 190) break; 
        }
    }
    tft.fillRect(0, 200, 320, 40, TFT_RED); tft.drawRect(0, 200, 320, 40, TFT_WHITE);
    tft.setTextColor(TFT_WHITE); tft.setTextSize(2); tft.setTextDatum(MC_DATUM); tft.drawString("CLOSE & RETURN", 160, 220);
}

void drawMainUI() {
    tft.fillScreen(TFT_BLACK);
    
    // --- 顶部状态栏重新排版 ---
    tft.fillRect(0, 0, 320, 20, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE); tft.setTextSize(1); tft.setTextDatum(ML_DATUM);
    tft.drawString(String("Me: ") + outData.sender, 5, 10);
    
    tft.fillRect(80, 0, 35, 20, TFT_ORANGE); tft.drawRect(80, 0, 35, 20, TFT_WHITE);
    tft.setTextColor(TFT_BLACK); tft.setTextDatum(MC_DATUM); tft.drawString("SNP", 97, 10);

    tft.fillRect(120, 0, 35, 20, TFT_CYAN); tft.drawRect(120, 0, 35, 20, TFT_WHITE);
    tft.setTextColor(TFT_BLACK); tft.drawString("GAL", 137, 10);

    // 新增：休眠设置按钮 (SLP)
    tft.fillRect(160, 0, 35, 20, TFT_PURPLE); tft.drawRect(160, 0, 35, 20, TFT_WHITE);
    tft.setTextColor(TFT_WHITE);
    String slpStr = "ON";
    if (sleepTimeout == 30000) slpStr = "30s";
    else if (sleepTimeout == 60000) slpStr = "1m";
    else if (sleepTimeout == 300000) slpStr = "5m";
    tft.drawString(slpStr, 177, 10);

    // 底部工具栏
    tft.fillRect(0, 210, 35, 30, TFT_RED); tft.drawRect(0, 210, 35, 30, TFT_WHITE);
    tft.setTextColor(TFT_WHITE); tft.drawString("CLR", 17, 225);

    tft.fillRect(40, 210, 35, 30, TFT_DARKGREY); tft.drawRect(40, 210, 35, 30, TFT_WHITE);
    tft.drawString("ERA", 57, 225);

    drawThicknessSlider();

    for (int i = 0; i < 175; i++) { uint16_t c = wheel((i * 255) / 174); tft.drawFastVLine(145 + i, 210, 30, c); }
    tft.drawRect(144, 209, 177, 32, TFT_WHITE); 

    for(int i = 0; i < historyCount; i++) drawSmoothLine(history[i].x0, history[i].y0, history[i].x1, history[i].y1, history[i].t, history[i].color);
    lastOnlineCount = -1; 
}

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *incomingDataPtr, int len) {
    sync_data packet; memcpy(&packet, incomingDataPtr, sizeof(packet));
    if (info != NULL) memcpy(packet.mac, info->src_addr, 6);
    xQueueSendFromISR(dataQueue, &packet, NULL);
}

void setup() {
    Serial.begin(115200);

    pinMode(PIN_RED, OUTPUT); pinMode(PIN_GREEN, OUTPUT); pinMode(PIN_BLUE, OUTPUT);
    pinMode(21, OUTPUT); digitalWrite(21, HIGH); // 初始化背光
    tft.init(); tft.setRotation(1); tft.invertDisplay(false);

    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, ETOUCH_CS);
    ts.begin(touchSPI); ts.setRotation(1);

    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (SD.begin(SD_CS_PIN, sdSPI, 20000000)) { sdReady = true; } 
    else { sdReady = false; }

    dataQueue = xQueueCreate(100, sizeof(sync_data));
    for(int i=0; i<MAX_PEERS; i++) peers[i].name[0] = '\0';

    WiFi.mode(WIFI_STA); WiFi.disconnect();
    if (esp_now_init() != ESP_OK) return;
    esp_now_register_recv_cb(OnDataRecv);

    memset(&peerInfo, 0, sizeof(peerInfo)); memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0; peerInfo.encrypt = false; esp_now_add_peer(&peerInfo);

    prefs.begin("chat_app", false);
    sleepTimeout = prefs.getUInt("sleep", 0); // 读取熄屏时间，默认常亮

    String savedName = prefs.getString("username", "");
    if (savedName.length() > 0) {
        strcpy(outData.sender, savedName.c_str()); strcpy(customName, savedName.c_str());
        nameLen = strlen(customName); isNaming = false; drawMainUI();
        outData.cmd = CMD_REQ_SYNC; esp_now_send(broadcastAddress, (uint8_t*)&outData, sizeof(outData));
    } else {
        isNaming = true; drawKeyboard();
    }
    
    lastTouchTime = millis();
}

void loop() {
    // 1. 网络数据处理
    sync_data packet;
    while (xQueueReceive(dataQueue, &packet, 0) == pdTRUE) {
        if (packet.cmd != CMD_REQ_SYNC) updatePeer(packet.sender, packet.mac); 

        if (packet.cmd == CMD_DRAW || packet.cmd == CMD_SYNC_DATA) {
            addToHistory(packet); 
            if (!isNaming && !isViewingOnline && !isViewingGallery) drawSmoothLine(packet.x0, packet.y0, packet.x1, packet.y1, packet.t, packet.color);
            if (packet.cmd == CMD_DRAW) {
                lastRecvTime = millis();
                
                // 收到他人画画信息，智能亮屏唤醒
                if (isScreenOff) {
                    digitalWrite(21, HIGH);
                    isScreenOff = false;
                    lastTouchTime = millis();
                }

                if (!isNaming && !isViewingOnline && !isViewingGallery) {
                    tft.fillRect(200, 0, 65, 20, TFT_DARKGREY);
                    tft.setTextColor(TFT_YELLOW); tft.setTextSize(1); tft.setTextDatum(ML_DATUM);
                    tft.drawString(String("By:") + packet.sender, 202, 10);
                }
            }
            if (packet.cmd == CMD_SYNC_DATA || packet.cmd == CMD_SYNC_CLEAR) shouldSendSync = false; 
        } 
        else if (packet.cmd == CMD_CLEAR || packet.cmd == CMD_SYNC_CLEAR) {
            historyCount = 0; 
            if (!isNaming && !isViewingOnline && !isViewingGallery) tft.fillRect(0, 20, 320, 190, TFT_BLACK);
            if (packet.cmd == CMD_SYNC_CLEAR) shouldSendSync = false;
        } 
        else if (packet.cmd == CMD_REQ_SYNC) {
            if (historyCount > 0 && !isNaming) { shouldSendSync = true; syncDelayTime = millis() + random(100, 600); }
        }
    }

    // 2. 发送历史缓存
    if (shouldSendSync && millis() > syncDelayTime) {
        shouldSendSync = false; 
        sync_data clrData; clrData.cmd = CMD_SYNC_CLEAR; 
        esp_now_send(broadcastAddress, (uint8_t*)&clrData, sizeof(clrData)); delay(10);
        for(int i = 0; i < historyCount; i++) {
            sync_data p = history[i]; p.cmd = CMD_SYNC_DATA; 
            esp_now_send(broadcastAddress, (uint8_t*)&p, sizeof(p)); delay(3); 
        }
    }

    // 3. 心跳、在线监测与灯光控制
    if (millis() - lastHeartbeat >= 2500 && nameLen > 0) {
        lastHeartbeat = millis(); outData.cmd = CMD_HEARTBEAT;
        esp_now_send(broadcastAddress, (uint8_t*)&outData, sizeof(outData));
    }

    if (!isNaming && !isViewingOnline && !isViewingGallery) {
        int currentOnline = getOnlineCount();
        if (currentOnline != lastOnlineCount) {
            lastOnlineCount = currentOnline;
            tft.fillRect(270, 0, 50, 20, TFT_DARKGREY);
            tft.setTextColor(TFT_GREEN); tft.setTextDatum(MR_DATUM);
            tft.drawString(String("On:") + currentOnline, 315, 10);
        }
    }

    if (millis() - lastRecvTime < 1000) {
        if (millis() - lastFlashTime >= 150) {
            lastFlashTime = millis(); flashState = !flashState; setRGB(flashState, !flashState, false);
        }
    } else {
        int phase = (millis() / 2000) % 3; setRGB(phase == 0, phase == 1, phase == 2);
    }

    // 4. 触控中心与息屏唤醒逻辑
    if (ts.touched()) {
        lastTouchTime = millis(); // 更新最后触摸时间
        
        // --- 屏幕唤醒拦截 ---
        if (isScreenOff) {
            digitalWrite(21, HIGH); // 点亮背光
            isScreenOff = false;
            delay(300); // 延时防抖，避免点亮屏幕的瞬间画出一个点
            return; 
        }

        TS_Point p = ts.getPoint();
        int x = map(p.x, 300, 3800, 0, 320); int y = map(p.y, 300, 3800, 0, 240);
        
        // --- 键盘模式 ---
        if (isNaming) {
            if (y > 40 && y < 200) {
                int r = (y - 40) / 32; int c = x / 40;
                if (c < 8 && r < 5) {
                    char k = keys[r][c]; if (k == '*') return;
                    if (k == '<' && nameLen > 0) customName[--nameLen] = '\0';
                    else if (k == ' ' && nameLen < 14) { customName[nameLen++] = ' '; customName[nameLen] = '\0'; }
                    else if (k != '<' && k != ' ' && nameLen < 14) { customName[nameLen++] = k; customName[nameLen] = '\0'; }
                    tft.fillRect(0, 0, 320, 40, TFT_DARKGREY);
                    tft.setTextColor(TFT_YELLOW); tft.setTextDatum(MC_DATUM); tft.drawString(customName, 160, 20); delay(200);
                }
            } else if (y >= 200 && nameLen > 0) {
                strcpy(outData.sender, customName); prefs.putString("username", customName);
                isNaming = false; drawMainUI(); delay(300);
                outData.cmd = CMD_REQ_SYNC; esp_now_send(broadcastAddress, (uint8_t*)&outData, sizeof(outData));
            }
            return;
        }

        // --- 名单模式 ---
        if (isViewingOnline) {
            if (y > 200) { isViewingOnline = false; drawMainUI(); delay(300); }
            return;
        }

        // --- 相册模式 ---
        if (isViewingGallery) {
            if (y < 40) {
                if (x > 250) { 
                    if(bmpCount > 0) {
                        SD.remove(bmpFiles[currentBmpIndex]); loadBmpList();
                        if(bmpCount > 0) { currentBmpIndex = 0; showBmp(currentBmpIndex); }
                        else { isViewingGallery = false; drawMainUI(); }
                    }
                    delay(300);
                }
            } else if (y > 200) {
                if (x < 100) { 
                    currentBmpIndex--; if (currentBmpIndex < 0) currentBmpIndex = bmpCount - 1;
                    if (bmpCount > 0) showBmp(currentBmpIndex); delay(300);
                } else if (x >= 100 && x <= 220) { 
                    isViewingGallery = false; drawMainUI(); delay(300);
                } else if (x > 220) { 
                    currentBmpIndex++; if (currentBmpIndex >= bmpCount) currentBmpIndex = 0;
                    if (bmpCount > 0) showBmp(currentBmpIndex); delay(300);
                }
            }
            return; 
        }

        // --- 主画板交互与动态空气墙 ---
        if (y < 20) { // 顶部
            if (x < 75) { isNaming = true; drawKeyboard(); delay(300); } 
            else if (x >= 80 && x < 115) { captureScreen(); delay(500); } 
            else if (x >= 120 && x < 155) {
                if (sdReady) { loadBmpList(); isViewingGallery = true; showBmp(currentBmpIndex); delay(300); } 
                else { tft.fillRect(120, 0, 35, 20, TFT_RED); tft.setTextColor(TFT_WHITE); tft.setTextDatum(MC_DATUM); tft.drawString("ERR", 137, 10); delay(1000); drawMainUI(); }
            } 
            else if (x >= 160 && x < 195) { 
                // 点击 SLP 按钮，循环切换休眠时间
                if (sleepTimeout == 0) sleepTimeout = 30000;          // 30秒
                else if (sleepTimeout == 30000) sleepTimeout = 60000; // 1分钟
                else if (sleepTimeout == 60000) sleepTimeout = 300000;// 5分钟
                else sleepTimeout = 0;                                // 常亮
                
                prefs.putUInt("sleep", sleepTimeout);
                drawMainUI(); // 刷新 UI 按钮文字
                delay(300);
            }
            else if (x >= 265) { isViewingOnline = true; drawOnlineList(); delay(300); }
            return;
        } 
        else if (y >= 210) { // 底部
            if (x < 35) { 
                tft.fillRect(0, 20, 320, 190, TFT_BLACK); historyCount = 0;
                outData.cmd = CMD_CLEAR; esp_now_send(broadcastAddress, (uint8_t*)&outData, sizeof(outData)); delay(200);
            } else if (x >= 40 && x < 75) { currentColor = TFT_BLACK; currentThickness = 12; 
            } else if (x >= 80 && x <= 140) {
                currentThickness = constrain(map(x, 80, 140, 2, 12), 2, 12);
                if (currentColor == TFT_BLACK) currentColor = TFT_WHITE; 
                drawThicknessSlider(); 
            } else if (x >= 145) { 
                int pos = (x - 145) * 255 / 174; if (pos > 255) pos = 255;
                currentColor = wheel(pos);
            }
            wasTouched = false; 
        } 
        else { // 绘画区 (动态约束边界，防止污染功能区)
            int safeX = constrain(x, currentThickness, 319 - currentThickness);
            int safeY = constrain(y, 20 + currentThickness, 209 - currentThickness);

            if (wasTouched) {
                if (abs(safeX - lastX) > 80 || abs(safeY - lastY) > 80) { lastX = safeX; lastY = safeY; }
                else if (safeX != lastX || safeY != lastY) {
                    drawSmoothLine(lastX, lastY, safeX, safeY, currentThickness, currentColor);
                    outData.cmd = CMD_DRAW;
                    outData.x0 = lastX; outData.y0 = lastY; outData.x1 = safeX; outData.y1 = safeY;
                    outData.color = currentColor; outData.t = currentThickness;
                    addToHistory(outData); esp_now_send(broadcastAddress, (uint8_t*)&outData, sizeof(outData));
                    lastX = safeX; lastY = safeY; 
                }
            } else { 
                tft.fillCircle(safeX, safeY, currentThickness, currentColor); lastX = safeX; lastY = safeY; 
                outData.cmd = CMD_DRAW;
                outData.x0 = safeX; outData.y0 = safeY; outData.x1 = safeX; outData.y1 = safeY;
                outData.color = currentColor; outData.t = currentThickness;
                addToHistory(outData); esp_now_send(broadcastAddress, (uint8_t*)&outData, sizeof(outData));
            }
            wasTouched = true; delay(2); 
        }
    } else {
        wasTouched = false; // 手指离开屏幕
        
        // --- 屏幕超时自动熄屏检测 ---
        if (!isScreenOff && sleepTimeout > 0 && (millis() - lastTouchTime > sleepTimeout)) {
            digitalWrite(21, LOW); // 关闭背光
            isScreenOff = true;
        }
    }
}