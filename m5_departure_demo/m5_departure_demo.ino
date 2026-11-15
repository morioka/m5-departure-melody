#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <vector>

enum class Sound { Name, On, Off };

// CoreS3-SE: PORT.A (Grove)
constexpr int PIN_SWITCH = 1;  // GPIO1-GND: ON時に閉、OFF時に開
// CoreS3 internal microSD (official pin map)
constexpr int SD_CS = 4, SD_SCK = 36, SD_MISO = 35, SD_MOSI = 37;
constexpr size_t MAX_WAV_BYTES = 3 * 1024 * 1024;
constexpr unsigned long DEBOUNCE_MS = 30;

struct Station {
  String name, nameWav, onWav, offWav;
};
std::vector<Station> stations;
int stationCount = 0;
int currentStation = 0;
int missingFileCount = 0;
String firstMissingPath;
String statusText = "待機中";

// Tap the volume button to cycle 160 -> 220 -> 255 -> 160.
constexpr uint8_t VOLUMES[] = {160, 220, 255};
uint8_t volumeIndex = 0;
void cycleVolume();

// 一つの保持接点のみを監視する（30msデバウンス）。
struct SwitchInput {
  bool raw = HIGH;
  bool stable = HIGH;
  unsigned long changedAt = 0;
};
SwitchInput mainSwitch;

uint8_t* wavBuffer = nullptr;
bool audioStarted = false;
unsigned long startedAt = 0;

void showUI();

// 戻り値: +1=ONへ変化、-1=OFFへ変化、0=変化なし。
int switchTransition() {
  const bool value = digitalRead(PIN_SWITCH);
  const unsigned long now = millis();
  if (value != mainSwitch.raw) {
    mainSwitch.raw = value;
    mainSwitch.changedAt = now;
  }
  if (value != mainSwitch.stable &&
      now - mainSwitch.changedAt >= DEBOUNCE_MS) {
    mainSwitch.stable = value;
    return value == LOW ? +1 : -1;
  }
  return 0;
}

// CSV is intentionally simple: UTF-8, four comma-separated fields, no quoting.
// Detailed errors are reported on Serial and the display.
String configError;
int configErrorLine = 0;

bool invalidCsv(int row, const String& reason) {
  configErrorLine = row;
  configError = reason;
  Serial.printf("stations.csv line %d: %s\n", row, reason.c_str());
  return false;
}

bool readStations() {
  File f = SD.open("/stations.csv", FILE_READ);
  if (!f) {
    configError = "stations.csv がありません";
    return false;
  }
  int lineNo = 1;
  String header = f.readStringUntil('\n');
  header.trim();
  if (header.startsWith("\xEF\xBB\xBF")) header.remove(0, 3); // UTF-8 BOM
  if (header != "name,name_wav,on_wav,off_wav") {
    f.close();
    return invalidCsv(lineNo, "ヘッダーが不正です");
  }
  while (f.available()) {
    ++lineNo;
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;
    int a = line.indexOf(',');
    int b = a < 0 ? -1 : line.indexOf(',', a + 1);
    int c = b < 0 ? -1 : line.indexOf(',', b + 1);
    if (a < 0 || b < 0 || c < 0 || line.indexOf(',', c + 1) >= 0) {
      f.close();
      return invalidCsv(lineNo, "4列になっていません");
    }
    Station s;
    s.name = line.substring(0, a);
    s.nameWav = line.substring(a + 1, b);
    s.onWav = line.substring(b + 1, c);
    s.offWav = line.substring(c + 1);
    s.name.trim(); s.nameWav.trim(); s.onWav.trim(); s.offWav.trim();
    if (s.name.isEmpty() || s.nameWav.length() == 0 ||
        s.onWav.length() == 0 || s.offWav.length() == 0) {
      f.close();
      return invalidCsv(lineNo, "空欄があります");
    }
    if (!s.nameWav.startsWith("/") || !s.onWav.startsWith("/") ||
        !s.offWav.startsWith("/")) {
      f.close();
      return invalidCsv(lineNo, "音声パスは / で始めてください");
    }
    stations.push_back(s);
    ++stationCount;
  }
  f.close();
  if (stationCount == 0) return invalidCsv(lineNo, "駅データがありません");
  Serial.printf("Loaded stations: %d\n", stationCount);
  return true;
}

