/**The MIT License (MIT)

 Copyright (c) 2018 by ThingPulse Ltd., https://thingpulse.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 */

#include "settings.h"

#include <Arduino.h>
#include "time.h"
#include <SPI.h>
#ifdef ESP32
#include <WiFi.h>
#endif
#ifdef ESP8266
#include <ESP8266WiFi.h>
#endif

#include "SunMoonCalc.h"
#include <JsonListener.h>
#include <OpenWeatherMapCurrent.h>
#include <OpenWeatherMapForecast.h>
#include <Astronomy.h>
#include <MiniGrafx.h>
#include <Carousel.h>

#ifdef DISPLAY_ST7789
#include <ST7789_SPI.h>
#define Display ST7789_SPI
#endif
#ifdef DISPLAY_ILI9341
#include <ILI9341_SPI.h>
#define Display ILI9341_SPI
#endif
#include "ArialRounded.h"
#include "moonphases.h"
#include "weathericons.h"

#include "main.h"

#define MINI_BLACK 0
#define MINI_WHITE 1
#define MINI_YELLOW 2
#define MINI_BLUE 3

#define MAX_FORECASTS 12

// defines the colors usable in the paletted 16 color frame buffer
uint16_t palette[] = {ILI9341_BLACK,  // 0
                      ILI9341_WHITE,  // 1
                      ILI9341_YELLOW, // 2
                      0x7E3C};        // 3

// Limited to 4 colors due to memory constraints
int BITS_PER_PIXEL = 2; // 2^2 =  4 colors

#ifdef ESP8266
ADC_MODE(ADC_VCC);
#endif

Display tft = Display(TFT_CS, TFT_DC, TFT_RST);
MiniGrafx gfx = MiniGrafx(&tft, BITS_PER_PIXEL, palette);
Carousel carousel(&gfx, 0, 0, tft.width(), 100);
const int itemsOnCarousel = tft.width() > 300 ? 4 : 3;

#if defined(TOUCH_CS) && defined(TOUCH_IRQ)
#define TOUCH_ENABLED
#include <TouchControllerWS.h>
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
TouchControllerWS touchController(&ts);

void calibrationCallback(int16_t x, int16_t y);
CalibrationCallback calibration = &calibrationCallback;
#endif

OpenWeatherMapCurrentData currentWeather;
OpenWeatherMapForecastData forecasts[MAX_FORECASTS];

Astronomy::MoonData moonData;
// SunMoonCalc::Moon moonData;

int frameCount = 3;
FrameCallback frames[] = {drawForecast1, drawForecast2, drawForecast3};

// how many different screens do we have?
int screenCount = 5;
long lastDownloadUpdate = 0;
long lastScreenChange = 0;

uint16_t screen = 0;
long timerPress;
bool canBtnPress;

void connectWifi()
{
  if (WiFi.status() == WL_CONNECTED)
    return;

  // Manual Wifi
  Serial.printf("Connecting to WiFi %s/%s", WIFI_SSID.c_str(), WIFI_PASS.c_str());
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.hostname(CONFIG_WIFI_HOSTNAME);
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASS.c_str());
  int i = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    if (i > 80)
      i = 0;
    drawProgress(i, "Connecting to WiFi '" + String(WIFI_SSID.c_str()) + "'");
    i += 10;
    Serial.print(".");
  }
  drawProgress(100, "Connected to WiFi '" + String(WIFI_SSID.c_str()) + "'");
  Serial.println("connected.");
  Serial.printf("Connected, IP address: %s/%s\n", WiFi.localIP().toString().c_str(), WiFi.subnetMask().toString().c_str()); // Get ip and subnet mask
  Serial.printf("Connected, MAC address: %s\n", WiFi.macAddress().c_str());                                                 // Get the local mac address
}

void initTime()
{
  time_t now;

  gfx.fillBuffer(MINI_BLACK);
  gfx.setFont(ArialRoundedMTBold_14);

  Serial.printf("Configuring time for timezone %s\n", TIMEZONE.c_str());
#ifdef ESP8266
  configTime(TIMEZONE.c_str(), NTP_SERVERS);
#endif
#ifdef ESP32
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVERS);
#endif
  int i = 1;
  while ((now = time(nullptr)) < NTP_MIN_VALID_EPOCH)
  {
    drawProgress(i * 10, "Updating time...");
    Serial.print(".");
    delay(300);
    yield();
    i++;
  }
  drawProgress(100, "Time synchronized");
  Serial.println();

  printf("Local time: %s", asctime(localtime(&now))); // print formated local time, same as ctime(&now)
  printf("UTC time:   %s", asctime(gmtime(&now)));    // print formated GMT/UTC time
}

