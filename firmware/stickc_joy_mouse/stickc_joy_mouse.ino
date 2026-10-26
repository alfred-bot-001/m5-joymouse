// StickC JoyMouse —— M5StickC + Mini JoyC HAT 做成的蓝牙鼠标
//
// 硬件：M5StickC（ESP32-PICO-D4，4MB）顶部 8 针插座插 Mini JoyC HAT（STM32F030，I2C 0x54，SDA=G0，SCL=G26）。
// 功能：摇杆控制光标，摇杆按键 = 左键；StickC 正面 A 键 = 右键；按住侧面 B 键时摇杆上下 = 滚轮。
// 蓝牙：标准 BLE HID 鼠标（Just Works 配对并绑定），设备名见 kDeviceName。
//
// 依赖：esp32 核心 3.3.x 自带的 BLE 库（Bluedroid）、M5Unified 0.2.x。
// 构建：见 ../build.sh（FQBN esp32:esp32:m5stack_stickc，分区 huge_app）。

#include <M5Unified.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <HIDTypes.h>
#include <Wire.h>

// ---------------- 可调参数 ----------------
static const char kDeviceName[] = "StickC JoyMouse";
static const char kFirmwareVersion[] = "0.1.5";

static constexpr uint8_t kJoyAddr = 0x54;   // Mini JoyC HAT I2C 地址
static constexpr int kHatSda = 0;           // StickC HAT 插座 SDA = G0
static constexpr int kHatScl = 26;          // StickC HAT 插座 SCL = G26
static constexpr uint32_t kI2cFreq = 100000;

static constexpr uint32_t kReportIntervalMs = 10;  // 100 Hz 上报
static constexpr float kDeadZone = 0.12f;          // 归一化死区（0~1）
static constexpr float kCurveExp = 1.8f;           // 加速曲线指数，>1 小偏移更慢
static constexpr float kMaxSpeedPx = 14.0f;        // 满偏时每次上报的位移（像素）
static constexpr float kScrollPerReport = 0.08f;   // 滚轮模式满偏时每次上报的格数
static constexpr bool kInvertX = false;            // 光标左右方向反了就改这里
static constexpr bool kInvertY = false;            // 光标上下方向反了就改这里

// ---------------- Mini JoyC 寄存器（与官方 m5stack/M5Hat-Mini-JoyC 库一致） ----------------
enum : uint8_t {
  kRegAdc12 = 0x00,      // X/Y 12 位 ADC，各 2 字节小端
  kRegPos10 = 0x10,      // X/Y 10 位位置，各 2 字节
  kRegPos8 = 0x20,       // X/Y 8 位位置，各 1 字节，有符号（中心 0）
  kRegButton = 0x30,     // 按键，0 = 按下
  kRegRgb = 0x40,        // R,G,B
  kRegCal = 0x50,        // 校准值
  kRegFwVersion = 0xFE,
};

// ---------------- HID 报告描述符（3 键 + X + Y + 滚轮） ----------------
static const uint8_t kReportMap[] = {
  USAGE_PAGE(1), 0x01,        // Generic Desktop
  USAGE(1), 0x02,             // Mouse
  COLLECTION(1), 0x01,        // Application
  REPORT_ID(1), 0x01,
  USAGE(1), 0x01,             // Pointer
  COLLECTION(1), 0x00,        // Physical
  USAGE_PAGE(1), 0x09,        // Buttons
  USAGE_MINIMUM(1), 0x01,
  USAGE_MAXIMUM(1), 0x03,
  LOGICAL_MINIMUM(1), 0x00,
  LOGICAL_MAXIMUM(1), 0x01,
  REPORT_COUNT(1), 0x03,
  REPORT_SIZE(1), 0x01,
  HIDINPUT(1), 0x02,          // Data, Var, Abs
  REPORT_COUNT(1), 0x01,
  REPORT_SIZE(1), 0x05,
  HIDINPUT(1), 0x03,          // Const（填充）
  USAGE_PAGE(1), 0x01,        // Generic Desktop
  USAGE(1), 0x30,             // X
  USAGE(1), 0x31,             // Y
  USAGE(1), 0x38,             // Wheel
  LOGICAL_MINIMUM(1), 0x81,   // -127
  LOGICAL_MAXIMUM(1), 0x7F,   // 127
  REPORT_SIZE(1), 0x08,
  REPORT_COUNT(1), 0x03,
  HIDINPUT(1), 0x06,          // Data, Var, Rel
  END_COLLECTION(0),
  END_COLLECTION(0),
};

