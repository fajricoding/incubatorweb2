#include <WiFi.h>
#include <WiFiManager.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include "DHTesp.h"
#include <LiquidCrystal_I2C.h>

// ── KONFIGURASI ──
#define FIREBASE_HOST    "https://incubatorcloud-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_API_KEY "AIzaSyAHuGNYJFGmHlGd_8aTz-r0hgyFX7lm7VI"

// ── PIN SENSOR & AKTUATOR ──
#define DHT_PIN      13   // Data DHT22 (pakai pull-up 10k ke 3V3 kalau bukan modul)
#define PIN_HEATER   12   // Relay aktif HIGH
#define PIN_MOTOR    14   // PWM ke L298N
#define PWM_FREQ     1000
#define PWM_RES      8
#define PIN_BUZZER   25   // Buzzer aktif HIGH

// ── PIN TOMBOL FISIK (aktif LOW, INPUT_PULLUP) ──
#define BTN_MENU       4  // Pilih parameter yang mau diatur
#define BTN_UP         16 // Naikkan nilai parameter terpilih
#define BTN_DOWN       17 // Turunkan nilai parameter terpilih
#define BTN_STARTSTOP  5  // Start / Stop alat (bisa ditekan kapan saja)

// ── MAKRO RELAY & BUZZER AKTIF HIGH ──
#define HEATER_ON()   digitalWrite(PIN_HEATER, HIGH)
#define HEATER_OFF()  digitalWrite(PIN_HEATER, LOW)
#define BUZZER_ON()   digitalWrite(PIN_BUZZER, HIGH)
#define BUZZER_OFF()  digitalWrite(PIN_BUZZER, LOW)
#define HEATER_IS_ON() (digitalRead(PIN_HEATER) == HIGH)

// ── FIREBASE ──
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ── NTP (hanya dipakai untuk pencatatan histori saat online) ──
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000);

// ── SENSOR & LCD ──
DHTesp dhtSensor;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// DHT22 kadang gagal baca sesekali (dan minimal 2 detik antar pembacaan).
// Selama gagal < toleransi, pakai nilai valid terakhir supaya heater/buzzer
// tidak ikut "kedip" karena glitch sesaat.
float lastSuhuValid = NAN;
unsigned long lastSuhuValidTime = 0;
const unsigned long SENSOR_TOLERANSI_MS = 10000;

// ── STATUS KONEKSI ──
bool online = false; // true jika WiFi + Firebase berhasil konek saat boot

// ── STATE KONTROL (dipakai baik mode online maupun offline/lokal) ──
bool  aktif            = false;
bool  status_heater    = false;
bool  heaterSebelumnya = false;
float targetSuhu       = 37.0;
int   pwmMotor         = 150;
float durasiJam        = 2.0;
unsigned long motorStartMillis = 0; // ganti dari epoch NTP -> millis() lokal

// ── STATE MOTOR SIKLUS ──
bool motorFaseOn         = true;
unsigned long cycleTimer = 0;
const unsigned long MOTOR_ON_MS  = 120000; // 120 detik ON
const unsigned long MOTOR_OFF_MS = 180000; // 180 detik OFF

// ── TIMER ──
unsigned long lastSendTime    = 0;
unsigned long lastStatusTime  = 0;
unsigned long lastKontrolRead = 0;
const unsigned long SEND_INTERVAL    = 10000;
const unsigned long SEND_UPDATE_SUHU = 1500;
const unsigned long KONTROL_INTERVAL = 3000;

// ── WIFI ──
const unsigned long CONFIG_PORTAL_TIMEOUT_S = 50;
const char* AP_SETUP_SSID = "ESP32-INCUBATOR-Setup";
WiFiManager wm;

// ── MENU LOKAL (tombol fisik) ──
enum MenuMode { MENU_IDLE, MENU_SUHU, MENU_PWM, MENU_DURASI };
MenuMode menuMode = MENU_IDLE;
unsigned long menuLastActivity = 0;
const unsigned long MENU_TIMEOUT_MS = 6000; // otomatis kembali ke tampilan utama