void setup()
{
  Serial.begin(SERIAL_BAUD);
  delay(500);
  Serial.println("Starting...");

  loadPropertiesFromSpiffs();

#ifdef TFT_LED
  // The LED pin needs to set HIGH
  // Use this pin to save energy
  // Turn on the background LED
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH); // HIGH to Turn on;
#endif

  gfx.init();
  gfx.fillBuffer(MINI_BLACK);
  gfx.setRotation(TFT_ROTATION);
  gfx.commit();

  Serial.printf("TFT: w = %d, h = %d\n", tft.width(), tft.height());

  connectWifi();

  #ifdef TOUCH_ENABLED
  Serial.println("Initializing touch screen...");
  ts.begin();
  #endif

  Serial.println("Mounting file system...");
  bool isFSMounted = SPIFFS.begin();
  if (!isFSMounted)
  {
    Serial.println("Formatting file system...");
    drawProgress(50, "Formatting file system");
    SPIFFS.format();
  }
  drawProgress(100, "Formatting done");

  #ifdef TOUCH_ENABLED
  boolean isCalibrationAvailable = touchController.loadCalibration();
  if (!isCalibrationAvailable)
  {
    Serial.println("Calibration not available");
    touchController.startCalibration(&calibration);
    while (!touchController.isCalibrationFinished())
    {
      gfx.fillBuffer(0);
      gfx.setColor(MINI_YELLOW);
      gfx.setTextAlignment(TEXT_ALIGN_CENTER);
      gfx.drawString(tft.width()/2, 160, "Please calibrate\ntouch screen by\ntouch point");
      touchController.continueCalibration();
      gfx.commit();
      yield();
    }
    touchController.saveCalibration();
  }
  #endif

  carousel.setFrames(frames, frameCount);
  carousel.disableAllIndicators();

  initTime();

  // update the weather information
  updateData();
  lastDownloadUpdate = millis();

  lastScreenChange = millis();
  timerPress = millis();
  canBtnPress = true;
}

long lastDrew = 0;
bool btnClick;
// uint8_t MAX_TOUCHPOINTS = 10;
// TS_Point points[10];
// uint8_t currentTouchPoint = 0;

void loop()
{
  gfx.fillBuffer(MINI_BLACK);
  #ifdef TOUCH_ENABLED
  if (touchController.isTouched(0))
  {
    TS_Point p = touchController.getPoint();

    Serial.printf("Touch point detected at %d/%d.\n", p.x, p.y);

    if (p.y < 80)
    {
      IS_STYLE_12HR = !IS_STYLE_12HR;
    }
    else
    {
      screen = (screen + 1) % screenCount;
    }
  }
  #endif

  if (screen == 0)
  {
    drawTime();
    drawWifiQuality();
    int remainingTimeBudget = carousel.update();
    if (remainingTimeBudget > 0)
    {
      // You can do some work here
      // Don't do stuff if you are below your
      // time budget.
      delay(remainingTimeBudget);
    }
    drawCurrentWeather();
    drawAstronomy();
  }
  else if (screen == 1)
  {
    drawCurrentWeatherDetail();
  }
  else if (screen == 2)
  {
    drawForecastTable(0);
  }
  else if (screen == 3)
  {
    drawForecastTable(4);
  }
  else if (screen == 4)
  {
    drawAbout();
  }
  gfx.commit();

  // Check if we should update weather information
  if ((UPDATE_INTERVAL_SECS > 0) && (millis() - lastDownloadUpdate > 1000 * UPDATE_INTERVAL_SECS))
  {
    updateData();
    lastDownloadUpdate = millis();
  }

  // Check if screen should be changed automatically
  if ((SCREEN_CHANGE_SECS > 0) && (millis() - lastScreenChange > 1000 * SCREEN_CHANGE_SECS))
  {
    screen = (screen + 1) % screenCount;
    lastScreenChange = millis();
  }

  if ((SLEEP_INTERVAL_SECS > 0) && (millis() - timerPress >= SLEEP_INTERVAL_SECS * 1000))
  { // after 2 minutes go to sleep
    drawProgress(25, "Going to Sleep!");
    delay(1000);
    drawProgress(50, "Going to Sleep!");
    delay(1000);
    drawProgress(75, "Going to Sleep!");
    delay(1000);
    drawProgress(100, "Going to Sleep!");
// go to deepsleep for xx minutes or 0 = permanently
#ifdef ESP8266
    ESP.deepSleep(0, WAKE_RF_DEFAULT); // 0 delay = permanently to sleep
#endif
#ifdef ESP32
// not yet implemented for esp32
#endif
  }
}

