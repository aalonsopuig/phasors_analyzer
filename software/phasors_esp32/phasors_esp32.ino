/*
===============================================================================
Name:         phasors_esp32
Version:      1.0.0
Author:       Alejandro Alonso Puig (https://github.com/aalonsopuig) + GPT
Date:         2026-05
License:      Apache 2.0
-------------------------------------------------------------------------------
Description:

Educational phasor analyzer proof of concept for ESP32 + SSD1306 OLED display.

The system measures:
- voltage across the DUT
- current through the DUT

using two conditioned analog channels connected to the ESP32 ADC.

The instrument estimates:
- current phase relative to voltage
- DUT impedance phase angle

and displays the corresponding phasors graphically.

The DUT is excited using an external floating sinewave generator.

Current measurement is performed indirectly using a series sensing resistor:

    Rsense = 100 ohm

The voltage across the sensing resistor is measured and used to infer current.

The analog front-end converts bipolar analog signals into the 0..3.3V range
required by the ESP32 ADC using passive offsetting circuitry.

Hardware:
- ESP32 classic
- SSD1306 OLED 128x64 I2C
- GPIO34: voltage channel
- GPIO35: current channel
- GPIO25: pushbutton to GND

Calibration sequence:
1. Short probes
2. Open probes
3. Connect 330 ohm calibration resistor

===============================================================================
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// -----------------------------------------------------------------------------
// OLED configuration
// -----------------------------------------------------------------------------

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT   64
#define OLED_RESET      -1
#define OLED_ADDR     0x3C

// OLED display object
Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET
);

// -----------------------------------------------------------------------------
// Pin configuration
// -----------------------------------------------------------------------------

const int PIN_V = 34;       // Voltage channel ADC input
const int PIN_I = 35;       // Current channel ADC input
const int BUTTON_PIN = 25;  // Pushbutton connected to GND

// -----------------------------------------------------------------------------
// Measurement configuration
// -----------------------------------------------------------------------------

// Current sensing resistor value in ohms.
// Lower than previous 330 ohm version to improve measurements on small
// inductors such as 1mH coils.
const float R_SENSE = 100.0;

// -----------------------------------------------------------------------------
// Sampling configuration
// -----------------------------------------------------------------------------

// Number of temporal samples captured per measurement block.
const int SAMPLE_COUNT = 256;

// Approximate sampling frequency.
const int SAMPLE_RATE_HZ = 10000;

// Time between samples in microseconds.
const int SAMPLE_PERIOD_US = 1000000L / SAMPLE_RATE_HZ;

// Buffers storing centered ADC samples.
int16_t vSamples[SAMPLE_COUNT];
int16_t iSamples[SAMPLE_COUNT];

// -----------------------------------------------------------------------------
// ADC offset calibration values
// -----------------------------------------------------------------------------

// Initial approximate midpoint values.
// Actual values are measured during calibration.
int zeroV = 2048;
int zeroI = 2048;

// -----------------------------------------------------------------------------
// Instrumental phase correction
// -----------------------------------------------------------------------------

// Stores measured systematic phase error introduced by:
// - non-simultaneous ADC sampling
// - ADC timing
// - analog circuitry
// - software latency
float phaseOffsetDeg = 0.0;

// -----------------------------------------------------------------------------
// Phasor graphics configuration
// -----------------------------------------------------------------------------

const int PH_CENTER_X = 31;
const int PH_CENTER_Y = 32;
const int PH_RADIUS   = 27;

const int TEXT_X = 68;

// =============================================================================
// SETUP
// =============================================================================

void setup() {

  // Serial debug port.
  Serial.begin(115200);

  // Button uses internal pull-up.
  // Button pressed -> GPIO connected to GND -> LOW.
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Configure ESP32 ADC resolution.
  analogReadResolution(12);

  // Configure ADC attenuation for approx. 0..3.3V range.
  analogSetAttenuation(ADC_11db);

  // Initialize I2C bus.
  Wire.begin(21, 22);

  // Initialize OLED display.
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    while (true);
  }

  // Startup screen.
  showSplash();

  // Guided calibration sequence.
  runCalibration();
}

// =============================================================================
// MAIN LOOP
// =============================================================================

void loop() {

  // Capture temporal waveform block.
  captureBlock();

  // Variable storing current phase relative to voltage.
  float currentPhaseDeg = 0.0;

  // Calculate phase.
  bool ok = calculateCurrentPhase(currentPhaseDeg);

  // Remove previously measured instrumental phase error.
  currentPhaseDeg -= phaseOffsetDeg;

  // Normalize angle to [-180,+180].
  currentPhaseDeg = normalizeAngle(currentPhaseDeg);

  // Impedance phase is opposite sign:
  //
  // Z = V / I
  //
  // Therefore:
  //
  // angle(Z) = angle(V) - angle(I)
  //
  // Since voltage is reference (0 deg):
  //
  // angle(Z) = -angle(I)
  //
  float impedancePhaseDeg = -currentPhaseDeg;

  impedancePhaseDeg = normalizeAngle(impedancePhaseDeg);

  // Draw phasor visualization.
  drawPhasors(
    currentPhaseDeg,
    impedancePhaseDeg,
    ok
  );

  // Slow refresh intentionally chosen for stability and readability.
  delay(500);
}

// =============================================================================
// SPLASH SCREEN
// =============================================================================

void showSplash() {

  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(0, 10);
  display.println("PHASORS");

  display.setTextSize(1);
  display.setCursor(0, 42);
  display.println("phasors_esp32");

  display.setCursor(0, 54);
  display.println("v1.0");

  display.display();

  delay(2000);
}

// =============================================================================
// CALIBRATION SEQUENCE
// =============================================================================

void runCalibration() {

  // ---------------------------------------------------------------------------
  // STEP 1:
  // Short probes.
  //
  // Used to calibrate voltage channel offset.
  // ---------------------------------------------------------------------------

  showMessage(
    "Short",
    "probes",
    "Press button"
  );

  waitButton();

  zeroV = averageAdc(PIN_V);

  // ---------------------------------------------------------------------------
  // STEP 2:
  // Open probes.
  //
  // Used to calibrate current channel offset.
  // ---------------------------------------------------------------------------

  showMessage(
    "Open",
    "probes",
    "Press button"
  );

  waitButton();

  zeroI = averageAdc(PIN_I);

  // ---------------------------------------------------------------------------
  // STEP 3:
  // Connect pure resistive DUT.
  //
  // This measures instrumental phase error.
  // Ideally a resistor should produce 0 degrees phase shift.
  // Any measured phase is considered systematic instrument error.
  // ---------------------------------------------------------------------------

  showMessage(
    "Connect",
    "330R",
    "Press button"
  );

  waitButton();

  captureBlock();

  float rawPhase = 0.0;

  bool ok = calculateCurrentPhase(rawPhase);

  if (ok) {
    phaseOffsetDeg = rawPhase;
  }
  else {
    phaseOffsetDeg = 0.0;
  }

  // ---------------------------------------------------------------------------
  // Calibration finished.
  // ---------------------------------------------------------------------------

  showMessage(
    "Connect",
    "DUT",
    "Press button"
  );

  waitButton();
}

// =============================================================================
// BUTTON HANDLING
// =============================================================================

void waitButton() {

  // Wait until button released.
  while (digitalRead(BUTTON_PIN) == LOW) {
    delay(10);
  }

  // Wait for press.
  while (digitalRead(BUTTON_PIN) == HIGH) {
    delay(10);
  }

  // Debounce.
  delay(40);

  // Wait until release again.
  while (digitalRead(BUTTON_PIN) == LOW) {
    delay(10);
  }

  delay(150);
}

// =============================================================================
// ADC OFFSET MEASUREMENT
// =============================================================================

int averageAdc(int pin) {

  long sum = 0;

  const int n = 300;

  for (int i = 0; i < n; i++) {

    sum += analogRead(pin);

    delay(2);
  }

  return sum / n;
}

// =============================================================================
// TEMPORAL SIGNAL ACQUISITION
// =============================================================================

void captureBlock() {

  // Timestamp scheduling used to keep sample spacing approximately constant.
  unsigned long nextSampleTime = micros();

  for (int n = 0; n < SAMPLE_COUNT; n++) {

    // Wait until next acquisition instant.
    while ((long)(micros() - nextSampleTime) < 0) {
    }

    // Read voltage channel.
    int rawV = analogRead(PIN_V);

    // Read current channel.
    int rawI = analogRead(PIN_I);

    // Remove calibrated offsets.
    vSamples[n] = rawV - zeroV;

    // Current channel inverted due to actual physical polarity of the
    // sensing resistor wiring.
    iSamples[n] = -(rawI - zeroI);

    // Schedule next sample.
    nextSampleTime += SAMPLE_PERIOD_US;
  }
}

// =============================================================================
// PHASE CALCULATION
// =============================================================================

bool calculateCurrentPhase(float &phaseDeg) {

  // Arrays storing interpolated rising zero crossing positions.
  float vCross[12];
  float iCross[12];

  // Detect crossings.
  int vCount = findRisingCrossings(
    vSamples,
    SAMPLE_COUNT,
    vCross,
    12
  );

  int iCount = findRisingCrossings(
    iSamples,
    SAMPLE_COUNT,
    iCross,
    12
  );

  // Not enough valid crossings.
  if (vCount < 2 || iCount < 1) {
    phaseDeg = 0.0;
    return false;
  }

  // ---------------------------------------------------------------------------
  // Estimate signal period using voltage crossings.
  // ---------------------------------------------------------------------------

  float period = 0.0;

  for (int i = 1; i < vCount; i++) {
    period += vCross[i] - vCross[i - 1];
  }

  period /= (vCount - 1);

  if (period <= 0.0) {
    phaseDeg = 0.0;
    return false;
  }

  // ---------------------------------------------------------------------------
  // Compare voltage and current crossings.
  // ---------------------------------------------------------------------------

  float deltaSum = 0.0;

  int used = 0;

  for (int v = 0; v < vCount; v++) {

    float bestDelta = 9999.0;

    for (int i = 0; i < iCount; i++) {

      float delta = iCross[i] - vCross[v];

      // Wrap into one period.
      while (delta >  period / 2.0) delta -= period;
      while (delta < -period / 2.0) delta += period;

      // Keep nearest crossing.
      if (abs(delta) < abs(bestDelta)) {
        bestDelta = delta;
      }
    }

    if (abs(bestDelta) < period / 2.0) {
      deltaSum += bestDelta;
      used++;
    }
  }

  if (used == 0) {
    phaseDeg = 0.0;
    return false;
  }

  // Average crossing difference.
  float deltaAvg = deltaSum / used;

  // Convert temporal shift into phase angle.
  //
  // Positive phase:
  // current leads voltage.
  //
  phaseDeg = -360.0 * deltaAvg / period;

  phaseDeg = normalizeAngle(phaseDeg);

  return true;
}

// =============================================================================
// RISING ZERO CROSSING DETECTION
// =============================================================================

int findRisingCrossings(
  int16_t *samples,
  int count,
  float *crossings,
  int maxCrossings
) {

  int found = 0;

  for (int n = 1; n < count; n++) {

    int16_t prev = samples[n - 1];
    int16_t curr = samples[n];

    // Rising crossing:
    // previous sample negative,
    // current sample positive.
    if (prev < 0 && curr >= 0) {

      // Linear interpolation improves crossing precision.
      float fraction =
        (float)(-prev) /
        (float)(curr - prev);

      crossings[found++] =
        (n - 1) + fraction;

      if (found >= maxCrossings) {
        break;
      }
    }
  }

  return found;
}

// =============================================================================
// ANGLE NORMALIZATION
// =============================================================================

float normalizeAngle(float angle) {

  while (angle > 180.0) angle -= 360.0;
  while (angle < -180.0) angle += 360.0;

  return angle;
}

// =============================================================================
// SIMPLE TEXT SCREEN
// =============================================================================

void showMessage(
  const char *line1,
  const char *line2,
  const char *line3
) {

  display.clearDisplay();

  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);

  display.setCursor(0, 0);
  display.println(line1);
  display.println(line2);

  display.setTextSize(1);

  display.setCursor(0, 48);
  display.println(line3);

  display.display();
}

// =============================================================================
// PHASOR VISUALIZATION
// =============================================================================

void drawPhasors(
  float currentPhaseDeg,
  float impedancePhaseDeg,
  bool valid
) {

  display.clearDisplay();

  // Draw reference circle and axes.
  drawCircleReference();

  // Voltage reference vector.
  drawVector(
    0.0,
    PH_RADIUS,
    "V"
  );

  // Current vector.
  drawVector(
    currentPhaseDeg,
    PH_RADIUS - 6,
    "I"
  );

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(TEXT_X, 0);
  display.println("PHASOR");

  display.setCursor(TEXT_X, 16);
  display.print("Z:");
  display.print(impedancePhaseDeg, 1);
  display.println("deg");

  display.setCursor(TEXT_X, 30);
  display.print("I:");
  display.print(currentPhaseDeg, 1);
  display.println("deg");

  display.setCursor(TEXT_X, 44);
  display.print("Rs:");
  display.print((int)R_SENSE);
  display.println("R");

  display.setCursor(TEXT_X, 56);

  if (valid) {
    display.println("OK");
  }
  else {
    display.println("NO SYNC");
  }

  display.display();
}

// =============================================================================
// REFERENCE CIRCLE
// =============================================================================

void drawCircleReference() {

  display.drawCircle(
    PH_CENTER_X,
    PH_CENTER_Y,
    PH_RADIUS,
    SSD1306_WHITE
  );

  // Horizontal dotted axis.
  for (
    int x = PH_CENTER_X - PH_RADIUS;
    x <= PH_CENTER_X + PH_RADIUS;
    x += 4
  ) {
    display.drawPixel(
      x,
      PH_CENTER_Y,
      SSD1306_WHITE
    );
  }

  // Vertical dotted axis.
  for (
    int y = PH_CENTER_Y - PH_RADIUS;
    y <= PH_CENTER_Y + PH_RADIUS;
    y += 4
  ) {
    display.drawPixel(
      PH_CENTER_X,
      y,
      SSD1306_WHITE
    );
  }
}

// =============================================================================
// VECTOR DRAWING
// =============================================================================

void drawVector(
  float angleDeg,
  int length,
  const char *label
) {

  // Convert degrees to radians.
  float angleRad =
    angleDeg * PI / 180.0;

  // Endpoint coordinates.
  int x2 =
    PH_CENTER_X +
    length * cos(angleRad);

  int y2 =
    PH_CENTER_Y -
    length * sin(angleRad);

  // Draw vector line.
  display.drawLine(
    PH_CENTER_X,
    PH_CENTER_Y,
    x2,
    y2,
    SSD1306_WHITE
  );

  // Draw vector endpoint.
  display.fillCircle(
    x2,
    y2,
    2,
    SSD1306_WHITE
  );

  // Draw vector label.
  display.setTextSize(1);

  display.setTextColor(SSD1306_WHITE);

  display.setCursor(
    x2 + 2,
    y2 - 4
  );

  display.print(label);
}