struct __attribute__((packed)) MouseReport {
  uint8_t buttons;
  int8_t x;
  int8_t y;
  int8_t wheel;
};

// ---------------- 运行状态 ----------------
static M5Canvas canvas(&M5.Display);

static BLEHIDDevice* hid = nullptr;
static BLECharacteristic* inputMouse = nullptr;
static volatile bool bleConnected = false;
static volatile bool bleEncrypted = false;

static bool joyPresent = false;
static uint8_t joyFw = 0;
static float centerX = 0.f, centerY = 0.f;   // 8 位位置为有符号值，官方已把中心标定到 0，这里再补上电偏差
static float accX = 0.f, accY = 0.f, accWheel = 0.f;
static uint8_t lastButtons = 0;
static uint8_t lastRgb[3] = {0xFF, 0xFF, 0xFF};
static uint32_t lastReportMs = 0, lastUiMs = 0, lastBatteryMs = 0, lastJoyRetryMs = 0;
static int lastDx = 0, lastDy = 0;   // 屏幕显示用
static char i2cScanText[20] = "--";   // 摇杆不在线时，屏幕显示总线上扫到的地址
static char diagText[24] = "";        // 排查用：引脚电平 / 5V 输出
static char diagText2[24] = "";       // 排查用：两根线的 ADC 毫伏 / 对调引脚探测
static uint16_t retryCount = 0;       // 摇杆未找到时的重试次数，用于定期给 HAT 断电重启
static uint8_t rawDbg[3] = {0, 0, 0}; // 最近一次原始 X/Y/按键
static float lastNx = 0.f, lastNy = 0.f;

// ---------------- BLE 回调 ----------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    bleConnected = true;
    Serial.println("[ble] connected");
  }
  void onDisconnect(BLEServer*) override {
    bleConnected = false;
    bleEncrypted = false;
    lastButtons = 0;
    Serial.println("[ble] disconnected, advertising again");
    BLEDevice::startAdvertising();
  }
};

class SecurityCallbacks : public BLESecurityCallbacks {
  bool onSecurityRequest() override { return true; }
#if defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    bleEncrypted = cmpl.success;
    Serial.printf("[ble] pairing %s (reason %d)\n", cmpl.success ? "ok" : "failed", cmpl.fail_reason);
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    bleEncrypted = desc->sec_state.encrypted;
    Serial.printf("[ble] pairing %s\n", bleEncrypted ? "ok" : "failed");
  }
#endif
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }
};

// ---------------- 摇杆（Arduino Wire，G0/G26，与官方例程一致） ----------------
static bool i2cRead(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(kJoyAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)kJoyAddr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; ++i) buf[i] = Wire.read();
  return true;
}

static bool i2cWrite(uint8_t reg, const uint8_t* buf, size_t len) {
  Wire.beginTransmission(kJoyAddr);
  Wire.write(reg);
  Wire.write(buf, len);
  return Wire.endTransmission() == 0;
}

static bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool joyReadRaw(int8_t& x, int8_t& y, uint8_t& btn) {
  uint8_t pos[2];
  if (!i2cRead(kRegPos8, pos, 2)) return false;
  uint8_t b;
  if (!i2cRead(kRegButton, &b, 1)) return false;
  x = (int8_t)pos[0];
  y = (int8_t)pos[1];
  btn = b;
  return true;
}

static void joySetRgb(uint8_t r, uint8_t g, uint8_t b) {
  if (!joyPresent) return;
  if (r == lastRgb[0] && g == lastRgb[1] && b == lastRgb[2]) return;
  uint8_t rgb[3] = {r, g, b};
  if (i2cWrite(kRegRgb, rgb, 3)) {
    lastRgb[0] = r; lastRgb[1] = g; lastRgb[2] = b;
  }
}

