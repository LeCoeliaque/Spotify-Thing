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
#include <icons.h>
#include <secrets.h>

Spotify sp(CLIENT_ID, CLIENT_SECRET, REFRESH_TOKEN);


// Button Pins
#define BTN_LEFT   5
#define BTN_MIDDLE 6
#define BTN_RIGHT  43

#define DEBOUNCE_MS   30
#define LONG_PRESS_MS 500

// Display Pins
#define TFT_CS   2
#define TFT_DC   3
#define TFT_RST  44
#define TFT_MOSI 9
#define TFT_SCLK 7
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// Layout Config
#define TFT_WIDTH       320
#define TFT_HEIGHT      240
#define ALBUM_SIZE      160
#define ALBUM_X         ((TFT_WIDTH - ALBUM_SIZE) / 2)
#define ALBUM_Y         0
#define INFO_Y          160
#define ARTIST_Y        (INFO_Y + 18)
#define PROGRESS_Y      190
#define CONTROLS_Y      200
#define TRACK_TEXT_SIZE 2
#define CHAR_W          12
#define TRACK_MARGIN    10
#define TRACK_REGION_W  (TFT_WIDTH - TRACK_MARGIN * 2)
#define TRACK_ROW_H     16
#define SCROLL_DELAY_MS 30
#define SCROLL_PAUSE_MS 1500

#define MENU_ITEM_H   30
#define MENU_VISIBLE  7
#define INACTIVITY_MS 60000UL

extern const uint16_t playBitmap[16*16];
extern const uint16_t pauseBitmap[16*16];
extern const uint16_t nextBitmap[16*16];
extern const uint16_t prevBitmap[16*16];
extern const uint16_t shuffleBitmap[16*16];

// State Control
enum Screen { SCREEN_PLAYER, SCREEN_PLAYLIST };
static Screen currentScreen = SCREEN_PLAYER;
static unsigned long lastInteraction = 0;
static unsigned long lastPollTime    = 0;
#define POLL_INTERVAL_MS 3000

static String lastTrack   = "";
static String lastAlbum   = "";
static bool   lastPlaying = false;
static bool   shuffleActive = false;

static String        scrollTrack      = "";
static int           scrollOffset     = 0;
static bool          scrolling        = false;
static bool          scrollPausing    = true;
static unsigned long lastScrollTime   = 0;
static unsigned long scrollPauseStart = 0;

#define MAX_PLAYLISTS 20
struct PlaylistEntry { String name; String uri; };
static PlaylistEntry playlists[MAX_PLAYLISTS];
static int  playlistCount = 0;
static int  menuSelected  = 0;
static int  menuScrollTop = 0;

// Button Logic
static unsigned long btnPressTime[3] = {0,0,0};
static bool          btnDown[3]      = {false,false,false};
static bool          btnLongFired[3] = {false,false,false};
const int BTN_PINS[3] = { BTN_LEFT, BTN_MIDDLE, BTN_RIGHT };

int readBtn(int idx){
    bool pressed = (digitalRead(BTN_PINS[idx]) == LOW);
    unsigned long now = millis();
    if(pressed && !btnDown[idx]){
        btnDown[idx]=true; btnPressTime[idx]=now; btnLongFired[idx]=false;
        return 0;
    }
    if(pressed && btnDown[idx] && !btnLongFired[idx]){
        if(now - btnPressTime[idx] >= LONG_PRESS_MS){ btnLongFired[idx]=true; return 2; }
        return 0;
    }
    if(!pressed && btnDown[idx]){
        btnDown[idx]=false;
        if(!btnLongFired[idx] && (now - btnPressTime[idx] >= DEBOUNCE_MS)) return 1;
    }
    return 0;
}

void recordInteraction(){ lastInteraction = millis(); }

// Rendering
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
    if(fill > 0) tft.fillRect(20, PROGRESS_Y, fill, 5, ST77XX_GREEN);
    if(fill < barWidth) tft.fillRect(20 + fill, PROGRESS_Y, barWidth - fill, 5, ST77XX_BLACK);
}

void drawBitmap2xCanvas(int x, int y, const uint16_t* bitmap, uint16_t tint = ST77XX_WHITE){
    GFXcanvas16 canvas(32, 32);
    canvas.fillScreen(ST77XX_BLACK);
    for(int row = 0; row < 16; row++)
        for(int col = 0; col < 16; col++){
            uint16_t color = bitmap[row*16+col];
            if(color != ST77XX_BLACK) canvas.fillRect(col*2, row*2, 2, 2, tint);
        }
    tft.drawRGBBitmap(x, y, canvas.getBuffer(), 32, 32);
}