// Update the internet based information and update screen
void updateData()
{
  time_t now = time(nullptr);

  gfx.fillBuffer(MINI_BLACK);
  gfx.setFont(ArialRoundedMTBold_14);

  drawProgress(50, "Updating conditions...");
  OpenWeatherMapCurrent *currentWeatherClient = new OpenWeatherMapCurrent();
  currentWeatherClient->setMetric(IS_METRIC);
  currentWeatherClient->setLanguage(OPEN_WEATHER_MAP_LANGUAGE);
  currentWeatherClient->updateCurrentById(&currentWeather, OPEN_WEATHER_MAP_API_KEY, OPEN_WEATHER_MAP_LOCATION_ID);
  delete currentWeatherClient;
  currentWeatherClient = nullptr;

  drawProgress(70, "Updating forecasts...");
  OpenWeatherMapForecast *forecastClient = new OpenWeatherMapForecast();
  forecastClient->setMetric(IS_METRIC);
  forecastClient->setLanguage(OPEN_WEATHER_MAP_LANGUAGE);
  uint8_t allowedHours[] = {12, 0};
  forecastClient->setAllowedHours(allowedHours, sizeof(allowedHours));
  forecastClient->updateForecastsById(forecasts, OPEN_WEATHER_MAP_API_KEY, OPEN_WEATHER_MAP_LOCATION_ID, MAX_FORECASTS);
  delete forecastClient;
  forecastClient = nullptr;

  drawProgress(80, "Updating astronomy...");
  Astronomy *astronomy = new Astronomy();
  moonData = astronomy->calculateMoonData(now);
  moonData.phase = astronomy->calculateMoonPhase(now);
  delete astronomy;
  astronomy = nullptr;
  // https://github.com/ThingPulse/esp8266-weather-station/issues/144 prevents using this
  //   // 'now' has to be UTC, lat/lng in degrees not raadians
  //   SunMoonCalc *smCalc = new SunMoonCalc(now - dstOffset, currentWeather.lat, currentWeather.lon);
  //   moonData = smCalc->calculateSunAndMoonData().moon;
  //   delete smCalc;
  //   smCalc = nullptr;
  Serial.printf("Free mem: %d\n", ESP.getFreeHeap());

  delay(1000);
}

// Progress bar helper
void drawProgress(uint8_t percentage, String text)
{
  gfx.fillBuffer(MINI_BLACK);
  gfx.drawPalettedBitmapFromPgm((tft.width() - ThingPulseLogo_Width) / 2, 5, ThingPulseLogo);
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.setColor(MINI_WHITE);
  gfx.drawString(tft.width() / 2, 90, "https://thingpulse.com");
  gfx.setColor(MINI_YELLOW);

  gfx.drawString(tft.width() / 2, 146, text);
  gfx.setColor(MINI_WHITE);
  gfx.drawRect(10, 168, tft.width() - 2 * 10, 15);
  gfx.setColor(MINI_BLUE);
  gfx.fillRect(12, 170, (tft.width() - 2 * 12) * percentage / 100, 11);

  gfx.commit();
}

