// Sticky Reminders — a reminder appliance on the Sticky board.
//
// Home: reminder list with a centered "New reminder" button and a settings
// gear. Settings: Wi-Fi/NTP clock sync and an alarm-buzzer toggle. Tapping a
// reminder opens a delete dialog. Built on FreeInkUI, the RTC, Buzzer and
// Icons libraries.

#include <EInkDisplay.h>
#include <BoardConfig.h>
#include <InputManager.h>
#include <FreeInkApp.h>
#include <FreeInkUIDisplayTarget.h>
#include <FreeInkUIInputManager.h>
#include <Rtc.h>
#include <Buzzer.h>
#include <WiFi.h>
#include <time.h>
#include "generated_icons.h"        // icon_bell_24, icon_clock_24, icon_wifi_24, ...
#include "generated_font_large.h"   // 32px Noto Sans (gen_font.py) for the settings labels
#include <cstdio>
#include <cstring>

using namespace freeink;

EInkDisplay display(
  BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi,
  BoardConfig::ACTIVE.display.cs,   BoardConfig::ACTIVE.display.dc,
  BoardConfig::ACTIVE.display.rst,  BoardConfig::ACTIVE.display.busy);

InputManager input;
Rtc          rtc;
Buzzer       buzzer;

// The framebuffer doesn't exist until display.begin(), so the DisplayTarget
// and App are constructed in setup() — not here at static init.
// Capacities: 64 interactions per frame, 24 handlers (the app registers 21 —
// a handler past the cap is silently dropped, check app->handlerOverflowed()).
using App = ui::FreeInkApp<64, 24>;
App* app = nullptr;

// Every DisplayTarget font slot defaults to the bundled 34px Noto Sans; slot 1
// is bound to the generated 45px-line variant in setup().
constexpr ui::FontId kFontLarge = 1;

static ui::BitmapRef iconRef(const Icon& i) {   // 1-bit mask (bit 1 = transparent)
  return { i.bits, i.w, i.h, ui::BitmapFormat::Mask1, /*progmem=*/true };
}

enum : ui::ActionId {
  ActionNew = 1, ActionOpen, ActionDelete, ActionDismiss,
  ActionKey, ActionShift, ActionMode, ActionBackspace, ActionSave, ActionLead,
  ActionOpenSettings, ActionToggleSound, ActionClockFmt, ActionOpenTz, ActionTz,
  ActionOpenWifi, ActionRescan, ActionPickNet, ActionConnect, ActionBack,
  ActionAlertDismiss
};
enum ScreenId { ScreenHome, ScreenNew, ScreenSettings, ScreenTz, ScreenWifiScan, ScreenWifiPass };

constexpr uint8_t kMaxReminders = 12;
constexpr uint8_t kMaxNets      = 12;

struct Reminder { char text[40]; uint16_t dueMin; bool fired; };
struct AppState {
  ScreenId      screen = ScreenHome;
  Rtc::DateTime now{};
  bool          haveTime = false;
  Reminder      items[kMaxReminders];
  uint8_t       count = 0;
  int16_t       selected = -1;
  int16_t       dialogFor = -1;                        // reminder index in the delete dialog
  int16_t       alertFor = -1;                         // reminder index in the active alert dialog
  bool          soundOn = true;
  bool          use24h = true;
  char          draft[40] = "";
  int16_t       leadMin = 1;
  int16_t       tzMin = 0;                             // UTC offset in minutes
  char          nets[kMaxNets][33];  uint8_t netCount = 0;   // scan results
  bool          scanPending = false;                   // scan runs after "Scanning…" is on the panel
  char          ssid[33] = "";                         // the picked network
  char          pass[64] = "";
  const char*   wifiStatus = "";
  // SDK-owned keyboard editing state: shift/symbol layers, layout-correct
  // UTF-8 append, multi-byte-aware backspace. attach() binds the field to edit.
  ui::KeyboardEntry kb;
} state;