void drawControls(bool playing, bool forceRedraw){
    static bool lastDrawnPlaying = !playing;
    int spacing = 60;
    int xStart  = (TFT_WIDTH - (spacing * 3 + 32)) / 2;
    if(forceRedraw){
        tft.fillRect(0, CONTROLS_Y, TFT_WIDTH, 32, ST77XX_BLACK);
        drawBitmap2xCanvas(xStart,            CONTROLS_Y, prevBitmap);
        drawBitmap2xCanvas(xStart+spacing*2,  CONTROLS_Y, nextBitmap);
        drawBitmap2xCanvas(xStart+spacing*3,  CONTROLS_Y, shuffleBitmap,
                           shuffleActive ? ST77XX_GREEN : ST77XX_WHITE);
    }
    if(forceRedraw || playing != lastDrawnPlaying){
        tft.fillRect(xStart+spacing, CONTROLS_Y, 32, 32, ST77XX_BLACK);
        drawBitmap2xCanvas(xStart+spacing, CONTROLS_Y, playing ? pauseBitmap : playBitmap);
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
    while(http.connected() && idx < len)
        if(stream->available())
            idx += stream->readBytes(&buf[idx], min(CHUNK, len-idx));
    TJpgDec.setJpgScale(2);
    tft.fillRect(0, ALBUM_Y, TFT_WIDTH, ALBUM_SIZE, ST77XX_BLACK);
    TJpgDec.drawJpg(ALBUM_X, ALBUM_Y, buf, len);
    free(buf);
    http.end();
    return true;
}

void drawPlaylistMenu(){
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_GREEN);
    tft.setTextSize(1);
    tft.setCursor(TRACK_MARGIN, 4);
    tft.print("PLAYLISTS");
    tft.setTextColor(0x7BEF);
    tft.setCursor(4, TFT_HEIGHT - 10);
    tft.print("<up  [select]  dn>   hold> back");
    tft.drawFastHLine(0, 15, TFT_WIDTH, ST77XX_WHITE);
    tft.drawFastHLine(0, TFT_HEIGHT - 13, TFT_WIDTH, 0x7BEF);
    if(playlistCount == 0){
        tft.setTextColor(ST77XX_WHITE);
        tft.setCursor(TRACK_MARGIN, 50);
        tft.print("No playlists found.");
        return;
    }
    int visibleEnd = min(menuScrollTop + MENU_VISIBLE, playlistCount);
    for(int i = menuScrollTop; i < visibleEnd; i++){
        int y    = 18 + (i - menuScrollTop) * MENU_ITEM_H;
        bool sel = (i == menuSelected);
        tft.fillRect(0, y, TFT_WIDTH-6, MENU_ITEM_H-2, sel ? ST77XX_GREEN : ST77XX_BLACK);
        tft.setTextColor(sel ? ST77XX_BLACK : ST77XX_WHITE);
        tft.setTextSize(1);
        tft.setTextWrap(false);
        tft.setCursor(TRACK_MARGIN, y+9);
        String name = playlists[i].name;
        int maxChars = (TFT_WIDTH - TRACK_MARGIN*2 - 6) / 6;
        if((int)name.length() > maxChars) name = name.substring(0, maxChars-2) + "..";
        tft.print(name);
    }
    if(playlistCount > MENU_VISIBLE){
        int trackH = TFT_HEIGHT - 30;
        int thumbH = max(6, trackH * MENU_VISIBLE / playlistCount);
        int thumbY = 16 + trackH * menuScrollTop / playlistCount;
        tft.drawFastVLine(TFT_WIDTH-4, 16, trackH, 0x7BEF);
        tft.fillRect(TFT_WIDTH-5, thumbY, 4, thumbH, ST77XX_GREEN);
    }
}

// Spotify Update
void pollSpotify(){
    StaticJsonDocument<512> filter;
    filter["item"]["name"]            = true;
    filter["item"]["artists"]         = true;
    filter["item"]["album"]["images"] = true;
    filter["is_playing"]              = true;
    filter["progress_ms"]             = true;
    filter["item"]["duration_ms"]     = true;

    response res = sp.currently_playing(filter);
    if(res.status_code < 200 || res.reply.isNull()) return;

    String track    = res.reply["item"]["name"].as<String>();
    String artist   = res.reply["item"]["artists"][0]["name"].as<String>();
    bool   playing  = res.reply["is_playing"].as<bool>();
    int    progress = res.reply["progress_ms"].as<int>();
    int    duration = res.reply["item"]["duration_ms"].as<int>();

    String albumURL = "";
    JsonArray imgs  = res.reply["item"]["album"]["images"].as<JsonArray>();
    if(!imgs.isNull())
        albumURL = imgs[imgs.size() > 1 ? 1 : 0]["url"].as<String>();

    bool trackChanged = (track != lastTrack);

    if(trackChanged){
        initScroll(track);
        drawArtist(artist);
        drawControls(playing, true);
    } else {
        drawControls(playing, false);
    }

    drawProgressBar(progress, duration);

    if(albumURL != lastAlbum){
        drawAlbumFromHTTP(albumURL);
        lastAlbum = albumURL;
    }

    lastTrack   = track;
    lastPlaying = playing;
}