struct Tombol {
  uint8_t pin;
  bool lastState = HIGH;
  unsigned long lastTekan = 0;
};
Tombol btnMenu  = {BTN_MENU};
Tombol btnUp    = {BTN_UP};
Tombol btnDown  = {BTN_DOWN};
Tombol btnStart = {BTN_STARTSTOP};

void kirimKontrolKeFirebase(); // forward declaration

// ======================================================
// FUNGSI BANTU LCD
// ======================================================
void tampillcd(String baris1, String baris2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(baris1);
  lcd.setCursor(0, 1);
  lcd.print(baris2);
}

// ── Buzzer bunyi non-blocking (1 detik), untuk event heater/start-stop ──
unsigned long buzzerStartTime = 0;
bool buzzerAktif = false;

void buzzerMulai() {
  BUZZER_ON();
  buzzerStartTime = millis();
  buzzerAktif     = true;
}

void buzzerUpdate() {
  if (buzzerAktif && millis() - buzzerStartTime >= 1000) {
    BUZZER_OFF();
    buzzerAktif = false;
  }
}

// ── Bunyi klik pendek untuk feedback navigasi menu ──
void buzzerKlik() {
  BUZZER_ON();
  delay(30);
  BUZZER_OFF();
}

// ======================================================
// BACA SENSOR DHT22
// ======================================================
float bacaSuhu() {
  float t = dhtSensor.getTemperature(); // DHTesp otomatis menahan pembacaan tiap 2 detik
  if (!isnan(t) && t > -40.0 && t < 80.0) {
    lastSuhuValid     = t;
    lastSuhuValidTime = millis();
    return t;
  }
  // Gagal baca: pakai nilai valid terakhir selama masih dalam toleransi
  if (!isnan(lastSuhuValid) && (millis() - lastSuhuValidTime) < SENSOR_TOLERANSI_MS) {
    return lastSuhuValid;
  }
  return NAN; // gagal terus > toleransi -> dianggap sensor error
}

// ======================================================
// TOMBOL FISIK — deteksi tekan dengan debounce sederhana
// ======================================================
bool tombolDitekan(Tombol &b) {
  bool state = digitalRead(b.pin);
  bool ditekan = false;
  if (state == LOW && b.lastState == HIGH && (millis() - b.lastTekan) > 200) {
    ditekan = true;
    b.lastTekan = millis();
  }
  b.lastState = state;
  return ditekan;
}

void ubahNilaiMenu(int arah) {
  switch (menuMode) {
    case MENU_SUHU:
      targetSuhu += arah * 0.5;
      targetSuhu = constrain(targetSuhu, 20.0, 60.0);
      break;
    case MENU_PWM:
      pwmMotor += arah * 5;
      pwmMotor = constrain(pwmMotor, 0, 255);
      if (aktif && motorFaseOn) ledcWrite(PIN_MOTOR, pwmMotor); // motor langsung ikut berubah kalau sedang jalan
      break;
    case MENU_DURASI:
      durasiJam += arah * 0.5;
      durasiJam = constrain(durasiJam, 0.5, 24.0);
      break;
    default:
      break;
  }
}

// ======================================================
// KONTROL VIA TOMBOL FISIK — SELALU AKTIF, ONLINE MAUPUN OFFLINE
// ======================================================
void handleTombolFisik() {
  // MENU: pindah parameter (Suhu -> PWM -> Durasi -> tampilan normal -> ulang)
  if (tombolDitekan(btnMenu)) {
    menuMode = (MenuMode)((menuMode + 1) % 4);
    menuLastActivity = millis();
    buzzerKlik();
  }

  // UP / DOWN hanya aktif saat sedang mengatur salah satu parameter
  if (menuMode != MENU_IDLE) {
    if (tombolDitekan(btnUp)) {
      ubahNilaiMenu(1);
      menuLastActivity = millis();
      buzzerKlik();
      kirimKontrolKeFirebase(); // langsung sinkron tiap perubahan, jangan tunggu timeout
    }
    if (tombolDitekan(btnDown)) {
      ubahNilaiMenu(-1);
      menuLastActivity = millis();
      buzzerKlik();
      kirimKontrolKeFirebase(); // langsung sinkron tiap perubahan, jangan tunggu timeout
    }
    // Timeout: kembali otomatis ke tampilan utama (sinkron sudah terjadi tiap perubahan di atas,
    // panggilan ini cuma jaga-jaga kalau ada perubahan yang belum sempat terkirim)
    if (millis() - menuLastActivity > MENU_TIMEOUT_MS) {
      menuMode = MENU_IDLE;
      kirimKontrolKeFirebase();
    }
  }

  // START/STOP: berlaku kapan saja, baik online maupun offline
  if (tombolDitekan(btnStart)) {
    aktif = !aktif;
    if (aktif) {
      motorStartMillis = millis();
      cycleTimer       = millis();
      motorFaseOn      = true;
      ledcWrite(PIN_MOTOR, pwmMotor);
    } else {
      ledcWrite(PIN_MOTOR, 0);
      HEATER_OFF();
      status_heater = false;
    }
    menuMode = MENU_IDLE;
    buzzerMulai();
    kirimKontrolKeFirebase();
  }
}

