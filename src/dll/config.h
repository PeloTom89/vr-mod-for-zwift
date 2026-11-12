#pragma once

struct Config {
  float distance = 2.0f;  // metres in front of the head
  float width = 2.6f;     // metres; height follows Zwift's aspect ratio
  float height = 0.0f;    // metres, vertical offset of the screen centre
  float cropAspect = 0.0f;  // width/height to crop Zwift's frame to (centred); 0 = no crop

  // [depth] - phase 2 stereo from Zwift's depth buffer
  bool stereo = true;
  float ipdMm = 63.0f;          // max per-eye separation = this, i.e. distant scenery looks infinitely far
  float strength = 0.5f;        // 0..2 multiplier on the separation (live: Ctrl+Shift+Up/Down)
  float convergenceM = 2.5f;    // scene distance that sits exactly on the virtual screen

  // [vr] - phase 3: immersive (headset drives Zwift's camera) vs screen (phase 1/2 virtual screen)
  bool immersive = true;
  float worldScale = 1.0f;      // multiplies 100 Zwift units (cm) per real metre
  float parallax = 1.0f;        // eye-separation scale: 1 = your real IPD, 0 = no stereo (live: Ctrl+Shift+PgUp/PgDn)
  bool recentrePosition = true; // eye origin = where your head was at start / Ctrl+Shift+F1, not the runtime's origin
  int coherentMode = 2;       // both eyes from the newest frame: 0 never, 1 while Zwift's camera moves, 2 always
  float steadyCamera = 0.1f;    // seconds of filtering that takes Zwift's camera shake out (bridges, gravel); 0 = off
  bool cropToEyeFov = true;     // submit only the part of each eye image the headset can show (Ctrl+Shift+Insert)
  int bigWindowWidth = 3840;    // Zwift window width for Ctrl+Shift+Delete (sharper eye images)
  bool autoBigWindow = true;    // big window automatically while riding, normal size in Zwift's menus
  bool autoScreenInMenus = true; // virtual screen in Zwift's menus, immersive while riding
  float eyeResolution = 1.0f;   // immersive eye images filtered down to the headset's recommended size x this; 0 = off

  // [hud] immersive: Zwift's HUD comes out of the eye images and floats as flat, world-locked cards
  bool hud = true;
  int hudCompositeDraws = 1;    // draws into the window before the HUD starts (Zwift's scene composite)
  bool hudWindowAlpha = true;   // panel backgrounds from the HUD's coverage in the window's alpha (no world in them)
  struct HudCard {
    float x0, y0, x1, y1;       // part of Zwift's window, 0..1, top-left origin
    float px, py, dist;         // card centre in metres from your head's home position: right, up, ahead
    float width;                // metres (height follows the rect's shape)
    // cardN_wide: a bigger part of the window this card shows while a full-width banner is on screen (detected by
    // Zwift drawing in the wide part left of the card's own); other cards overlapping it are cut off above it.
    bool wide = false;
    float wx0 = 0, wy0 = 0, wx1 = 0, wy1 = 0;
  };
  static constexpr int kMaxHudCards = 8;
  HudCard hudCards[kMaxHudCards] = {};
  int hudCardCount = 0;
  float hudSpread = 1.0f;       // every card's angle from straight ahead x this (live: Ctrl+Shift+[ / ])
  // [hud] banner_probe: where a full-width banner is looked for (window fractions x0 y0 x1 y1); none = the
  // cardN_wide part left of that card's own
  bool haveBannerProbe = false;
  float bannerProbe[4] = {};

  // [render]
  bool shadows = true;          // false: skip draws into shadow maps (no shadows, less CPU)
  bool reflections = true;      // false: skip the water-reflection render pass (immersive mode)
};

// Reads zwift_vr.ini from the DLL's own folder; missing keys keep their defaults.
Config LoadConfig();