// draws the clock
void drawTime()
{
  char time_str[11];
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);

  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setColor(MINI_WHITE);
  String date = WDAY_NAMES[timeinfo->tm_wday] + " " + MONTH_NAMES[timeinfo->tm_mon] + " " + String(timeinfo->tm_mday) + " " + String(1900 + timeinfo->tm_year);
  gfx.drawString(tft.width() / 2, 6, date);

  gfx.setFont(ArialRoundedMTBold_36);

  if (IS_STYLE_12HR)
  {                                               // 12:00
    int hour = (timeinfo->tm_hour + 11) % 12 + 1; // take care of noon and midnight
    if (IS_STYLE_HHMM)
    {
      sprintf(time_str, "%2d:%02d\n", hour, timeinfo->tm_min); // hh:mm
    }
    else
    {
      sprintf(time_str, "%2d:%02d:%02d\n", hour, timeinfo->tm_min, timeinfo->tm_sec); // hh:mm:ss
    }
  }
  else
  { // 24:00
    if (IS_STYLE_HHMM)
    {
      sprintf(time_str, "%02d:%02d\n", timeinfo->tm_hour, timeinfo->tm_min); // hh:mm
    }
    else
    {
      sprintf(time_str, "%02d:%02d:%02d\n", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec); // hh:mm:ss
    }
  }

  gfx.drawString(tft.width() / 2, 20, time_str);

  gfx.setTextAlignment(TEXT_ALIGN_LEFT);
  gfx.setFont(ArialMT_Plain_10);
  gfx.setColor(MINI_BLUE);
  if (IS_STYLE_12HR)
  {
    sprintf(time_str, "\n%s", timeinfo->tm_hour >= 12 ? "PM" : "AM");
    gfx.drawString(195, 27, time_str);
  }
}

// draws current weather information
void drawCurrentWeather()
{
  gfx.setTransparentColor(MINI_BLACK);
  gfx.drawPalettedBitmapFromPgm(0, 55, getMeteoconIconFromProgmem(currentWeather.icon));

  // Weather Text
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setColor(MINI_BLUE);
  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);
  gfx.drawString(tft.width() - 20, 65, DISPLAYED_LOCATION_NAME);

  gfx.setFont(ArialRoundedMTBold_36);
  gfx.setColor(MINI_WHITE);
  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);

  gfx.drawString(tft.width() - 20, 78, String(currentWeather.temp, 1) + (IS_METRIC ? "°C" : "°F"));

  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setColor(MINI_YELLOW);
  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);
  gfx.drawString(tft.width() - 20, 118, currentWeather.description);
}

void drawForecast1(MiniGrafx *display, CarouselState *state, int16_t x, int16_t y)
{
  uint8_t shift_x = tft.width() /itemsOnCarousel;
  for (uint8_t i = 0; i < itemsOnCarousel; i++)
  {
    drawForecastDetail(x + 10 + shift_x * i, y + 165, i);
  }
}

void drawForecast2(MiniGrafx *display, CarouselState *state, int16_t x, int16_t y)
{
  uint8_t shift_x = tft.width() /itemsOnCarousel;
  for (uint8_t i = 0; i < itemsOnCarousel; i++)
  {
    drawForecastDetail(x + 10 + shift_x * i, y + 165, (i + itemsOnCarousel));
  }
}

void drawForecast3(MiniGrafx *display, CarouselState *state, int16_t x, int16_t y)
{
  uint8_t shift_x = tft.width() /itemsOnCarousel;
  for (uint8_t i = 0; i < itemsOnCarousel; i++)
  {
    drawForecastDetail(x + 10 + shift_x * i, y + 165, (i + 2*itemsOnCarousel));
  }
}

// helper for the forecast columns
void drawForecastDetail(uint16_t x, uint16_t y, uint8_t dayIndex)
{
  gfx.setColor(MINI_YELLOW);
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  time_t time = forecasts[dayIndex].observationTime;
  struct tm *timeinfo = localtime(&time);
  gfx.drawString(x + 25, y - 15, WDAY_NAMES[timeinfo->tm_wday] + " " + String(timeinfo->tm_hour) + ":00");

  gfx.setColor(MINI_WHITE);
  gfx.drawString(x + 25, y, String(forecasts[dayIndex].temp, 1) + (IS_METRIC ? "°C" : "°F"));

  gfx.drawPalettedBitmapFromPgm(x, y + 15, getMiniMeteoconIconFromProgmem(forecasts[dayIndex].icon));
  gfx.setColor(MINI_BLUE);
  gfx.drawString(x + 25, y + 60, String(forecasts[dayIndex].rain, 1) + (IS_METRIC ? "mm" : "in"));
}

