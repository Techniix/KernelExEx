/*
 *  KernelExEx - how the Wi-Fi drivers and the Wi-Fi program (kexwifi.exe)
 *  talk: values under one registry key. Shared by both sides.
 *  Copyright (C) 2026, KernelExEx contributors. GPLv2.
 *
 *  HKLM\Software\KernelEx\WiFi
 *    Networks\<SSID in hex>        saved networks, written by kexwifi.exe
 *      SSID    REG_BINARY  the network name (1-32 bytes)
 *      Key     REG_SZ      the WPA2 key as 64 hex digits (the PSK, derived
 *                          from the password with PBKDF2 by kexwifi.exe, so
 *                          the password itself is never stored); "" = open
 *      TKIP    REG_DWORD   1 = the user allowed older WPA (TKIP) for it
 *      Auto    REG_DWORD   0 = join only when asked ("Connect automatically"
 *                          off); missing or 1 = join by itself when in range
 *    Adapters\<interface>          one per Wi-Fi adapter (urtwn0, rtwn0...)
 *      Name     REG_SZ     adapter description          (driver writes)
 *      MAC      REG_BINARY the adapter's own address (6 bytes) (driver)
 *      Present  REG_DWORD  1 while the driver runs       (driver)
 *      State    REG_DWORD  KEXWIFI_* below               (driver)
 *      SSID     REG_BINARY network joined or being joined (driver)
 *      BSSID    REG_BINARY 6 bytes                       (driver)
 *      Channel  REG_DWORD                                (driver)
 *      Signal   REG_DWORD  0-100                         (driver)
 *      Scan     REG_BINARY array of struct kexwifi_ap    (driver, on request)
 *      ScanSeq  REG_DWORD  bumped each time Scan is written (driver)
 *      Fixed    REG_DWORD  1 = a network is set in the adapter's Advanced
 *                          properties (the old way); it is always used
 *      Request  REG_DWORD  KEXWIFI_REQ_* (kexwifi.exe writes, the driver
 *                          sets it back to 0 when it has acted on it)
 *      Target   REG_BINARY SSID for KEXWIFI_REQ_CONNECT  (kexwifi.exe)
 *
 *  The driver looks at Request every 2 seconds and writes its values only
 *  when they change (Signal in steps of 20), so the registry is not
 *  rewritten all the time.
 */
#ifndef KEXWIFI_REG_H
#define KEXWIFI_REG_H

#define KEXWIFI_KEY_A "Software\\KernelEx\\WiFi"

#define KEXWIFI_OFF 0          /* turned off (Disconnect) or not running */
#define KEXWIFI_SEARCHING 1    /* looking for a saved network */
#define KEXWIFI_CONNECTING 2   /* joining, or WPA2 handshake under way */
#define KEXWIFI_CONNECTED 3

#define KEXWIFI_REQ_NONE 0
#define KEXWIFI_REQ_RELOAD 1      /* saved networks changed: join the best one */
#define KEXWIFI_REQ_CONNECT 2     /* join Target (a saved network) now */
#define KEXWIFI_REQ_OFF 3         /* disconnect and stay off */
#define KEXWIFI_REQ_ON 4          /* back on, join saved networks */
#define KEXWIFI_REQ_SCAN 5        /* write Scan */

#define KEXWIFI_SEC_OPEN 0
#define KEXWIFI_SEC_WEP 1      /* listed, never joined: WEP is broken */
#define KEXWIFI_SEC_WPA2 2
#define KEXWIFI_SEC_WPA 3      /* WPA1/TKIP only */
#define KEXWIFI_SEC_WPA3 4     /* SAE only: not supported yet */
#define KEXWIFI_SEC_ENT 5      /* 802.1X (enterprise): not supported yet */

#pragma pack(push, 1)
struct kexwifi_ap
{
	unsigned char ssid_len;
	unsigned char ssid[32];
	unsigned char bssid[6];
	unsigned char channel;
	unsigned char signal;     /* 0-100 */
	unsigned char security;   /* KEXWIFI_SEC_* */
	unsigned char saved;      /* 1 = in Networks */
	unsigned char reserved;
};
#pragma pack(pop)

#define KEXWIFI_MAX_AP 48

#endif
