#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <SpotifyEsp32.h>
#include <SPI.h>
#include <WiFiClientSecure.h>
#include <TJpg_Decoder.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <icons.h>
#include <secrets.h>

Preferences prefs;

// Buffer for Spotify object
static uint8_t spBuf[sizeof(Spotify)];
Spotify& sp = *reinterpret_cast<Spotify*>(spBuf);

#define TFT_CS   2
#define TFT_DC   3
#define TFT_RST  44
#define TFT_MOSI 9
#define TFT_SCLK 7
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// Layout Config
#define TFT_WIDTH         320
#define TFT_HEIGHT        240
#define ALBUM_SIZE        160
#define ALBUM_X           ((TFT_WIDTH - ALBUM_SIZE) / 2)
#define ALBUM_Y           0
#define INFO_Y            160
#define ARTIST_Y          (INFO_Y + 18)
#define PROGRESS_Y        190
#define CONTROLS_Y        200
#define ICON_SIZE         32

// Long text Scroll Config
#define TRACK_TEXT_SIZE   2
#define CHAR_W            12
#define TRACK_MARGIN      10
#define TRACK_REGION_W    (TFT_WIDTH - TRACK_MARGIN * 2)
#define TRACK_ROW_H       16
#define SCROLL_DELAY_MS   30
#define SCROLL_PAUSE_MS   1500

extern const uint16_t playBitmap[16*16];
extern const uint16_t pauseBitmap[16*16];
extern const uint16_t nextBitmap[16*16];
extern const uint16_t prevBitmap[16*16];
extern const uint16_t shuffleBitmap[16*16];

static SemaphoreHandle_t dataMutex;

struct TrackData {
    String track;
    String artist;
    String albumURL;
    bool   playing   = false;
    int    progress  = 0;
    int    duration  = 0;
    bool   changed   = false;
};

static TrackData pending;
static bool      newData = false;

// Display States
static String        lastAlbum        = "";
static String        lastTrack        = "";
static bool          lastPlaying      = false;

static String        scrollTrack      = "";
static int           scrollOffset     = 0;
static bool          scrolling        = false;
static bool          scrollPausing    = true;
static unsigned long lastScrollTime   = 0;
static unsigned long scrollPauseStart = 0;


// RENDERING FUNCTIONS --------------------------------------------------

bool tft_output(short x, short y, unsigned short w, unsigned short h, unsigned short* bitmap){
    tft.drawRGBBitmap(x, y, bitmap, w, h);
    return true;
}

void renderTrackRow(int offset){
    GFXcanvas16 canvas(TFT_WIDTH, TRACK_ROW_H);
    canvas.fillScreen(ST77XX_BLACK);
    canvas.setTextColor(ST77XX_WHITE);
    canvas.setTextSize(TRACK_TEXT_SIZE);
    canvas.setTextWrap(false);
    canvas.setCursor(TRACK_MARGIN - offset, 0);
    canvas.print(scrollTrack);
    tft.drawRGBBitmap(0, INFO_Y, canvas.getBuffer(), TFT_WIDTH, TRACK_ROW_H);
}

void initScroll(String track){
    scrollTrack      = track;
    scrollOffset     = 0;
    scrollPausing    = true;
    scrollPauseStart = millis();
    lastScrollTime   = millis();
    scrolling        = ((int)(track.length() * CHAR_W) > TRACK_REGION_W);
    renderTrackRow(0);
}