// draw moonphase and sunrise/set and moonrise/set
void drawAstronomy()
{

  gfx.setFont(MoonPhases_Regular_36);
  gfx.setColor(MINI_WHITE);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.drawString(tft.width() / 2, 275, String((char)(97 + (moonData.illumination * 26))));

  gfx.setColor(MINI_WHITE);
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.setColor(MINI_YELLOW);
  gfx.drawString(tft.width() / 2, 250, MOON_PHASES[moonData.phase]);

  gfx.setTextAlignment(TEXT_ALIGN_LEFT);
  gfx.setColor(MINI_YELLOW);
  gfx.drawString(5, 250, SUN_MOON_TEXT[0]);
  gfx.setColor(MINI_WHITE);
  time_t time = currentWeather.sunrise;
  gfx.drawString(5, 276, SUN_MOON_TEXT[1] + ":");
  gfx.drawString(45, 276, getTime(&time));
  time = currentWeather.sunset;
  gfx.drawString(5, 291, SUN_MOON_TEXT[2] + ":");
  gfx.drawString(45, 291, getTime(&time));

  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);
  gfx.setColor(MINI_YELLOW);
  gfx.drawString(tft.width() - 5, 250, SUN_MOON_TEXT[3]);
  gfx.setColor(MINI_WHITE);
  float lunarMonth = 29.53;
  // approximate moon age
  gfx.drawString(tft.width() - 5, 276, String(moonData.phase <= 4 ? lunarMonth * moonData.illumination / 2.0 : lunarMonth - moonData.illumination * lunarMonth / 2.0, 1) + "d");
  gfx.drawString(tft.width() - 5, 291, String(moonData.illumination * 100, 0) + "%");
  gfx.drawString(tft.width() - 45, 276, SUN_MOON_TEXT[4] + ":");
  gfx.drawString(tft.width() - 45, 291, SUN_MOON_TEXT[5] + ":");
}

void drawCurrentWeatherDetail()
{
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.setColor(MINI_WHITE);
  gfx.drawString(tft.width() / 2, 2, "Current Conditions");

  // String weatherIcon;
  // String weatherText;
  drawLabelValue(0, "Temperature:", String(currentWeather.temp, 1) + (IS_METRIC ? "°C" : "°F"));
  drawLabelValue(1, "Wind Speed:", String(currentWeather.windSpeed, 1) + (IS_METRIC ? "m/s" : "mph"));
  drawLabelValue(2, "Wind Dir:", String(currentWeather.windDeg, 1) + "°");
  drawLabelValue(3, "Humidity:", String(currentWeather.humidity) + "%");
  drawLabelValue(4, "Pressure:", String(currentWeather.pressure) + "hPa");
  drawLabelValue(5, "Clouds:", String(currentWeather.clouds) + "%");
  drawLabelValue(6, "Visibility:", String(currentWeather.visibility) + "m");
}

void drawLabelValue(uint8_t line, String label, String value)
{
  const uint8_t labelX = 15;
  const uint8_t valueX = tft.width() / 2;
  gfx.setTextAlignment(TEXT_ALIGN_LEFT);
  gfx.setColor(MINI_YELLOW);
  gfx.drawString(labelX, 30 + line * 15, label);
  gfx.setColor(MINI_WHITE);
  gfx.drawString(valueX, 30 + line * 15, value);
}

// converts the dBm to a range between 0 and 100%
int8_t getWifiQuality()
{
  int32_t dbm = WiFi.RSSI();
  if (dbm <= -100)
  {
    return 0;
  }
  else if (dbm >= -50)
  {
    return 100;
  }
  else
  {
    return 2 * (dbm + 100);
  }
}

void drawWifiQuality()
{
  int8_t quality = getWifiQuality();
  gfx.setColor(MINI_WHITE);
  gfx.setFont(ArialMT_Plain_10);
  gfx.setTextAlignment(TEXT_ALIGN_RIGHT);
  gfx.drawString(tft.width() - 10, 9, String(quality) + "%");
  for (int8_t i = 0; i < 4; i++)
  {
    for (int8_t j = 0; j < 2 * (i + 1); j++)
    {
      if (quality > i * 25 || j == 0)
      {
        gfx.setPixel(tft.width() - 10 + 2 * i, 18 - j);
      }
    }
  }
}