void loadPlaylists(){
    playlistCount = 0;
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setCursor(TRACK_MARGIN, TFT_HEIGHT/2 - 4);
    tft.print("Loading playlists...");

    StaticJsonDocument<256> filter;
    filter["items"][0]["name"] = true;
    filter["items"][0]["uri"]  = true;

    response res = sp.get_current_users_playlists(MAX_PLAYLISTS, 0, filter);
    if(res.status_code >= 200 && !res.reply.isNull()){
        JsonArray items = res.reply["items"].as<JsonArray>();
        for(JsonVariant item : items){
            if(playlistCount >= MAX_PLAYLISTS) break;
            playlists[playlistCount].name = item["name"].as<String>();
            playlists[playlistCount].uri  = item["uri"].as<String>();
            playlistCount++;
        }
    }
}

// Transition Screens
void enterPlaylistMenu(){
    currentScreen = SCREEN_PLAYLIST;
    menuSelected  = 0;
    menuScrollTop = 0;
    loadPlaylists();
    drawPlaylistMenu();
}

void returnToPlayer(){
    currentScreen = SCREEN_PLAYER;
    lastAlbum = "";
    lastTrack = "";
    tft.fillScreen(ST77XX_BLACK);
    lastPollTime = 0;  // force immediate poll
}

// Button Press Handling
void handlePlayerButtons(){
    int l = readBtn(0);
    int m = readBtn(1);
    int r = readBtn(2);

    if(l == 1){ recordInteraction(); sp.previous(); lastPollTime = 0; }
    if(l == 2){ recordInteraction(); enterPlaylistMenu(); }

    if(m == 1){
        recordInteraction();
        if(lastPlaying) sp.pause_playback();
        else            sp.start_resume_playback();
        lastPlaying = !lastPlaying;
        drawControls(lastPlaying, false);
    }
    if(m == 2){
        recordInteraction();
        shuffleActive = !shuffleActive;
        sp.shuffle(shuffleActive);
        drawControls(lastPlaying, true);
    }

    if(r == 1){ recordInteraction(); sp.skip(); lastPollTime = 0; }
}

void handlePlaylistButtons(){
    int l = readBtn(0);
    int m = readBtn(1);
    int r = readBtn(2);

    if(l == 1){
        recordInteraction();
        if(menuSelected > 0){
            menuSelected--;
            if(menuSelected < menuScrollTop) menuScrollTop = menuSelected;
            drawPlaylistMenu();
        }
    }
    if(m == 1 && playlistCount > 0){
        recordInteraction();
        sp.start_resume_playback(playlists[menuSelected].uri.c_str(), 0, 0);
        delay(300);
        returnToPlayer();
    }
    if(r == 1){
        recordInteraction();
        if(menuSelected < playlistCount - 1){
            menuSelected++;
            if(menuSelected >= menuScrollTop + MENU_VISIBLE)
                menuScrollTop = menuSelected - MENU_VISIBLE + 1;
            drawPlaylistMenu();
        }
    }
    if(r == 2){ recordInteraction(); returnToPlayer(); }
}


// Setup & Loop -------------------------------------------------------------------------------------
void setup(){
    Serial.begin(115200);
    tft.init(240, 320);
    tft.setRotation(1);
    tft.fillScreen(ST77XX_BLACK);
    TJpgDec.setCallback(tft_output);

    pinMode(BTN_LEFT,   INPUT_PULLUP);
    pinMode(BTN_MIDDLE, INPUT_PULLUP);
    pinMode(BTN_RIGHT,  INPUT_PULLUP);

    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setCursor(TRACK_MARGIN, TFT_HEIGHT/2 - 4);
    tft.print("Connecting to WiFi...");
    WiFi.begin(SSID, PASSWORD);
    while(WiFi.status() != WL_CONNECTED) delay(500);

    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(TRACK_MARGIN, TFT_HEIGHT/2 - 4);
    tft.print("Connecting to Spotify...");
    sp.begin();

    tft.fillScreen(ST77XX_BLACK);
}

void loop(){
    unsigned long now = millis();

    // Auto-return to player after inactivity
    if(currentScreen == SCREEN_PLAYLIST && now - lastInteraction >= INACTIVITY_MS)
        returnToPlayer();

    if(currentScreen == SCREEN_PLAYER){
        handlePlayerButtons();
        // Poll Spotify every 3 seconds
        if(now - lastPollTime >= POLL_INTERVAL_MS){
            lastPollTime = now;
            pollSpotify();
        }
        // Scroll title
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
    } else {
        handlePlaylistButtons();
    }
}
