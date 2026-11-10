#pragma once
// Whether Zwift is in a ride or in its menus, read from Zwift's own log
// (%LOCALAPPDATA%\Zwift\Logs\Log.txt, recreated on each Zwift launch):
//   [GameState] ZSF_INITIAL_CHALLENGE_SELECT -> ZSF_RIDE   ride started (world on screen)
//   [GameState] ZSF_X -> ZSF_Y (Y != ZSF_RIDE)            a menu screen (login, pairing, drop-in, ...)
//   [GameState] Ending Ride / EndGameSession              ride over
// ("Starting Ride" is ignored: it comes ~25 s early, when the drop-in starts loading the world.)
// Seen on 2026-11-09 (Quest 3 laptop): TOKEN_LOGIN -> EULA -> TRAINING_PLAN -> POST_LOGIN_INIT ->
// PAIRING_SCREEN -> DROPIN_SCREEN -> INITIAL_CHALLENGE_SELECT -> RIDE. In-ride menus (Esc menu, mid-ride
// pairing) don't log a ZSF change, so they stay in whatever mode the ride is in.
#include <cstdint>

// Starts the background reader thread (once).
void ZwiftStateStart();

// -1 = unknown yet, 0 = menus, 1 = riding.
int ZwiftStateRiding();

// Increments on every change of ZwiftStateRiding(); lets the render thread act on changes only, so a manual
// Ctrl+Shift+F10 stays in effect until Zwift's state changes again.
uint32_t ZwiftStateSeq();