void drawForecastTable(uint8_t start)
{
  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.setColor(MINI_WHITE);
  gfx.drawString(tft.width() / 2, 2, "Forecasts");
  uint16_t y = 0;

  String degreeSign = "°F";
  if (IS_METRIC)
  {
    degreeSign = "°C";
  }
  int firstColumnLabelX = 50;
  int firstColumnValueX = firstColumnLabelX + 20;
  int secondColumnLabelX = tft.width() / 2 + 10;
  int secondColumnValueX = secondColumnLabelX + 40;
  for (uint8_t i = start; i < start + 4; i++)
  {
    gfx.setTextAlignment(TEXT_ALIGN_LEFT);
    y = 45 + (i - start) * 75;
    if (y > tft.width())
    {
      break;
    }
    gfx.setColor(MINI_WHITE);
    gfx.setTextAlignment(TEXT_ALIGN_CENTER);
    time_t time = forecasts[i].observationTime;
    struct tm *timeinfo = localtime(&time);
    gfx.drawString(tft.width() / 2, y - 15, WDAY_NAMES[timeinfo->tm_wday] + " " + String(timeinfo->tm_hour) + ":00");

    gfx.drawPalettedBitmapFromPgm(0, 5 + y, getMiniMeteoconIconFromProgmem(forecasts[i].icon));
    gfx.setTextAlignment(TEXT_ALIGN_LEFT);
    gfx.setColor(MINI_YELLOW);
    gfx.setFont(ArialRoundedMTBold_14);
    gfx.drawString(0, y - 8, forecasts[i].main);
    gfx.setTextAlignment(TEXT_ALIGN_LEFT);

    gfx.setColor(MINI_BLUE);
    gfx.drawString(firstColumnLabelX, y, "T:");
    gfx.setColor(MINI_WHITE);
    gfx.drawString(firstColumnValueX, y, String(forecasts[i].temp, 0) + degreeSign);

    gfx.setColor(MINI_BLUE);
    gfx.drawString(firstColumnLabelX, y + 15, "H:");
    gfx.setColor(MINI_WHITE);
    gfx.drawString(firstColumnValueX, y + 15, String(forecasts[i].humidity) + "%");

    gfx.setColor(MINI_BLUE);
    gfx.drawString(firstColumnLabelX, y + 30, "P: ");
    gfx.setColor(MINI_WHITE);
    gfx.drawString(firstColumnValueX, y + 30, String(forecasts[i].rain, 2) + (IS_METRIC ? "mm" : "in"));

    gfx.setColor(MINI_BLUE);
    gfx.drawString(secondColumnLabelX, y, "Pr:");
    gfx.setColor(MINI_WHITE);
    gfx.drawString(secondColumnValueX, y, String(forecasts[i].pressure, 0) + "hPa");

    gfx.setColor(MINI_BLUE);
    gfx.drawString(secondColumnLabelX, y + 15, "WSp:");
    gfx.setColor(MINI_WHITE);
    gfx.drawString(secondColumnValueX, y + 15, String(forecasts[i].windSpeed, 0) + (IS_METRIC ? "m/s" : "mph"));

    gfx.setColor(MINI_BLUE);
    gfx.drawString(secondColumnLabelX, y + 30, "WDi: ");
    gfx.setColor(MINI_WHITE);
    gfx.drawString(secondColumnValueX, y + 30, String(forecasts[i].windDeg, 0) + "°");
  }
}

