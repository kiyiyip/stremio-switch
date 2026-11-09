#pragma once

// All files the app writes live under one folder on the SD card instead of the
// root. The directory is created at startup by ensureAppDataDir() in main.cpp.
#define APPDATA_DIR      "sdmc:/switch/stremio-switch"
#define APPDATA_LOG      APPDATA_DIR "/stremio-switch.log"
#define APPDATA_CACHE    APPDATA_DIR "/cache.bin"
// Cached Stremio artwork, one .jpg per library item. Posters never change, so
// entries are kept forever and a hit never touches the network.
#define APPDATA_POSTERS  APPDATA_DIR "/posters"
// Subtitle files pulled from Stremio's subtitle addons, one per subtitle picked
// in the player. Kept so re-picking the same one costs nothing; they are a few
// tens of KB each.
#define APPDATA_SUBS     APPDATA_DIR "/subtitles"