// Files are checked only for existence at boot, not fully decoded.
void checkSoundFiles() {
  missingFileCount = 0;
  firstMissingPath = "";
  for (int i = 0; i < stationCount; ++i) {
    const String paths[] = {stations[i].nameWav, stations[i].onWav,
                            stations[i].offWav};
    for (const auto& path : paths) {
      if (!SD.exists(path.c_str())) {
        ++missingFileCount;
        if (firstMissingPath.isEmpty()) firstMissingPath = path;
        Serial.printf("Missing file: %s\n", path.c_str());
      }
    }
  }
  Serial.printf("File check: %d/%d present\n",
                stationCount * 3 - missingFileCount, stationCount * 3);
}

void fail(const char* reason) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setFont(&fonts::efontJA_16);
  M5.Display.setTextColor(TFT_RED);
  M5.Display.setCursor(12, 60);
  M5.Display.println(reason);
  if (configError.length()) {
    M5.Display.printf("line: %d\n", configErrorLine);
    M5.Display.println(configError);
  }
  M5.Display.println("SDを修正して再起動");
  Serial.println(reason);
  while (true) delay(1000);
}

void showUI() {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setFont(&fonts::efontJA_16);
  d.setTextSize(1);
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_CYAN);
  d.drawString("発車メロディー", 75, 15);
  char num[32];
  // 2桁までは02桁、それ以上では自然に桁数を増やす。
  if (stationCount < 100) {
    snprintf(num, sizeof(num), "%02d/%02d", currentStation + 1, stationCount);
  } else {
    snprintf(num, sizeof(num), "%d/%d", currentStation + 1, stationCount);
  }
  d.drawString(num, 187, 15);
  // Volume button: top-right, kept clear of station selection buttons.
  d.fillRoundRect(236, 1, 83, 28, 5, 0x4208);
  d.setTextColor(TFT_WHITE);
  char volumeLabel[20];
  snprintf(volumeLabel, sizeof(volumeLabel), "VOL %u", VOLUMES[volumeIndex]);
  d.drawString(volumeLabel, 277, 15);
  if (missingFileCount > 0) {
    d.setTextColor(TFT_YELLOW);
    char warning[24];
    snprintf(warning, sizeof(warning), "欠落:%d", missingFileCount);
    d.drawString(warning, 260, 124);
  }

  d.fillRoundRect(8, 30, 304, 101, 7, 0x11C7);
  d.setTextColor(TFT_WHITE);
  d.setTextSize(2);
  d.drawString(stations[currentStation].name, 160, 76);
  d.setTextSize(1);
  d.drawString(statusText, 160, 116);

  const char* top[] = {"前", "駅名", "次"};
  for (int i = 0; i < 3; ++i) {
    int x = 8 + i * 104;
    d.fillRoundRect(x, 142, 96, 40, 6, 0x4208);
    d.setTextColor(TFT_WHITE);
    d.drawString(top[i], x + 48, 162);
  }
  d.fillRoundRect(8, 192, 148, 42, 6, 0x0400);
  d.fillRoundRect(164, 192, 148, 42, 6, 0xA800);
  d.setTextColor(TFT_WHITE);
  d.drawString("ON テスト", 82, 213);
  d.drawString("OFF テスト", 238, 213);
}


// End playback before *any* attempt to open the next file.
// This ensures a missing/invalid WAV never leaves the preceding audio running.
void stopAudio() {
  M5.Speaker.end();
  audioStarted = false;
}