struct BuzzPattern {
  bool     active = false;
  bool     toneOn = false;
  uint8_t  remaining = 0;
  uint32_t nextAt = 0;
};
BuzzPattern buzz;

static void startAlertBuzz() {
  buzzer.noTone();
  buzz.active = true;
  buzz.toneOn = false;
  buzz.remaining = 5;
  buzz.nextAt = 0;
}

static void stopAlertBuzz() {
  buzz.active = false;
  buzz.toneOn = false;
  buzz.remaining = 0;
  buzz.nextAt = 0;
  buzzer.noTone();
}

static void serviceAlertBuzz() {
  if (!buzz.active) return;
  const uint32_t now = millis();
  if (buzz.nextAt && static_cast<int32_t>(now - buzz.nextAt) < 0) return;

  if (buzz.toneOn) {
    buzzer.noTone();
    buzz.toneOn = false;
    buzz.nextAt = now + 160;
    return;
  }

  if (buzz.remaining == 0) {
    stopAlertBuzz();
    return;
  }

  buzzer.tone(2000, 0);
  buzz.toneOn = true;
  buzz.remaining--;
  buzz.nextAt = now + 120;
}


// Format a minutes-of-day value per the 12/24-hour setting.
static void formatMin(bool use24h, uint16_t min, char* buf, size_t n) {
  const uint8_t hh = min / 60, mm = min % 60;
  if (use24h) {
    std::snprintf(buf, n, "%02u:%02u", hh, mm);
  } else {
    uint8_t h = hh % 12;
    if (h == 0) h = 12;
    std::snprintf(buf, n, "%u:%02u %s", h, mm, hh < 12 ? "AM" : "PM");
  }
}

static void formatClock(const AppState& s, char* buf, size_t n) {
  if (!s.haveTime) { std::snprintf(buf, n, "--:--"); return; }
  formatMin(s.use24h, s.now.hour * 60 + s.now.minute, buf, n);
}

static void removeReminder(AppState& s, int16_t idx) {
  if (idx < 0 || idx >= s.count) return;
  for (uint8_t i = idx; i + 1 < s.count; ++i) s.items[i] = s.items[i + 1];
  s.count--;
  if (s.selected >= s.count) s.selected = s.count - 1;
}

// Switch screens. invalidateTransition() keeps transitions on FAST partial
// refreshes (a FULL is a 1-2s stall) with a periodic FULL to clear ghosting.
static void gotoScreen(AppState& s, ScreenId id) {
  s.screen = id;
  if (app) {
    app->invalidateTransition();
    // The tapped element doesn't exist on the next screen; without this a
    // same-action element there (e.g. its back button) inherits the gray.
    app->clearTapFlash();
  }
}

// Sub-screen chrome: back button + centered title + optional right label,
// without the SDK's default bottom rule (Home uses the status bar instead
// and has no back). An action/enabled pair puts a trailing button (Save) on
// the right edge instead of the label.
static void navHeader(App::ScreenType& screen, const char* title, const char* rightLabel = nullptr,
                      const char* actionLabel = nullptr, ui::ActionId action = ui::NO_ACTION,
                      bool actionEnabled = true) {
  screen.navHeader(title, ActionBack, iconRef(icon_arrow_left_24), rightLabel, ui::EdgesNone,
                   actionLabel, action, actionEnabled);
}

