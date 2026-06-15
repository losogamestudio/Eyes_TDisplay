/*
  AnimatronicEyes_TDisplay.ino

  Use esp32 2.0.17 and included version of TFT_eSPI

  Full-color animatronic eyes for the TTGO T-Display (original)
  Display: ST7789  135 x 240  (landscape = 240 wide, 135 tall)
  Library: TFT_eSPI

  SETUP:
    In TFT_eSPI/User_Setup_Select.h:
      - comment out:  #include <User_Setup.h>
      - uncomment:    #include <User_Setups/Setup25_TTGO_T_Display.h>

  Arduino IDE board: ESP32 Dev Module
    - PSRAM: Disabled  (classic ESP32, no PSRAM)
    - Flash Size: 4MB (or 16MB depending on your variant)
    - Partition Scheme: Default 4MB with spiffs

  External control API:
    cmdLook(x, y)          -1..1 each axis
    cmdBlink()
    cmdSquint(0..1)
    cmdEmotion(EMOTION_X)
    cmdCenter()
    cmdIdle()

  Serial test: l r u d c b s i  0-7
*/

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>

// ════════════════════════════════════════════════════════════════════════════
//  CONFIG
// ════════════════════════════════════════════════════════════════════════════

// ── Display ──────────────────────────────────────────────────────────────────
// Rotation 1 = landscape (240 wide × 135 tall)
// Rotation 3 = landscape flipped (if mounted upside-down)
#define DISPLAY_ROTATION    1
#define SCREEN_W          240
#define SCREEN_H          135

// ── Eye socket geometry ───────────────────────────────────────────────────────
// 240×135 is tight — eyes need to be compact and well-separated
#define EYE_RX             42    // socket half-width
#define EYE_RY             38    // socket half-height
#define LEFT_EYE_CX        52    // left eye center X
#define RIGHT_EYE_CX      188    // right eye center X
#define EYE_CY             85    // eye center Y  (just below mid for brow room)

// ── Iris / pupil ──────────────────────────────────────────────────────────────
#define IRIS_R             20
#define PUPIL_R             9
#define SHINE_R             3
#define SHINE_OX            6
#define SHINE_OY           -6

// ── Gaze travel ───────────────────────────────────────────────────────────────
#define MAX_GAZE_X         20
#define MAX_GAZE_Y          15

// ── Eyebrow geometry ──────────────────────────────────────────────────────────
#define BROW_HALF_W        32
#define BROW_EYE_GAP        20   // px above top of socket
#define BROW_THICKNESS      4
#define BROW_RAISE_RANGE    20
#define BROW_TILT_RANGE     15

// ── Colors (16-bit 565) ───────────────────────────────────────────────────────
#define COL_BG          TFT_BLACK
#define COL_SCLERA      0xFFFF
#define COL_IRIS_OUTER  0x0318   // deep teal
#define COL_IRIS_MID    0x051D
#define COL_IRIS_INNER  0x0210
#define COL_PUPIL       0x0000
#define COL_SHINE       0xFFFF
#define COL_EYELID      TFT_BLACK
#define COL_BROW        0x4A8C   // brighter blue-grey
#define COL_OUTLINE     0x6B6D   // brighter grey edges

// Warm amber eyes alternative — swap these three in:
// #define COL_IRIS_OUTER  0xC300
// #define COL_IRIS_MID    0x8200
// #define COL_IRIS_INNER  0x4100

// ── Animation speeds ──────────────────────────────────────────────────────────
#define GAZE_SPEED      0.12f
#define BLINK_SPEED     0.32f
#define SQUINT_SPEED    0.10f
#define BROW_SPEED      0.07f

// ── Backlight ─────────────────────────────────────────────────────────────────
#define PIN_LCD_BL          4

// ════════════════════════════════════════════════════════════════════════════
//  DISPLAY + SPRITE
// ════════════════════════════════════════════════════════════════════════════

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

// ════════════════════════════════════════════════════════════════════════════
//  EMOTIONS
// ════════════════════════════════════════════════════════════════════════════

enum Emotion {
  EMOTION_NEUTRAL = 0,
  EMOTION_HAPPY,
  EMOTION_SAD,
  EMOTION_ANGRY,
  EMOTION_SURPRISED,
  EMOTION_SUSPICIOUS,
  EMOTION_WORRIED,
  EMOTION_DISGUSTED,
  EMOTION_COUNT
};