// I2C 总线恢复：若从机（HAT 上的 STM32）在上次复位时正读到一半，会一直拉低 SDA；
// 而 SDA 就是 G0，G0 复位时为低会让 ESP32 进下载模式。这里按标准做法在 SCL 上补 9 个时钟再发 STOP。
static void i2cBusRecover() {
  pinMode(kHatSda, INPUT_PULLUP);
  pinMode(kHatScl, OUTPUT_OPEN_DRAIN);
  digitalWrite(kHatScl, HIGH);
  delayMicroseconds(10);
  for (int i = 0; i < 9 && digitalRead(kHatSda) == LOW; ++i) {
    digitalWrite(kHatScl, LOW);
    delayMicroseconds(10);
    digitalWrite(kHatScl, HIGH);
    delayMicroseconds(10);
  }
  pinMode(kHatSda, OUTPUT_OPEN_DRAIN);
  digitalWrite(kHatSda, LOW);
  delayMicroseconds(10);
  digitalWrite(kHatSda, HIGH);
  delayMicroseconds(10);
  pinMode(kHatSda, INPUT_PULLUP);
  pinMode(kHatScl, INPUT_PULLUP);
}

static void i2cStart() {
  Wire.end();
  i2cBusRecover();
  Wire.begin(kHatSda, kHatScl, kI2cFreq);
  Wire.setTimeOut(20);
}

static bool joyInit() {
  if (!i2cProbe(kJoyAddr)) {
    i2cStart();   // 可能是总线被拉死，恢复后再试一次
    if (!i2cProbe(kJoyAddr)) {
      // 把总线上所有应答地址列出来，画到屏幕上便于排查
      size_t n = 0;
      i2cScanText[0] = 0;
      for (uint8_t a = 0x08; a <= 0x77 && n + 3 < sizeof(i2cScanText); ++a) {
        if (i2cProbe(a)) n += snprintf(i2cScanText + n, sizeof(i2cScanText) - n, "%02X ", a);
      }
      if (n == 0) strcpy(i2cScanText, "none");
      // 引脚空闲电平 + 5V 输出状态
      Wire.end();
      pinMode(kHatSda, INPUT_PULLUP);
      pinMode(kHatScl, INPUT_PULLUP);
      delay(2);
      snprintf(diagText, sizeof(diagText), "D%d C%d 5V%d", digitalRead(kHatSda), digitalRead(kHatScl),
               M5.Power.getExtOutput() ? 1 : 0);
      // 两根线的模拟电压（去掉内部上拉后测），用于区分“被主动拉低”与“被无源拉到半高”
      pinMode(kHatSda, INPUT);
      pinMode(kHatScl, INPUT);
      delay(2);
      uint32_t mvSda = analogReadMilliVolts(kHatSda);
      uint32_t mvScl = analogReadMilliVolts(kHatScl);
      // 对照：把 SDA/SCL 对调再探一次
      pinMode(kHatSda, INPUT_PULLUP);
      pinMode(kHatScl, INPUT_PULLUP);
      Wire.begin(kHatScl, kHatSda, kI2cFreq);
      Wire.setTimeOut(20);
      bool swapped = i2cProbe(kJoyAddr);
      Wire.end();
      snprintf(diagText2, sizeof(diagText2), "%4lumV %4lumV %s", (unsigned long)mvSda, (unsigned long)mvScl,
               swapped ? "SWAP!" : "p" );
      // 每 5 次失败给 HAT 插座的 5V 断电 0.3 秒再通，尝试让 HAT 上的 STM32 重新上电
      ++retryCount;
      if (retryCount % 5 == 0) {
        M5.Power.setExtOutput(false);
        delay(300);
        M5.Power.setExtOutput(true);
        delay(100);
      }
      snprintf(diagText2 + strlen(diagText2), sizeof(diagText2) - strlen(diagText2), "%u", (unsigned)retryCount);
      Serial.printf("[joy] i2c scan: %s diag: %s / %s\n", i2cScanText, diagText, diagText2);
      Wire.begin(kHatSda, kHatScl, kI2cFreq);
      Wire.setTimeOut(20);
      return false;
    }
    Serial.println("[joy] bus recovered");
  }
  // 上电时摇杆应处于回中、未按状态：取 24 次平均作为中心
  int32_t sx = 0, sy = 0;
  int n = 0;
  for (int i = 0; i < 24; ++i) {
    int8_t x, y; uint8_t b;
    if (joyReadRaw(x, y, b)) { sx += x; sy += y; ++n; }
    delay(5);
  }
  if (n < 12) return false;
  centerX = (float)sx / n;
  centerY = (float)sy / n;
  i2cRead(kRegFwVersion, &joyFw, 1);
  lastRgb[0] = lastRgb[1] = lastRgb[2] = 0xFF;
  Serial.printf("[joy] found at 0x%02X fw=%u center=(%.1f,%.1f)\n", kJoyAddr, joyFw, centerX, centerY);
  return true;
}