void appScreen(App::ScreenType& screen, void* user) {
  auto& s = *static_cast<AppState*>(user);
  const auto& theme = screen.theme();
  const bool modal = s.dialogFor >= 0 || s.alertFor >= 0;

  if (s.screen == ScreenNew) {
    // Save lives in the header's trailing slot, mirroring the back button.
    navHeader(screen, "New reminder", nullptr, "Save", ActionSave, !s.kb.empty());
    ui::QwertyKeyboardProps keys;
    keys.keyAction = ActionKey;  keys.shiftAction = ActionShift;
    keys.modeAction = ActionMode;
    keys.deleteAction = ActionBackspace;  keys.okAction = ActionSave;
    ui::applyEntry(keys, s.kb);
    screen.qwertyKeyboard(keys, 260, ui::LayoutAnchor::Bottom);

    char leadStr[12];
    std::snprintf(leadStr, sizeof leadStr, "%d min", s.leadMin);
    ui::StepperRowProps step;
    step.row.label = "Remind in";  step.value = leadStr;
    step.increment = ActionLead;   step.incrementValue = 5;
    step.decrement = ActionLead;   step.decrementValue = -5;
    // Control sizes derive from the value font; widestValue keeps the value
    // slot from shifting as the number changes.
    step.widestValue = "240 min";
    screen.stepperRow(step, ui::LayoutAnchor::Bottom);

    screen.insetContent({8, 12, 8, 12});
    ui::TextAreaProps body;
    body.text = s.draft;  body.cursor = static_cast<int16_t>(s.kb.length());  body.showCaret = true;
    screen.textArea(body);
    return;
  }

  if (s.screen == ScreenSettings) {
    static char clockVal[12];
    formatClock(s, clockVal, sizeof clockVal);

    navHeader(screen, "Settings", clockVal);

    screen.insetContent({8, 12, 0, 12});

    // Roomier settings list: large-font labels, a bigger switch, and taller
    // rows — the row builders scale the gap between rows with the height, so
    // taller rows also get more air. Icons and the right-side value/switch
    // align to the label's title band (the SDK's slot layout), leaving the
    // full row width for the subtitle underneath.
    ui::TextStyle label = theme.bodyText;
    label.font = kFontLarge;
    const int16_t rowH = static_cast<int16_t>(screen.target().lineHeight(kFontLarge) +
                                              screen.target().lineHeight(theme.smallText.font) + 16);
    auto bigRow = [&](ui::SettingRowProps& p, const Icon& icon) {
      p.labelText = label;  p.icon = iconRef(icon);
      screen.settingRow(p, rowH);
    };
    // hitToggleOnly: flipping a setting takes a deliberate tap on the switch,
    // not anywhere on the row.
    auto bigToggle = [&](ui::ToggleRowProps& p, const Icon& icon) {
      p.row.labelText = label;  p.row.icon = iconRef(icon);
      p.hitToggleOnly = true;
      p.toggleWidth = 56;  p.toggleHeight = 26;  p.knobInset = 4;
      screen.toggleRow(p, rowH);
    };

    ui::SettingRowProps wifi;
    wifi.label = "Wi-Fi & clock";
    wifi.subtitle = "Pick a network to sync the time";
    wifi.value = s.ssid[0] ? s.ssid : "Not set";
    wifi.action = ActionOpenWifi;
    bigRow(wifi, icon_wifi_24);

    ui::ToggleRowProps clockFmt;
    clockFmt.row.label = "24-hour time";
    clockFmt.row.subtitle = "Use 00:00-23:59 format";
    clockFmt.checked = s.use24h;
    clockFmt.toggleAction = ActionClockFmt;
    bigToggle(clockFmt, icon_clock_24);

    char tzStr[12];
    const int16_t tzAbs = s.tzMin < 0 ? -s.tzMin : s.tzMin;
    std::snprintf(tzStr, sizeof tzStr, "UTC%c%d:%02d", s.tzMin < 0 ? '-' : '+', tzAbs / 60, tzAbs % 60);
    ui::SettingRowProps tz;
    tz.label = "Adjust clock";
    tz.subtitle = "Dial the time to your zone";
    tz.value = tzStr;
    tz.action = ActionOpenTz;
    bigRow(tz, icon_globe_24);

    ui::ToggleRowProps sound;
    sound.row.label = "Alarm buzzer";
    sound.row.subtitle = "Beep when a reminder is due";
    sound.checked = s.soundOn;
    sound.toggleAction = ActionToggleSound;
    bigToggle(sound, icon_bell_24);
    return;
  }

  if (s.screen == ScreenTz) {
    navHeader(screen, "Adjust clock");

    char clockStr[12];
    formatClock(s, clockStr, sizeof clockStr);

    ui::Rect body = screen.body().inset({0, 16, 0, 16});
    const int16_t gap = static_cast<int16_t>(theme.spaceSm * 2);
    const int16_t controlH = static_cast<int16_t>(theme.rowHeight + 24);
    const int16_t buttonW = controlH;
    int16_t clockW = static_cast<int16_t>(body.width - buttonW * 2 - gap * 2);
    if (clockW > 220) clockW = 220;
    if (clockW < 96) clockW = 96;
    const int16_t controlW = static_cast<int16_t>(buttonW * 2 + clockW + gap * 2);
    ui::Rect control = ui::centeredRect(body, ui::Size{controlW, controlH});

    auto drawStep = [&](ui::Rect rect, bool plus, int16_t delta) {
      ui::ButtonProps props;
      props.action = ActionTz;
      props.value = delta;
      screen.button(props, rect);

      const int16_t half = 8;
      const int16_t cx = static_cast<int16_t>(rect.x + rect.width / 2);
      const int16_t cy = static_cast<int16_t>(rect.y + rect.height / 2);
      screen.target().line(ui::Point{static_cast<int16_t>(cx - half), cy},
                           ui::Point{static_cast<int16_t>(cx + half), cy}, 3, ui::Paint::solid(ui::Color::Black));
      if (plus) {
        screen.target().line(ui::Point{cx, static_cast<int16_t>(cy - half)},
                             ui::Point{cx, static_cast<int16_t>(cy + half)}, 3,
                             ui::Paint::solid(ui::Color::Black));
      }
    };

    ui::Rect minusRect{control.x, control.y, buttonW, controlH};
    ui::Rect clockRect{static_cast<int16_t>(minusRect.right() + gap), control.y, clockW, controlH};
    ui::Rect plusRect{static_cast<int16_t>(clockRect.right() + gap), control.y, buttonW, controlH};
    drawStep(minusRect, false, -15);

    ui::TextStyle val = theme.titleText;
    val.align = ui::TextAlign::Center;
    screen.target().text(clockRect, clockStr, val);

    drawStep(plusRect, true, 15);
    return;
  }

  if (s.screen == ScreenWifiScan) {
    navHeader(screen, "Choose a network");
    const ui::FooterAction footer[] = {
      { .label = "Rescan", .action = ActionRescan, .enabled = !s.scanPending },
    };
    screen.footer(footer, 1);

    if (s.netCount == 0) {
      // Scanning (or nothing found): a centered message instead of an empty list.
      screen.centeredText(s.scanPending ? "Scanning for networks..." : s.wifiStatus);
      return;
    }

    ui::ListItem rows[kMaxNets];
    for (uint8_t i = 0; i < s.netCount; ++i) {
      rows[i].label = s.nets[i];  rows[i].actionValue = i;  rows[i].icon = iconRef(icon_wifi_24);
    }
    screen.list(rows, s.netCount, s.selected, ActionPickNet);
    return;
  }

  if (s.screen == ScreenWifiPass) {
    // Connect stays enabled even with an empty password (open networks).
    navHeader(screen, s.ssid, nullptr, "Connect", ActionConnect);
    ui::QwertyKeyboardProps keys;
    keys.keyAction = ActionKey;  keys.shiftAction = ActionShift;
    keys.modeAction = ActionMode;
    keys.deleteAction = ActionBackspace;  keys.okAction = ActionConnect;
    ui::applyEntry(keys, s.kb);
    screen.qwertyKeyboard(keys, 260, ui::LayoutAnchor::Bottom);

    ui::SettingRowProps pw;
    pw.label = "Password";
    pw.subtitle = s.wifiStatus[0] ? s.wifiStatus : "Enter password";
    pw.value = s.kb.empty() ? "type below" : s.pass;
    screen.settingRow(pw);
    return;
  }

  // ---- Home ----------------------------------------------------------------
  char clock[12];
  formatClock(s, clock, sizeof clock);
  ui::StatusBarProps sb;
  sb.title = "Reminders";  sb.trailing = clock;  sb.leadingIcon = iconRef(icon_bell_24);
  screen.status(sb);

  // Bottom action bar: left-aligned New button + square settings button.
  // While the delete dialog is up the bar isn't drawn at all — a dithered
  // "dim" can't hide 1-bit content, and ghost buttons under a modal read as
  // a bug. The band is still consumed so the list doesn't shift.
  ui::Rect band = screen.takeBottom(static_cast<int16_t>(theme.rowHeight + 16), theme.spaceSm);
  if (!modal) {
    band = band.inset({4, 12, 4, 12});
    ui::Rect gearRect{static_cast<int16_t>(band.right() - band.height), band.y,
                      band.height, band.height};
    ui::ButtonProps gear;
    gear.icon = iconRef(icon_settings_24);  gear.action = ActionOpenSettings;
    screen.button(gear, gearRect);

    // Left-aligned New button filling the bar up to the gear.
    int16_t newW = static_cast<int16_t>(band.width - band.height - theme.spaceSm);
    if (newW > 320) newW = 320;
    ui::ButtonProps newBtn;
    newBtn.label = "New reminder";  newBtn.icon = iconRef(icon_plus_24);
    newBtn.action = ActionNew;  newBtn.gap = 8;
    screen.button(newBtn, ui::Rect{band.x, band.y, newW, band.height});
  }

  if (s.count == 0) {
    screen.centeredText("Nothing scheduled");
  } else {
    // The list, with due time and countdown per row.
    static char values[kMaxReminders][12];
    static char subs[kMaxReminders][20];
    const uint16_t nowMin = s.now.hour * 60 + s.now.minute;
    ui::ListItem rows[kMaxReminders];
    for (uint8_t i = 0; i < s.count; ++i) {
      formatMin(s.use24h, s.items[i].dueMin, values[i], sizeof values[i]);
      if (s.items[i].fired) {
        std::snprintf(subs[i], sizeof subs[i], "done");
      } else {
        const uint16_t in = (s.items[i].dueMin + 1440 - nowMin) % 1440;
        std::snprintf(subs[i], sizeof subs[i], "in %u min", in);
      }
      rows[i].label = s.items[i].text;
      rows[i].subtitle = subs[i];
      rows[i].value = values[i];
      rows[i].actionValue = i;
      rows[i].icon = iconRef(s.items[i].fired ? icon_bell_24 : icon_clock_24);
    }
    screen.insetContent({4, 12, 0, 12});
    screen.list(rows, s.count, s.selected, modal ? ui::NO_ACTION : ActionOpen);
  }

  // Delete dialog over the home screen.
  if (s.alertFor >= 0) {
    static const ui::DialogOption opts[] = {
      { .label = "Dismiss", .action = ActionAlertDismiss },
    };
    ui::OptionDialogProps dlg;
    dlg.title = "Reminder due";
    dlg.headline = s.items[s.alertFor].text;
    dlg.message = "Dismiss to silence the buzzer.";
    dlg.options = opts;
    dlg.optionCount = 1;
    dlg.dimBackground = true;
    screen.dialog(dlg);
  } else if (s.dialogFor >= 0) {
    static const ui::DialogOption opts[] = {
      { .label = "Delete", .action = ActionDelete },
      { .label = "Cancel", .action = ActionDismiss },
    };
    ui::OptionDialogProps dlg;
    dlg.title = "Reminder";
    dlg.headline = s.items[s.dialogFor].text;
    dlg.message = "Remove this reminder?";
    dlg.options = opts;
    dlg.optionCount = 2;
    dlg.dimBackground = true;
    screen.dialog(dlg);
  }
}