struct BrowPose      { float raise, tilt; };
struct EmotionPreset { const char* name; BrowPose left, right; };

const EmotionPreset EMOTIONS[EMOTION_COUNT] = {
  { "Neutral",    { 0.0f,  0.0f }, { 0.0f,  0.0f } },
  { "Happy",      { 0.2f,  0.3f }, { 0.2f, -0.3f } },
  { "Sad",        {-0.2f,  0.7f }, {-0.2f, -0.7f } },
  { "Angry",      {-0.3f, -0.8f }, {-0.3f,  0.8f } },
  { "Surprised",  { 0.9f,  0.2f }, { 0.9f, -0.2f } },
  { "Suspicious", {-0.4f, -0.5f }, { 0.6f, -0.1f } },
  { "Worried",    { 0.4f,  0.6f }, { 0.4f, -0.6f } },
  { "Disgusted",  {-0.5f, -0.6f }, { 0.1f,  0.3f } },
};

// ════════════════════════════════════════════════════════════════════════════
//  STATE
// ════════════════════════════════════════════════════════════════════════════

struct EyeState {
  float gazeX=0, gazeY=0;
  float blinkAmount=0, squintAmount=0;
  float lBrowRaise=0, lBrowTilt=0;
  float rBrowRaise=0, rBrowTilt=0;
} eyes;

struct AnimTarget {
  float gazeX=0, gazeY=0;
  float blink=0, squint=0;
  float lBrowRaise=0, lBrowTilt=0;
  float rBrowRaise=0, rBrowTilt=0;
} target;

// ════════════════════════════════════════════════════════════════════════════
//  TIMING
// ════════════════════════════════════════════════════════════════════════════

unsigned long nextBlink=0, nextGaze=0, nextSaccade=0, blinkEnd=0;
bool inBlink=false, autonomousMode=true, squinting=false;

// ════════════════════════════════════════════════════════════════════════════
//  MATH
// ════════════════════════════════════════════════════════════════════════════

float lerpf(float a, float b, float t)    { return a + (b-a)*t; }
float clampf(float v, float lo, float hi) { return v<lo?lo:(v>hi?hi:v); }
float randF(float lo, float hi)           { return lo + (float)random(10000)/10000.f*(hi-lo); }

// ════════════════════════════════════════════════════════════════════════════
//  DRAW — EYEBROW
// ════════════════════════════════════════════════════════════════════════════

void drawBrow(int cx, int browBaseY, float raise, float tilt) {
  int raisePx = (int)(raise * BROW_RAISE_RANGE);
  int tiltPx  = (int)(tilt  * BROW_TILT_RANGE);
  int midY    = browBaseY - raisePx;
  bool isLeft = (cx < SCREEN_W / 2);

  int innerX, outerX, innerY, outerY;
  if (isLeft) {
    innerX = cx + BROW_HALF_W;  outerX = cx - BROW_HALF_W;
    innerY = midY - tiltPx;     outerY = midY + tiltPx;
  } else {
    innerX = cx - BROW_HALF_W;  outerX = cx + BROW_HALF_W;
    innerY = midY + tiltPx;     outerY = midY - tiltPx;
  }

  for (int t = 0; t < BROW_THICKNESS; t++) {
    int dy = t - BROW_THICKNESS / 2;
    spr.drawLine(innerX, innerY+dy, outerX, outerY+dy, COL_BROW);
  }
  spr.drawLine(innerX, innerY - BROW_THICKNESS/2 - 1,
               outerX, outerY - BROW_THICKNESS/2 - 1, COL_OUTLINE);
  spr.drawLine(innerX, innerY + BROW_THICKNESS/2 + 1,
               outerX, outerY + BROW_THICKNESS/2 + 1, COL_OUTLINE);
}

// ════════════════════════════════════════════════════════════════════════════
//  DRAW — ONE EYE
// ════════════════════════════════════════════════════════════════════════════