void drawArtist(String artist){
    tft.fillRect(0, ARTIST_Y, TFT_WIDTH, 10, ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setTextWrap(false);
    tft.setCursor(TRACK_MARGIN, ARTIST_Y);
    tft.print(artist);
}

void drawProgressBar(int progress, int duration){
    int barWidth = TFT_WIDTH - 40;
    float pct    = duration > 0 ? (float)progress / duration : 0;
    int fill     = (int)(pct * barWidth);
    tft.drawRect(20, PROGRESS_Y, barWidth, 5, ST77XX_WHITE);
    if(fill > 0)
        tft.fillRect(20, PROGRESS_Y, fill, 5, ST77XX_GREEN);
    if(fill < barWidth)
        tft.fillRect(20 + fill, PROGRESS_Y, barWidth - fill, 5, ST77XX_BLACK);
}

void drawBitmap2xCanvas(int x, int y, const uint16_t* bitmap){
    GFXcanvas16 canvas(32, 32);
    canvas.fillScreen(ST77XX_BLACK);
    for(int row = 0; row < 16; row++){
        for(int col = 0; col < 16; col++){
            uint16_t color = bitmap[row * 16 + col];
            if(color != ST77XX_BLACK)
                canvas.fillRect(col*2, row*2, 2, 2, color);
        }
    }
    tft.drawRGBBitmap(x, y, canvas.getBuffer(), 32, 32);
}

void drawControls(bool playing, bool forceRedraw){
    static bool lastDrawnPlaying = !playing;
    int spacing = 60;
    int xStart  = (TFT_WIDTH - (spacing * 3 + 32)) / 2;

    if(forceRedraw){
        tft.fillRect(0, CONTROLS_Y, TFT_WIDTH, 32, ST77XX_BLACK);
        drawBitmap2xCanvas(xStart,             CONTROLS_Y, prevBitmap);
        drawBitmap2xCanvas(xStart + spacing*2, CONTROLS_Y, nextBitmap);
        drawBitmap2xCanvas(xStart + spacing*3, CONTROLS_Y, shuffleBitmap);
    }

    if(forceRedraw || playing != lastDrawnPlaying){
        tft.fillRect(xStart + spacing, CONTROLS_Y, 32, 32, ST77XX_BLACK);
        drawBitmap2xCanvas(xStart + spacing, CONTROLS_Y,
                           playing ? pauseBitmap : playBitmap);
        lastDrawnPlaying = playing;
    }
}

bool drawAlbumFromHTTP(String url){
    if(url == "") return false;
    HTTPClient http;
    http.begin(url);
    if(http.GET() != HTTP_CODE_OK){ http.end(); return false; }
    WiFiClient* stream = http.getStreamPtr();
    int len = http.getSize();
    if(len <= 0){ http.end(); return false; }
    uint8_t* buf = (uint8_t*)malloc(len);
    if(!buf){ http.end(); return false; }
    int idx = 0;
    const int CHUNK = 1024;
    while(http.connected() && idx < len){
        if(stream->available())
            idx += stream->readBytes(&buf[idx], min(CHUNK, len - idx));
    }
    TJpgDec.setJpgScale(2);
    tft.fillRect(0, ALBUM_Y, TFT_WIDTH, ALBUM_SIZE, ST77XX_BLACK);
    TJpgDec.drawJpg(ALBUM_X, ALBUM_Y, buf, len);
    free(buf);
    http.end();
    return true;
}

// Spotify polling: Separate core to avoid blocking
void spotifyTask(void* param){
    for(;;){
        StaticJsonDocument<512> filter;
        filter["item"]["name"]            = true;
        filter["item"]["artists"]         = true;
        filter["item"]["album"]["images"] = true;
        filter["is_playing"]              = true;
        filter["progress_ms"]             = true;
        filter["item"]["duration_ms"]     = true;

        response res = sp.currently_playing(filter);

        if(res.status_code >= 200 && !res.reply.isNull()){
            String track   = res.reply["item"]["name"].as<String>();
            String artist  = res.reply["item"]["artists"][0]["name"].as<String>();
            bool   playing = res.reply["is_playing"].as<bool>();
            int    progress= res.reply["progress_ms"].as<int>();
            int    duration= res.reply["item"]["duration_ms"].as<int>();

            String albumURL = "";
            JsonArray imgs  = res.reply["item"]["album"]["images"].as<JsonArray>();
            if(!imgs.isNull())
                albumURL = imgs[imgs.size() > 1 ? 1 : 0]["url"].as<String>();

            if(xSemaphoreTake(dataMutex, portMAX_DELAY)){
                pending.track    = track;
                pending.artist   = artist;
                pending.albumURL = albumURL;
                pending.playing  = playing;
                pending.progress = progress;
                pending.duration = duration;
                pending.changed  = (track != lastTrack);
                newData          = true;
                xSemaphoreGive(dataMutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// Setup & Loop ------------------------------------------------------------------------

void setup(){
    Serial.begin(115200);
    tft.init(240, 320);
    tft.setRotation(1);
    tft.fillScreen(ST77XX_BLACK);
    TJpgDec.setCallback(tft_output);

    WiFi.begin(SSID, PASSWORD);
    while(WiFi.status() != WL_CONNECTED) delay(500);

    // Load saved token from NVS
    prefs.begin("spotify", false);
    String savedToken = prefs.getString("refresh_token", "");
    prefs.end();

    if(savedToken.length() > 0){
        // Normal boot construct with saved token, no browser needed
        Serial.println("Found saved refresh token, skipping auth.");
        new (spBuf) Spotify(CLIENT_ID, CLIENT_SECRET, savedToken.c_str());
        sp.begin();
    } else {
        // First boot construct without token, run auth flow once
        Serial.println("No token found — starting first-time auth...");
        new (spBuf) Spotify(CLIENT_ID, CLIENT_SECRET);
        sp.begin();
        while(!sp.is_auth()){
            sp.handle_client();
            delay(100);
        }
        // Save token so every future boot skips this
        String token = sp.get_user_tokens().refresh_token;
        prefs.begin("spotify", false);
        prefs.putString("refresh_token", token);
        prefs.end();
        Serial.printf("Refresh token saved to NVS: %s\n", token.c_str());
    }

    dataMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(
        spotifyTask,
        "spotify",
        8192,
        NULL,
        1,
        NULL,
        0     // core 0
    );
}


void loop(){
    unsigned long now = millis();

    // Update data if available
    if(newData && xSemaphoreTake(dataMutex, 0)){
        TrackData local = pending;
        newData = false;
        xSemaphoreGive(dataMutex);

        if(local.changed){
            initScroll(local.track);
            drawArtist(local.artist);
            drawControls(local.playing, true);
        } else {
            drawControls(local.playing, false);
        }

        drawProgressBar(local.progress, local.duration);

        if(local.albumURL != lastAlbum){
            drawAlbumFromHTTP(local.albumURL);
            lastAlbum = local.albumURL;
        }

        lastTrack   = local.track;
        lastPlaying = local.playing;
    }

    // Scroll Title
    if(scrolling){
        if(scrollPausing){
            if(now - scrollPauseStart >= SCROLL_PAUSE_MS){
                scrollPausing  = false;
                lastScrollTime = now;
            }
        } else if(now - lastScrollTime >= SCROLL_DELAY_MS){
            lastScrollTime = now;
            scrollOffset++;
            int maxOffset = (int)(scrollTrack.length() * CHAR_W) - TRACK_REGION_W + TRACK_MARGIN;
            if(scrollOffset > maxOffset){
                scrollOffset     = 0;
                scrollPausing    = true;
                scrollPauseStart = now;
            }
            renderTrackRow(scrollOffset);
        }
    }
}