// ======================================================
// SETUP
// ======================================================
void setup() {
  Serial.begin(115200);

  // LCD
  Wire.begin(26, 27);
  lcd.init();
  lcd.backlight();

  // Tombol fisik
  pinMode(BTN_MENU, INPUT_PULLUP);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_STARTSTOP, INPUT_PULLUP);

  // Pin output
  pinMode(PIN_HEATER, OUTPUT);
  HEATER_OFF();
  pinMode(PIN_BUZZER, OUTPUT);
  BUZZER_OFF();

  // PWM motor
  ledcAttach(PIN_MOTOR, PWM_FREQ, PWM_RES);
  ledcWrite(PIN_MOTOR, 0);

  // Sensor DHT22
  dhtSensor.setup(DHT_PIN, DHTesp::DHT22);

  // WiFiManager — kalau gagal konek, LANJUT mode OFFLINE (tidak restart)
  tampillcd("ESP32-INCUBATOR", "Cek WiFi...");
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_S);
  online = wm.autoConnect(AP_SETUP_SSID);

  if (!online) {
    Serial.println("WiFi tidak tersedia. Lanjut MODE OFFLINE (kontrol lokal via tombol).");
    tampillcd("Mode OFFLINE", "Kontrol lokal");
    delay(1500);
  } else {
    tampillcd("WiFi", "Tersambung");
    Serial.println("WiFi tersambung. IP: " + WiFi.localIP().toString());

    config.api_key      = FIREBASE_API_KEY;
    config.database_url = FIREBASE_HOST;
    config.token_status_callback = tokenStatusCallback;
    Firebase.signUp(&config, &auth, "", "");
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    Serial.print("Menunggu Firebase");
    unsigned long t = millis();
    while (!Firebase.ready() && millis() - t < 15000) {
      Serial.print(".");
      delay(500);
    }
    Serial.println(Firebase.ready() ? "\nFirebase siap!" : "\nFirebase timeout");

    timeClient.begin();
    timeClient.update();
  }

  cycleTimer = millis();
}

// ======================================================
// BACA /kontrol dari Firebase (hanya saat online DAN tidak sedang diedit via LCD)
// ======================================================
void bacaKontrol() {
  if (!Firebase.ready()) return;

  bool aktifSebelumnya = aktif;

  if (Firebase.RTDB.getJSON(&fbdo, "/kontrol")) {
    FirebaseJson     &json = fbdo.jsonObject();
    FirebaseJsonData  result;

    json.get(result, "aktif");
    aktif = result.intValue == 1;

    json.get(result, "target");
    if (result.success) targetSuhu = result.floatValue;

    json.get(result, "pwmMotor");
    if (result.success) pwmMotor = result.intValue;

    json.get(result, "durasiJam");
    if (result.success) durasiJam = result.floatValue;

    Serial.printf("[Kontrol] aktif=%d target=%.1f pwm=%d durasi=%.1fh\n",
                  aktif, targetSuhu, pwmMotor, durasiJam);

    if (!aktifSebelumnya && aktif) {
      motorStartMillis = millis();
      cycleTimer       = millis();
      motorFaseOn      = true;
      ledcWrite(PIN_MOTOR, pwmMotor);
      Serial.println("Motor START, PWM=" + String(pwmMotor));
    }

    if (aktifSebelumnya && !aktif) {
      ledcWrite(PIN_MOTOR, 0);
      HEATER_OFF();
      status_heater = false;
      Serial.println("STOP dari web");
    }

  } else {
    Serial.println("Gagal baca /kontrol: " + fbdo.errorReason());
  }
}