void drawEye(int cx, int cy, float gazeX, float gazeY,
             float blink, float squint) {

  int rx = EYE_RX, ry = EYE_RY;
  int px = cx + (int)(gazeX * MAX_GAZE_X);
  int py = cy + (int)(gazeY * MAX_GAZE_Y);

  // Sclera
  spr.fillEllipse(cx, cy, rx, ry, COL_SCLERA);

  // Iris — layered rings
  spr.fillCircle(px, py, IRIS_R,      COL_IRIS_OUTER);
  spr.fillCircle(px, py, IRIS_R - 5,  COL_IRIS_MID);
  spr.fillCircle(px, py, IRIS_R - 10, COL_IRIS_INNER);

  // Pupil
  spr.fillCircle(px, py, PUPIL_R, COL_PUPIL);

  // Specular shine
  spr.fillCircle(px + SHINE_OX, py + SHINE_OY, SHINE_R,   COL_SHINE);
  spr.fillCircle(px - SHINE_OX/2, py + SHINE_OY/2, SHINE_R/2+1, COL_SHINE);

  // Top eyelid
  float topClose = clampf(squint * 0.45f + blink, 0.0f, 1.0f);
  int   lidBot   = (cy - ry) + (int)(topClose * (ry * 2 + 4));
  if (lidBot > cy - ry)
    spr.fillRect(cx-rx-2, cy-ry-2, rx*2+4, lidBot-(cy-ry)+2, COL_EYELID);

  // Bottom eyelid
  float botClose = clampf(blink * 0.5f, 0.0f, 1.0f);
  int   lidTop   = (cy + ry) - (int)(botClose * ry);
  if (lidTop < cy + ry)
    spr.fillRect(cx-rx-2, lidTop, rx*2+4, (cy+ry+2)-lidTop, COL_EYELID);

  // Socket outline
  spr.drawEllipse(cx, cy, rx,   ry,   COL_OUTLINE);
  spr.drawEllipse(cx, cy, rx+1, ry+1, COL_OUTLINE);
}

// ════════════════════════════════════════════════════════════════════════════
//  DRAW — FULL FRAME
// ════════════════════════════════════════════════════════════════════════════

void drawFrame() {
  spr.fillSprite(COL_BG);

  drawEye(LEFT_EYE_CX,  EYE_CY, eyes.gazeX, eyes.gazeY,
          eyes.blinkAmount, eyes.squintAmount);
  drawEye(RIGHT_EYE_CX, EYE_CY, eyes.gazeX, eyes.gazeY,
          eyes.blinkAmount, eyes.squintAmount);

  int browY = EYE_CY - EYE_RY - BROW_EYE_GAP;
  drawBrow(LEFT_EYE_CX,  browY, eyes.lBrowRaise, eyes.lBrowTilt);
  drawBrow(RIGHT_EYE_CX, browY, eyes.rBrowRaise, eyes.rBrowTilt);

  spr.pushSprite(0, 0);
}

// ════════════════════════════════════════════════════════════════════════════
//  LERP STATE
// ════════════════════════════════════════════════════════════════════════════

void lerpState() {
  eyes.gazeX        = lerpf(eyes.gazeX,        target.gazeX,       GAZE_SPEED);
  eyes.gazeY        = lerpf(eyes.gazeY,        target.gazeY,       GAZE_SPEED);
  eyes.blinkAmount  = lerpf(eyes.blinkAmount,  target.blink,       BLINK_SPEED);
  eyes.squintAmount = lerpf(eyes.squintAmount, target.squint,      SQUINT_SPEED);
  eyes.lBrowRaise   = lerpf(eyes.lBrowRaise,   target.lBrowRaise,  BROW_SPEED);
  eyes.lBrowTilt    = lerpf(eyes.lBrowTilt,    target.lBrowTilt,   BROW_SPEED);
  eyes.rBrowRaise   = lerpf(eyes.rBrowRaise,   target.rBrowRaise,  BROW_SPEED);
  eyes.rBrowTilt    = lerpf(eyes.rBrowTilt,    target.rBrowTilt,   BROW_SPEED);
}

// ════════════════════════════════════════════════════════════════════════════
//  AUTONOMOUS ANIMATION
// ════════════════════════════════════════════════════════════════════════════

void tickBlink(unsigned long now) {
  if (!inBlink && now >= nextBlink) {
    inBlink = true;  target.blink = 1.0f;
    blinkEnd  = now + random(60, 130);
    nextBlink = now + random(2000, 6000);
  }
  if (inBlink && now >= blinkEnd) {
    target.blink = 0.0f;  inBlink = false;
  }
}