void playSound(Sound sound) {
  const Station& s = stations[currentStation];
  const String& path = sound == Sound::Name ? s.nameWav :
                       sound == Sound::On ? s.onWav : s.offWav;
  stopAudio();
  statusText = "読み込み中";
  showUI();

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    statusText = "音声ファイルなし";
    Serial.printf("Missing: %s\n", path.c_str());
    showUI();
    return;
  }
  size_t size = file.size();
  if (size < 44 || size > MAX_WAV_BYTES) {
    file.close();
    statusText = "WAVサイズエラー";
    Serial.printf("Bad size (%u): %s\n", (unsigned)size, path.c_str());
    showUI();
    return;
  }
  // Minimal RIFF/WAVE header check; format decoding is handled by M5Unified.
  uint8_t header[12];
  size_t headerBytes = file.read(header, sizeof(header));
  bool valid = headerBytes == sizeof(header) &&
               memcmp(header, "RIFF", 4) == 0 &&
               memcmp(header + 8, "WAVE", 4) == 0;
  if (!valid) {
    file.close();
    statusText = "WAV形式エラー";
    Serial.printf("Bad WAV header: %s\n", path.c_str());
    showUI();
    return;
  }
  file.seek(0);
  size_t loaded = file.read(wavBuffer, size);
  file.close();
  if (loaded != size) {
    statusText = "SD読み込みエラー";
    Serial.printf("Read failed: %s\n", path.c_str());
    showUI();
    return;
  }
  if (!M5.Speaker.begin()) {
    statusText = "スピーカー初期化失敗";
    showUI();
    return;
  }
  M5.Speaker.setVolume(VOLUMES[volumeIndex]);
  if (!M5.Speaker.playWav(wavBuffer, size, 1, 0, true)) {
    stopAudio();
    statusText = "WAV再生エラー";
    Serial.printf("Play failed: %s\n", path.c_str());
    showUI();
    return;
  }
  audioStarted = true;
  startedAt = millis();
  statusText = sound == Sound::Name ? "駅名を再生中" :
               sound == Sound::On ? "ONを再生中" : "OFFを再生中";
  showUI();
}

void cycleVolume() {
  volumeIndex = (volumeIndex + 1) % 3;
  M5.Speaker.setVolume(VOLUMES[volumeIndex]);
  Serial.printf("Volume: %u\n", VOLUMES[volumeIndex]);
  // Do not stop or restart audio; apply volume to the current playback.
  showUI();
}

void selectStation(int step) {
  currentStation = (currentStation + step + stationCount) % stationCount;
  playSound(Sound::Name);
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  M5.begin(cfg);
  M5.Display.setRotation(0);
  Serial.begin(115200);
  M5.Speaker.setVolume(VOLUMES[volumeIndex]);

  pinMode(PIN_SWITCH, INPUT_PULLUP);
  mainSwitch.raw = mainSwitch.stable = digitalRead(PIN_SWITCH);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 25000000)) fail("SDカードを認識できません");
  if (!readStations()) fail("stations.csvを確認してください");
  checkSoundFiles();
  wavBuffer = (uint8_t*)heap_caps_malloc(MAX_WAV_BYTES,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wavBuffer) fail("PSRAMを確保できません");
  statusText = missingFileCount ? "一部音声が欠落" : "待機中";
  showUI();
}

void loop() {
  M5.update();
  const int transition = switchTransition();
  if (transition == +1) playSound(Sound::On);
  else if (transition == -1) playSound(Sound::Off);

  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    int x = t.x, y = t.y;
    if (x >= 236 && x < 320 && y >= 0 && y < 30) {
      cycleVolume();
    } else if (y >= 142 && y < 182) {
      if (x >= 8 && x < 104) selectStation(-1);
      else if (x >= 112 && x < 208) playSound(Sound::Name);
      else if (x >= 216 && x < 312) selectStation(1);
    } else if (y >= 192 && y < 234) {
      if (x >= 8 && x < 156) playSound(Sound::On);
      else if (x >= 164 && x < 312) playSound(Sound::Off);
    }
  }

  if (audioStarted && millis() - startedAt > 250 && !M5.Speaker.isPlaying()) {
    audioStarted = false;
    statusText = "待機中";
    showUI();
  }
  delay(5);
}