// ======================================================
// KIRIM NILAI KONTROL LOKAL KE FIREBASE (supaya web ikut sinkron)
// ======================================================
void kirimKontrolKeFirebase() {
  if (!online || !Firebase.ready()) return;
  FirebaseJson j;
  j.set("aktif", aktif ? 1 : 0);
  j.set("target", targetSuhu);
  j.set("pwmMotor", pwmMotor);
  j.set("durasiJam", durasiJam);
  j.set("motorStartEpoch", (int)timeClient.getEpochTime());
  Firebase.RTDB.setJSON(&fbdo, "/kontrol", &j);
}

// ======================================================
// KIRIM DATA HISTORIS ke /suhu (hanya saat online)
// ======================================================
void kirimSensor(float suhuwrite) {
  timeClient.update();
  time_t    rawTime = timeClient.getEpochTime();
  struct tm *ti     = localtime(&rawTime);

  int menit   = ti->tm_min;
  int jam     = ti->tm_hour;
  int tanggal = ti->tm_mday;
  int bulan   = ti->tm_mon + 1;
  int tahun   = ti->tm_year + 1900;

  String timeKey = String(tahun) + "-"
    + (bulan   < 10 ? "0" : "") + String(bulan)   + "-"
    + (tanggal < 10 ? "0" : "") + String(tanggal) + "_"
    + (jam     < 10 ? "0" : "") + String(jam)     + ":"
    + (menit   < 10 ? "0" : "") + String(menit);

  FirebaseJson jsonSuhu;
  jsonSuhu.set("suhu",          suhuwrite);
  jsonSuhu.set("target_suhu",   targetSuhu);
  jsonSuhu.set("status_heater", status_heater);
  jsonSuhu.set("waktu/menit",   menit);
  jsonSuhu.set("waktu/jam",     jam);
  jsonSuhu.set("waktu/tanggal", tanggal);
  jsonSuhu.set("waktu/bulan",   bulan);
  jsonSuhu.set("waktu/tahun",   tahun);

  if (Firebase.RTDB.setJSON(&fbdo, ("/suhu/" + timeKey).c_str(), &jsonSuhu)) {
    Serial.println("Suhu terkirim: " + timeKey);
  } else {
    Serial.println("Gagal kirim suhu: " + fbdo.errorReason());
  }
}

// ======================================================
// KIRIM STATUS REALTIME ke /status (hanya saat online)
// ======================================================
void kirimstatus(float suhuupdate) {
  timeClient.update();
  time_t rawTime = timeClient.getEpochTime();

  bool error = isnan(suhuupdate);

  FirebaseJson jsonStatus;
  jsonStatus.set("suhu",        error ? 0.0 : suhuupdate);
  jsonStatus.set("heater",      HEATER_IS_ON() ? 1 : 0);
  jsonStatus.set("motor",       (ledcRead(PIN_MOTOR) > 0) ? 1 : 0);
  jsonStatus.set("timestamp",   (int)rawTime);
  jsonStatus.set("sensorError", error ? 1 : 0);

  if (Firebase.RTDB.setJSON(&fbdo, "/status", &jsonStatus)) {
    Serial.println(error ? "Status: SENSOR ERROR" : "Status terkirim");
  } else {
    Serial.println("Gagal kirim status: " + fbdo.errorReason());
  }
}

// ======================================================
// TAMPILAN LCD: tampilan utama (idle) atau layar menu edit
// ======================================================
void tampilkanLCD(float suhu) {
  switch (menuMode) {
    case MENU_SUHU:
      tampillcd("Set Target Suhu:", String(targetSuhu, 1) + " C");
      break;
    case MENU_PWM:
      tampillcd("Set RPM Motor:", String(pwmMotor) + " (0-255)");
      break;
    case MENU_DURASI:
      tampillcd("Set Durasi:", String(durasiJam, 1) + " jam");
      break;
    default:
      tampillcd(String(online ? "[ON] " : "[OFF]") + "SUHU:" + String(suhu, 1) + "C",
                "T:" + String(targetSuhu, 0) + "C " + (aktif ? "RUN" : "STOP"));
      break;
  }
}