void updateAnimation() {
  unsigned long now = millis();
  tickBlink(now);

  if (now >= nextGaze) {
    target.gazeX = randF(-1.0f, 1.0f);
    target.gazeY = randF(-0.6f, 0.6f);
    nextGaze = now + random(1500, 4500);
  }
  if (now >= nextSaccade) {
    target.gazeX = clampf(target.gazeX + randF(-0.15f, 0.15f), -1.f, 1.f);
    target.gazeY = clampf(target.gazeY + randF(-0.10f, 0.10f), -1.f, 1.f);
    nextSaccade = now + random(200, 700);
  }
  lerpState();
}

// ════════════════════════════════════════════════════════════════════════════
//  EXTERNAL CONTROL API
// ════════════════════════════════════════════════════════════════════════════

void cmdLook(float x, float y) {
  autonomousMode = false;
  target.gazeX = clampf(x, -1.f, 1.f);
  target.gazeY = clampf(y, -1.f, 1.f);
}
void cmdBlink() {
  inBlink = true;  target.blink = 1.0f;
  blinkEnd = millis() + 80;
}
void cmdSquint(float a) { target.squint = clampf(a, 0.f, 1.f); }

void cmdEmotion(Emotion e) {
  if (e < 0 || e >= EMOTION_COUNT) return;
  const EmotionPreset& p = EMOTIONS[e];
  target.lBrowRaise = p.left.raise;   target.lBrowTilt = p.left.tilt;
  target.rBrowRaise = p.right.raise;  target.rBrowTilt = p.right.tilt;
  Serial.print("Emotion: "); Serial.println(p.name);
}
void cmdCenter() { target.gazeX = 0; target.gazeY = 0; }
void cmdIdle() {
  autonomousMode = true;  target.squint = 0;
  nextBlink   = millis() + random(500, 1500);
  nextGaze    = millis() + random(500, 1000);
  nextSaccade = millis() + 300;
}

// ════════════════════════════════════════════════════════════════════════════
//  SERIAL TEST INTERFACE
// ════════════════════════════════════════════════════════════════════════════

void handleSerial() {
  while (Serial.available()) {
    char ch = (char)Serial.read();
    switch (ch) {
      case 'l': cmdLook(-1.f, 0.f);  Serial.println("LEFT");     break;
      case 'r': cmdLook( 1.f, 0.f);  Serial.println("RIGHT");    break;
      case 'u': cmdLook( 0.f,-1.f);  Serial.println("UP");       break;
      case 'd': cmdLook( 0.f, 1.f);  Serial.println("DOWN");     break;
      case 'c': cmdCenter();         Serial.println("CENTER");    break;
      case 'b': cmdBlink();          Serial.println("BLINK");     break;
      case 'i': cmdIdle();           Serial.println("IDLE");      break;
      case 's':
        squinting = !squinting;
        cmdSquint(squinting ? 0.7f : 0.0f);
        Serial.println(squinting ? "SQUINT ON" : "SQUINT OFF");
        break;
      case '0': cmdEmotion(EMOTION_NEUTRAL);    break;
      case '1': cmdEmotion(EMOTION_HAPPY);      break;
      case '2': cmdEmotion(EMOTION_SAD);        break;
      case '3': cmdEmotion(EMOTION_ANGRY);      break;
      case '4': cmdEmotion(EMOTION_SURPRISED);  break;
      case '5': cmdEmotion(EMOTION_SUSPICIOUS); break;
      case '6': cmdEmotion(EMOTION_WORRIED);    break;
      case '7': cmdEmotion(EMOTION_DISGUSTED);  break;
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
//  SETUP / LOOP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));

  // Backlight on
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  tft.init();
  tft.setRotation(DISPLAY_ROTATION);
  tft.fillScreen(COL_BG);

  // Sprite as double-buffer — classic ESP32 has no PSRAM so we get 240×135×2 = ~65KB
  // This fits in DRAM just fine on the classic ESP32 with 320KB
  spr.createSprite(SCREEN_W, SCREEN_H);
  spr.setColorDepth(16);

  nextBlink   = millis() + random(1000, 3000);
  nextGaze    = millis() + random(500,  2000);
  nextSaccade = millis() + 400;

  cmdEmotion(EMOTION_NEUTRAL);

  Serial.println("AnimatronicEyes T-Display ready.");
  Serial.println("l r u d c b s i   0-7 for emotions");
}

void loop() {
  handleSerial();

  if (autonomousMode) {
    updateAnimation();
  } else {
    tickBlink(millis());
    lerpState();
  }

  drawFrame();
}