// 把 -128..127 的有符号位置按上电中心归一化到 -1..1，并套死区和加速曲线
static float normalizeAxis(int8_t raw, float center) {
  float span = ((float)raw >= center) ? (127.f - center) : (center + 128.f);
  if (span < 1.f) span = 1.f;
  float v = ((float)raw - center) / span;
  if (v > 1.f) v = 1.f;
  if (v < -1.f) v = -1.f;
  float mag = fabsf(v);
  if (mag <= kDeadZone) return 0.f;
  mag = (mag - kDeadZone) / (1.f - kDeadZone);
  mag = powf(mag, kCurveExp);
  return (v < 0) ? -mag : mag;
}

static int8_t clamp8(int v) {
  if (v > 127) return 127;
  if (v < -127) return -127;
  return (int8_t)v;
}

// ---------------- 屏幕 ----------------
static void drawUi() {
  canvas.fillScreen(TFT_BLACK);
  canvas.setTextSize(1);
  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(2, 2);
  canvas.println("JoyMouse");
  canvas.printf(" v%s\n", kFirmwareVersion);
  canvas.println();

  canvas.setTextColor(bleConnected ? (bleEncrypted ? TFT_GREEN : TFT_YELLOW) : TFT_CYAN);
  canvas.setCursor(2, canvas.getCursorY());
  canvas.println(bleConnected ? (bleEncrypted ? "BLE: linked" : "BLE: pairing") : "BLE: advert.");

  canvas.setTextColor(joyPresent ? TFT_GREEN : TFT_RED);
  canvas.setCursor(2, canvas.getCursorY());
  canvas.println(joyPresent ? "JOY: ok" : "JOY: none");
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.setCursor(2, canvas.getCursorY());
  if (joyPresent) canvas.printf("%4d %4d %u\n", (int8_t)rawDbg[0], (int8_t)rawDbg[1], rawDbg[2]);
  else canvas.printf("i2c %s\n", i2cScanText);
  if (!joyPresent) {
    canvas.setCursor(2, canvas.getCursorY());
    canvas.printf("%s\n", diagText);
    canvas.setCursor(2, canvas.getCursorY());
    canvas.printf("%s\n", diagText2);
  }

  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(2, canvas.getCursorY());
  canvas.printf("X %+5.2f\n", lastNx);
  canvas.setCursor(2, canvas.getCursorY());
  canvas.printf("Y %+5.2f\n", lastNy);
  canvas.setCursor(2, canvas.getCursorY());
  canvas.printf("dx %+3d\n", lastDx);
  canvas.setCursor(2, canvas.getCursorY());
  canvas.printf("dy %+3d\n", lastDy);
  canvas.println();

  canvas.setCursor(2, canvas.getCursorY());
  canvas.printf("L%c R%c %s\n", (lastButtons & 1) ? '*' : '-', (lastButtons & 2) ? '*' : '-',
                M5.BtnB.isPressed() ? "SCR" : "   ");

  // 摇杆位置小图
  const int cx = canvas.width() / 2, cy = canvas.height() - 30, r = 22;
  canvas.drawCircle(cx, cy, r, TFT_DARKGREY);
  canvas.fillCircle(cx + (int)(lastNx * r), cy - (int)(lastNy * r), 3, TFT_ORANGE);

  // 底部一行：电池
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.setCursor(2, canvas.height() - 9);
  canvas.printf("bat %3d%%", M5.Power.getBatteryLevel());
  canvas.pushSprite(0, 0);
}