// ======================================================
// LOGIKA HEATER & MOTOR
// ======================================================
void kontrolAlat(float suhuupdate) {
  if (!aktif) {
    HEATER_OFF();
    ledcWrite(PIN_MOTOR, 0);
    status_heater    = false;
    heaterSebelumnya = false;
    motorFaseOn      = true;
    cycleTimer       = millis();
    return;
  }

  bool heaterHarusNyala = (suhuupdate < targetSuhu);

  if (heaterHarusNyala) {
    HEATER_ON();
    status_heater = true;
    if (!heaterSebelumnya) {
      Serial.println("Heater ON — suhu di bawah target");
      buzzerMulai();
    }
  } else {
    HEATER_OFF();
    status_heater = false;
    if (heaterSebelumnya) {
      Serial.println("Heater OFF — suhu target tercapai");
      buzzerMulai();
    }
  }
  heaterSebelumnya = heaterHarusNyala;

  // Durasi total motor — berbasis millis(), tidak bergantung NTP/internet
  unsigned long elapsedMs = millis() - motorStartMillis;
  unsigned long durasiMs  = (unsigned long)(durasiJam * 3600000UL);

  if (elapsedMs >= durasiMs) {
    ledcWrite(PIN_MOTOR, 0);
    return;
  }

  // Siklus ON 120 detik / OFF 180 detik
  unsigned long elapsedCycle = millis() - cycleTimer;

  if (motorFaseOn) {
    ledcWrite(PIN_MOTOR, pwmMotor);
    if (elapsedCycle >= MOTOR_ON_MS) {
      motorFaseOn = false;
      cycleTimer  = millis();
      ledcWrite(PIN_MOTOR, 0);
      Serial.println("Motor OFF (siklus)");
    }
  } else {
    ledcWrite(PIN_MOTOR, 0);
    if (elapsedCycle >= MOTOR_OFF_MS) {
      motorFaseOn = true;
      cycleTimer  = millis();
      ledcWrite(PIN_MOTOR, pwmMotor);
      Serial.println("Motor ON (siklus) PWM=" + String(pwmMotor));
    }
  }
}

// ======================================================
// LOOP
// ======================================================
void loop() {
  unsigned long now = millis();

  buzzerUpdate();
  handleTombolFisik();

  // Skip polling Firebase selama user sedang mengedit lewat menu LCD,
  // supaya nilai yang sedang diatur via tombol tidak ketimpa data lama dari cloud
  if (online && menuMode == MENU_IDLE && now - lastKontrolRead >= KONTROL_INTERVAL) {
    lastKontrolRead = now;
    bacaKontrol();
  }

  float suhuupdate = bacaSuhu();

  if (online && now - lastStatusTime >= SEND_UPDATE_SUHU) {
    lastStatusTime = now;
    kirimstatus(suhuupdate);
  }

  if (isnan(suhuupdate)) {
    Serial.println("Sensor tidak terbaca");
    tampillcd("SENSOR ERROR", "Cek DHT22!");
    HEATER_OFF();
    ledcWrite(PIN_MOTOR, 0);
    if (!buzzerAktif) buzzerMulai();
    delay(200);
    return;
  }

  Serial.printf("Suhu: %.1f C | Target: %.1f C | Heater: %s | Aktif: %s | %s\n",
    suhuupdate, targetSuhu,
    HEATER_IS_ON() ? "ON" : "OFF",
    aktif ? "YES" : "NO",
    online ? "ONLINE" : "OFFLINE");

  tampilkanLCD(suhuupdate);
  kontrolAlat(suhuupdate);

  if (online && now - lastSendTime >= SEND_INTERVAL) {
    lastSendTime = now;
    kirimSensor(suhuupdate);
  }

  delay(50); // kecil, supaya tombol tetap responsif
}
