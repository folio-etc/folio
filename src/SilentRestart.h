#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)

// Tear down the WiFi session in place — the non-reboot alternative to
// silentRestart(). Explicit WiFi shutdown frees the driver/LWIP heap, and
// ActivityManager already drops the font-glyph caches on the navigation out, so
// the heap returns to its steady state without a restart. (Downloads now use
// wolfSSL, not mbedTLS, so the handshake no longer fragments enough to warrant
// the nuclear option.)
void teardownWifiSession();