void drawAbout()
{
  gfx.fillBuffer(MINI_BLACK);
  gfx.drawPalettedBitmapFromPgm((tft.width() - ThingPulseLogo_Width) / 2, 5, ThingPulseLogo);

  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  gfx.setColor(MINI_WHITE);
  gfx.drawString(tft.width() / 2, 90, "https://thingpulse.com");

  gfx.setFont(ArialRoundedMTBold_14);
  gfx.setTextAlignment(TEXT_ALIGN_CENTER);
  drawLabelValue(7, "Heap Mem:", String(ESP.getFreeHeap() / 1024) + "kb");
#ifdef ESP8266
  drawLabelValue(8, "Flash Mem:", String(ESP.getFlashChipRealSize() / 1024 / 1024) + "MB");
#endif
  drawLabelValue(9, "WiFi Strength:", String(WiFi.RSSI()) + "dB");
#ifdef ESP8266
  drawLabelValue(10, "Chip ID:", String(ESP.getChipId()));
#endif
#ifdef ESP8266
  drawLabelValue(11, "VCC: ", String(ESP.getVcc() / 1024.0) + "V");
#endif
  drawLabelValue(12, "CPU Freq.: ", String(ESP.getCpuFreqMHz()) + "MHz");
  char time_str[15];
  const uint32_t millis_in_day = 1000 * 60 * 60 * 24;
  const uint32_t millis_in_hour = 1000 * 60 * 60;
  const uint32_t millis_in_minute = 1000 * 60;
  uint8_t days = millis() / (millis_in_day);
  uint8_t hours = (millis() - (days * millis_in_day)) / millis_in_hour;
  uint8_t minutes = (millis() - (days * millis_in_day) - (hours * millis_in_hour)) / millis_in_minute;
  sprintf(time_str, "%2dd%2dh%2dm", days, hours, minutes);
  drawLabelValue(13, "Uptime: ", time_str);
  drawLabelValue(14, "IP Address: ", WiFi.localIP().toString());
  gfx.setTextAlignment(TEXT_ALIGN_LEFT);
  gfx.setColor(MINI_YELLOW);
  gfx.drawString(15, 280, "Last Reset: ");
  gfx.setColor(MINI_WHITE);
#ifdef ESP8266
  gfx.drawStringMaxWidth(15, 295, 240 - 2 * 15, ESP.getResetInfo());
#endif
}

#ifdef TOUCH_ENABLED
void calibrationCallback(int16_t x, int16_t y)
{
  gfx.setColor(1);
  gfx.fillCircle(x, y, 10);
}
#endif

String getTime(time_t *timestamp)
{
  struct tm *timeInfo = localtime(timestamp);

  char buf[6];
  sprintf(buf, "%02d:%02d", timeInfo->tm_hour, timeInfo->tm_min);
  return String(buf);
}

void loadPropertiesFromSpiffs()
{
  if (SPIFFS.begin())
  {
    const char *msg = "Using '%s' from SPIFFS\n";
    Serial.println("Attempting to read application.properties file from SPIFFS.");
    File f = SPIFFS.open("/application.properties", "r");
    if (f)
    {
      Serial.println("File exists. Reading and assigning properties.");
      while (f.available())
      {
        String key = f.readStringUntil('=');
        String value = f.readStringUntil('\n');
        if (key == "ssid")
        {
          WIFI_SSID = value.c_str();
          Serial.printf(msg, "ssid");
        }
        else if (key == "password")
        {
          WIFI_PASS = value.c_str();
          Serial.printf(msg, "password");
        }
        else if (key == "timezone")
        {
          TIMEZONE = getTzInfo(value.c_str());
          Serial.printf(msg, "timezone");
        }
        else if (key == "owmApiKey")
        {
          OPEN_WEATHER_MAP_API_KEY = value.c_str();
          Serial.printf(msg, "owmApiKey");
        }
        else if (key == "owmLocationId")
        {
          OPEN_WEATHER_MAP_LOCATION_ID = value.c_str();
          Serial.printf(msg, "owmLocationId");
        }
        else if (key == "locationName")
        {
          DISPLAYED_LOCATION_NAME = value.c_str();
          Serial.printf(msg, "locationName");
        }
        else if (key == "isMetric")
        {
          IS_METRIC = value == "true" ? true : false;
          Serial.printf(msg, "isMetric");
        }
        else if (key == "is12hStyle")
        {
          IS_STYLE_12HR = value == "true" ? true : false;
          Serial.printf(msg, "is12hStyle");
        }
      }
    }
    f.close();
    Serial.println("Effective properties now as follows:");
    Serial.println("\tssid: " + WIFI_SSID);
    Serial.println("\tpassword: " + WIFI_PASS);
    Serial.println("\timezone: " + TIMEZONE);
    Serial.println("\tOWM API key: " + OPEN_WEATHER_MAP_API_KEY);
    Serial.println("\tOWM location id: " + OPEN_WEATHER_MAP_LOCATION_ID);
    Serial.println("\tlocation name: " + DISPLAYED_LOCATION_NAME);
    Serial.println("\tmetric: " + String(IS_METRIC ? "true" : "false"));
    Serial.println("\t12h style: " + String(IS_STYLE_12HR ? "true" : "false"));
  }
  else
  {
    Serial.println("SPIFFS mount failed.");
  }
}