void scanNets(AppState& s) {
  WiFi.mode(WIFI_STA);  WiFi.disconnect();
  int n = WiFi.scanNetworks();          // blocks ~2–4 s
  s.netCount = 0;
  for (int i = 0; i < n && s.netCount < kMaxNets; ++i) {
    if (WiFi.SSID(i).isEmpty()) continue;
    std::strncpy(s.nets[s.netCount], WiFi.SSID(i).c_str(), 32);
    s.nets[s.netCount][32] = 0;
    s.netCount++;
  }
  WiFi.scanDelete();
  s.wifiStatus = s.netCount ? "Tap a network" : "None found - tap Rescan";
}

void syncClock(AppState& s) {
  s.wifiStatus = "Connecting...";  app->invalidate(ui::RefreshHint::Fast);
  WiFi.begin(s.ssid, s.pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(100);
  if (WiFi.status() != WL_CONNECTED) { s.wifiStatus = "Wrong password?"; return; }

  configTime(s.tzMin * 60L, 0, "pool.ntp.org");   // UTC offset from Settings
  struct tm t;
  if (!getLocalTime(&t, 8000)) { s.wifiStatus = "NTP failed"; WiFi.disconnect(true); return; }

  Rtc::DateTime dt{ (uint16_t)(t.tm_year + 1900), (uint8_t)(t.tm_mon + 1), (uint8_t)t.tm_mday,
                    (uint8_t)t.tm_hour, (uint8_t)t.tm_min, (uint8_t)t.tm_sec, (uint8_t)t.tm_wday };
  rtc.set(dt);
  WiFi.disconnect(true);
  gotoScreen(s, ScreenSettings);
}

void setup() {
  // Assert the board's power-rail latch first thing (Sticky: PWR_HOLD /
  // PWR_LOCK) — releasing these pins later is a software power-off.
  BoardConfig::holdPowerRails();

  // The SD card shares the display's SPI bus and its rail may be latched off
  // from a previous firmware's sleep; an unpowered card clamps SCLK/MOSI.
  // (SDCardManager::begin() does this itself — this app doesn't use SD.)
  BoardConfig::releaseSdRail();
  delay(10);

  delay(250);                 // let USB-Serial/JTAG power up before CDC begin
  Serial.begin(115200);
  Serial.setTxTimeoutMs(1);   // never block when no host is reading

  display.begin();  input.begin();  rtc.begin();  buzzer.begin();

  // Now the framebuffer exists — build the UI on top of it.
  static ui::DisplayTarget target(display.getFrameBuffer(), display.getDisplayWidth(),
                                  display.getDisplayHeight(), display.getDisplayWidthBytes());
  target.setFont(kFontLarge, ui::kNotoSansLargeFont);
  static App appInstance(target, target.deviceContext());
  app = &appInstance;

  // One theme tweak: outlined rounded buttons everywhere (nav back button,
  // footers, action bar) instead of the default filled look. Set once here;
  // every button then inherits it and needs no per-call styling.
  ui::ThemeTokens theme = app->theme();
  theme.button = ui::outlinedButtonStyles(10);
  app->setTheme(theme);

  // Frames don't clear the target on their own; let the app start every
  // paint from a white canvas so screens only draw what they show.
  app->setClearColor(ui::Color::White);

  app->setScreen(appScreen, &state);

  // The keyboard: KeyboardEntry owns append/backspace and the layer flags;
  // the handlers just route the four keyboard actions to it.
  app->on(ActionKey, [](const ui::ActionEvent& e, void* u){
    static_cast<AppState*>(u)->kb.key(e.value); }, &state);
  app->on(ActionBackspace, [](const ui::ActionEvent&, void* u){
    static_cast<AppState*>(u)->kb.backspace(); }, &state);
  app->on(ActionShift, [](const ui::ActionEvent&, void* u){
    static_cast<AppState*>(u)->kb.shift(); }, &state);
  app->on(ActionMode, [](const ui::ActionEvent&, void* u){
    static_cast<AppState*>(u)->kb.mode(); }, &state);

  // Reminder entry.
  app->on(ActionNew, [](const ui::ActionEvent&, void* u){
    auto& s = *static_cast<AppState*>(u);
    gotoScreen(s, ScreenNew); s.draft[0] = 0;
    s.kb.attach(s.draft, sizeof s.draft, /*startShifted=*/true); }, &state);
  app->on(ActionLead, [](const ui::ActionEvent& e, void* u){
    auto& s = *static_cast<AppState*>(u);
    s.leadMin += e.value; if (s.leadMin < 1) s.leadMin = 1; if (s.leadMin > 240) s.leadMin = 240; }, &state);
  app->on(ActionSave, [](const ui::ActionEvent&, void* u){
    auto& s = *static_cast<AppState*>(u);
    if (s.draft[0] && s.count < kMaxReminders) {
      uint16_t nowMin = s.now.hour * 60 + s.now.minute;
      std::strncpy(s.items[s.count].text, s.draft, 39);
      s.items[s.count].dueMin = (nowMin + s.leadMin) % 1440;
      s.items[s.count].fired = false;
      s.count++;
    }
    gotoScreen(s, ScreenHome); }, &state);

  // Reminder row -> delete dialog.
  app->on(ActionOpen, [](const ui::ActionEvent& e, void* u){
    auto& s = *static_cast<AppState*>(u);
    s.selected = e.value; s.dialogFor = e.value; }, &state);
  app->on(ActionDelete, [](const ui::ActionEvent&, void* u){
    auto& s = *static_cast<AppState*>(u);
    removeReminder(s, s.dialogFor); s.dialogFor = -1; }, &state);
  app->on(ActionDismiss, [](const ui::ActionEvent&, void* u){
    static_cast<AppState*>(u)->dialogFor = -1; }, &state);
  app->on(ActionAlertDismiss, [](const ui::ActionEvent&, void* u){
    static_cast<AppState*>(u)->alertFor = -1;
    stopAlertBuzz();
  }, &state);

  // Settings.
  app->on(ActionOpenSettings, [](const ui::ActionEvent&, void* u){
    gotoScreen(*static_cast<AppState*>(u), ScreenSettings); }, &state);
  app->on(ActionToggleSound, [](const ui::ActionEvent&, void* u){
    auto& s = *static_cast<AppState*>(u); s.soundOn = !s.soundOn; }, &state);
  app->on(ActionOpenTz, [](const ui::ActionEvent&, void* u){
    gotoScreen(*static_cast<AppState*>(u), ScreenTz); }, &state);
  app->on(ActionClockFmt, [](const ui::ActionEvent&, void* u){
    auto& s = *static_cast<AppState*>(u); s.use24h = !s.use24h; }, &state);
  app->on(ActionTz, [](const ui::ActionEvent& e, void* u){
    auto& s = *static_cast<AppState*>(u);
    int16_t next = s.tzMin + e.value;
    if (next > 840)  next = -720;   // UTC-12 .. UTC+14, wrapping at the ends
    if (next < -720) next = 840;
    const int16_t delta = next - s.tzMin;
    s.tzMin = next;
    // Move the running clock immediately; Rtc::adjust is calendar-correct and
    // works without a prior NTP sync (dialing the clock by hand sets it).
    if (delta && rtc.adjust(delta * 60L, &s.now)) s.haveTime = true;
  }, &state);

  // Wi-Fi: scan, pick a network, connect, or go back. The blocking scan is
  // deferred (scanPending) so the "Scanning…" screen reaches the panel first.
  app->on(ActionOpenWifi, [](const ui::ActionEvent&, void* u){
    auto& s = *static_cast<AppState*>(u);
    s.netCount = 0; s.scanPending = true; s.wifiStatus = "";
    gotoScreen(s, ScreenWifiScan); }, &state);
  app->on(ActionRescan, [](const ui::ActionEvent&, void* u){
    auto& s = *static_cast<AppState*>(u);
    s.netCount = 0; s.scanPending = true; s.wifiStatus = ""; }, &state);
  app->on(ActionPickNet, [](const ui::ActionEvent& e, void* u){
    auto& s = *static_cast<AppState*>(u);
    std::strncpy(s.ssid, s.nets[e.value], 32); s.ssid[32] = 0;
    s.pass[0] = 0; s.kb.attach(s.pass, sizeof s.pass); s.wifiStatus = "";
    gotoScreen(s, ScreenWifiPass); }, &state);
  app->on(ActionConnect, [](const ui::ActionEvent&, void* u){
    syncClock(*static_cast<AppState*>(u)); }, &state);
  app->on(ActionBack, [](const ui::ActionEvent&, void* u){
    auto& s = *static_cast<AppState*>(u);
    switch (s.screen) {
      case ScreenWifiPass: gotoScreen(s, ScreenWifiScan); break;
      case ScreenWifiScan: gotoScreen(s, ScreenSettings); break;
      case ScreenTz:       gotoScreen(s, ScreenSettings); break;
      default:             gotoScreen(s, ScreenHome);     break;
    } }, &state);
}

void loop() {
  input.update();
  serviceAlertBuzz();

  static uint32_t lastTick = 0;
  static uint16_t lastMinute = 0xFFFF;
  if (millis() - lastTick > 1000) {                 // read the RTC ~1 Hz
    lastTick = millis();
    if (rtc.now(state.now)) {
      state.haveTime = true;
      uint16_t nowMin = state.now.hour * 60 + state.now.minute;
      for (uint8_t i = 0; i < state.count; ++i) {
        if (!state.items[i].fired && nowMin == state.items[i].dueMin) {
          state.items[i].fired = true;
          state.alertFor = i;
          if (state.soundOn) startAlertBuzz();
          app->invalidate(ui::RefreshHint::Fast);
        }
      }
      if (nowMin != lastMinute) { lastMinute = nowMin; app->invalidate(ui::RefreshHint::Fast); }
    }
  }

  ui::InputSnapshot snap = ui::snapshotFrom(input, app->device());

  // Render on demand, not every loop: a full repaint costs real CPU time (the
  // keyboard is ~40 per-pixel rounded fills), and rendering unconditionally
  // starves the touch poll — keystrokes land while the CPU is busy repainting
  // an unchanged frame. Idle loops just poll input at full cadence.
  const bool inputActive = snap.touchPressed || snap.touchReleased || snap.confirm ||
                           snap.back || snap.focusNext || snap.focusPrev ||
                           snap.prev || snap.next;

  // Keystroke coalescing: a panel refresh blocks ~half a second and any tap
  // completed inside it is lost, so refreshing per keystroke drops letters
  // under fast typing. While text-entry actions keep arriving, keep capturing
  // taps and hold the refresh; push one refresh when the typing pauses.
  static uint32_t typeHoldUntil = 0;
  const bool holdActive = typeHoldUntil != 0 && millis() < typeHoldUntil;

  if (inputActive || (app->invalidated() && !holdActive)) {
    app->render(snap);

    const ui::ActionEvent ev = app->lastEvent();
    const bool kbScreen = state.screen == ScreenNew || state.screen == ScreenWifiPass;
    if (ev && kbScreen && (ev.action == ActionKey || ev.action == ActionBackspace ||
                           ev.action == ActionShift || ev.action == ActionMode)) {
      typeHoldUntil = millis() + 250;   // batch this burst into one refresh
    } else if (ev) {
      typeHoldUntil = 0;                // any other action refreshes immediately
    }

    if (typeHoldUntil == 0 || millis() >= typeHoldUntil) {
      typeHoldUntil = 0;
      ui::present(display, app->lastRenderRefreshHint());

      // A frame was just pushed; if it was the "Scanning..." screen, run the
      // blocking Wi-Fi scan now that the user can see why we're busy.
      if (state.scanPending && state.screen == ScreenWifiScan &&
          app->lastRenderRefreshHint() != ui::RefreshHint::None) {
        state.scanPending = false;
        scanNets(state);
        app->invalidate(ui::RefreshHint::Fast);
      }
    }
  }
  delay(10);
}