// ---------------- setup / loop ----------------
void setup() {
  auto cfg = M5.config();
  cfg.output_power = true;      // 给 HAT 插座的 5V 供电
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.external_imu = false;
  cfg.external_rtc = false;
  M5.begin(cfg);
  M5.Power.setExtOutput(true);   // 再显式打开一次 HAT 插座 5V
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n[boot] %s v%s board=%d\n", kDeviceName, kFirmwareVersion, (int)M5.getBoard());

  M5.Display.setRotation(0);    // 竖屏，HAT 一端在上
  M5.Display.setBrightness(64);
  canvas.setColorDepth(8);
  canvas.createSprite(M5.Display.width(), M5.Display.height());

  // HAT 总线：Arduino Wire（I2C 0 号口）走 G0/G26；M5Unified 内部总线在 1 号口
  i2cStart();
  joyPresent = joyInit();
  if (!joyPresent) Serial.println("[joy] not found on G0/G26, will retry");

  // BLE HID
  BLEDevice::init(kDeviceName);
  BLESecurity* security = new BLESecurity();
  security->setCapability(ESP_IO_CAP_NONE);
  security->setAuthenticationMode(true, false, true);   // 绑定 + Just Works + LE Secure Connections
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  hid = new BLEHIDDevice(server);
  hid->manufacturer()->setValue("AI Home");
  hid->pnp(0x02, 0x303A, 0x0A01, 0x0100);   // USB 来源，Espressif VID
  hid->hidInfo(0x00, 0x02);                  // 国家码 0，可远程唤醒
  hid->reportMap((uint8_t*)kReportMap, sizeof(kReportMap));
  inputMouse = hid->inputReport(1);
  hid->setBatteryLevel(M5.Power.getBatteryLevel());
  hid->startServices();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->setAppearance(HID_MOUSE);
  adv->addServiceUUID(hid->hidService()->getUUID());
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[ble] advertising as HID mouse");

  drawUi();
}

void loop() {
  M5.update();
  const uint32_t now = millis();

  // 摇杆不在线时每秒重试一次
  if (!joyPresent && now - lastJoyRetryMs >= 1000) {
    lastJoyRetryMs = now;
    joyPresent = joyInit();
  }

  if (now - lastReportMs >= kReportIntervalMs) {
    lastReportMs = now;

    float nx = 0.f, ny = 0.f;
    bool joyBtn = false;
    if (joyPresent) {
      int8_t rx, ry; uint8_t rb;
      if (joyReadRaw(rx, ry, rb)) {
        rawDbg[0] = (uint8_t)rx; rawDbg[1] = (uint8_t)ry; rawDbg[2] = rb;
        nx = normalizeAxis(rx, centerX);
        ny = normalizeAxis(ry, centerY);
        joyBtn = (rb == 0);   // 官方库：按键寄存器 0 = 按下
      } else {
        joyPresent = false;
        Serial.println("[joy] read failed, marked offline");
      }
    }
    if (kInvertX) nx = -nx;
    if (kInvertY) ny = -ny;
    lastNx = nx;
    lastNy = ny;

    const bool scrollMode = M5.BtnB.isPressed();
    int dx = 0, dy = 0, dw = 0;
    if (scrollMode) {
      accWheel += ny * kScrollPerReport;
      dw = (int)accWheel;
      accWheel -= dw;
      accX = accY = 0.f;
    } else {
      accX += nx * kMaxSpeedPx;
      accY += -ny * kMaxSpeedPx;   // HID 的 Y 向下为正，摇杆向上为正
      dx = (int)accX;
      dy = (int)accY;
      accX -= dx;
      accY -= dy;
      accWheel = 0.f;
    }
    lastDx = dx;
    lastDy = dy;

    uint8_t buttons = 0;
    if (joyBtn) buttons |= 0x01;             // 摇杆按键 = 左键
    if (M5.BtnA.isPressed()) buttons |= 0x02;  // 正面 A 键 = 右键

    if (bleConnected && inputMouse) {
      if (dx || dy || dw || buttons != lastButtons) {
        MouseReport rep{buttons, clamp8(dx), clamp8(dy), clamp8(dw)};
        inputMouse->setValue((uint8_t*)&rep, sizeof(rep));
        inputMouse->notify();
      }
    }
    lastButtons = buttons;

    // HAT 上的 RGB：广播中蓝色慢闪，已连接绿色，按下左键白色
    if (!bleConnected) {
      joySetRgb(0, 0, ((now / 500) & 1) ? 40 : 0);
    } else if (joyBtn) {
      joySetRgb(60, 60, 60);
    } else {
      joySetRgb(0, 30, 0);
    }
  }

  if (now - lastBatteryMs >= 30000) {
    lastBatteryMs = now;
    if (hid) hid->setBatteryLevel(M5.Power.getBatteryLevel());
  }

  if (now - lastUiMs >= 100) {
    lastUiMs = now;
    drawUi();
  }
}
