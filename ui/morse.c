#include <stdbool.h>
#include <string.h>
#include "app/morse.h"
#include "app/dtmf.h"
#include "driver/st7565.h"
#include "external/printf/printf.h"
#include "helper/battery.h"
#include "misc.h"
#include "settings.h"
#include "ui/helper.h"
#include "ui/morse.h"
#include "app/morse.h"
#include "radio.h"
#include "ui/ui.h"
#include "driver/bk4819.h"
#include "bitmaps.h"

static uint8_t gMorsePage = 0;
static bool gMorseRxSetup = false;
static bool gMorsePrevRxValid = false;
static uint8_t gMorsePrevRxVfo = 0u;
static uint8_t gMorsePrevCrossBand = CROSS_BAND_OFF;
static uint8_t gMorsePrevDualWatch = DUAL_WATCH_OFF;

void UI_MORSE_Init(void)
{
    // Initialize MORSE mode - set up RX path with audio on first entry
    if (!gMorseRxSetup) {
        gMorsePrevRxVfo = gEeprom.RX_VFO;
        gMorsePrevCrossBand = gEeprom.CROSS_BAND_RX_TX;
        gMorsePrevDualWatch = gEeprom.DUAL_WATCH;
        gMorsePrevRxValid = true;

        // Force RX on the opposite VFO while keeping TX on the selected VFO.
        gEeprom.DUAL_WATCH = DUAL_WATCH_OFF;
        gEeprom.CROSS_BAND_RX_TX = (uint8_t)(gEeprom.TX_VFO + 1u);
        gEeprom.RX_VFO = (gEeprom.TX_VFO == 0u) ? 1u : 0u;
        gTxVfo = &gEeprom.VfoInfo[gEeprom.TX_VFO];
        gRxVfo = &gEeprom.VfoInfo[gEeprom.RX_VFO];
        gCurrentVfo = (gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF || gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)
            ? gRxVfo
            : gTxVfo;

        MORSE_ResetExitPrime();
        RADIO_SetupRegisters(true);
        gMorseRxSetup = true;
    }
}

uint8_t UI_MORSE_GetPage(void)
{
    return gMorsePage;
}

void UI_MORSE_NextPage(void)
{
    gMorsePage = (gMorsePage + 1u) % 2u;
}

void UI_MORSE_PrevPage(void)
{
    gMorsePage = (gMorsePage == 0u) ? 1u : 0u;
}

void UI_DisplayMORSEStatus(void)
{
    char String[32] = {0};
    const int percent = (int)BATTERY_VoltsToPercent(gBatteryVoltageAverage);

    gUpdateStatus = false;
    memset(gStatusLine, 0, sizeof(gStatusLine));

    snprintf_(String, sizeof(String), "FOX TX v%s", morseVersion);
    GUI_DisplaySmallest(String, 0, 1, true, true);

    gStatusLine[116] = 0b00011100;
    gStatusLine[117] = 0b00111110;
    for (int i = 118; i <= 126; i++)
    {
        gStatusLine[i] = 0b00100010;
    }

    for (int i = 127; i >= 118; i--)
    {
        const int filled = (int)((percent + 5u) * 9u / 100u);
        if ((127 - i) <= filled)
        {
            gStatusLine[i] = 0b00111110;
        }
    }

    ST7565_BlitStatusLine();
}

void UI_DisplayMORSE(void)
{

    
    char  String[64] = {0};

    gScreenToDisplay = DISPLAY_MORSE;
    {
        const uint8_t expected_rx = (gEeprom.TX_VFO == 0u) ? 1u : 0u;
        const uint8_t expected_cb = (uint8_t)(gEeprom.TX_VFO + 1u);
        bool needs_reapply = false;

        if (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF) {
            gEeprom.DUAL_WATCH = DUAL_WATCH_OFF;
            needs_reapply = true;
        }

        if (gEeprom.CROSS_BAND_RX_TX != expected_cb) {
            gEeprom.CROSS_BAND_RX_TX = expected_cb;
            needs_reapply = true;
        }

        if (gEeprom.RX_VFO != expected_rx || gRxVfo != &gEeprom.VfoInfo[expected_rx]) {
            gEeprom.RX_VFO = expected_rx;
            gRxVfo = &gEeprom.VfoInfo[expected_rx];
            needs_reapply = true;
        }

        if (needs_reapply) {
            gTxVfo = &gEeprom.VfoInfo[gEeprom.TX_VFO];
            gCurrentVfo = (gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF || gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)
                ? gRxVfo
                : gTxVfo;
            RADIO_SetupRegisters(false);
        }
    }
    MORSE_UpdateDtmfOverride();
    UI_DisplayClear();

    if (gMorsePage == 0u) {
        const char *cwid1 = cwid1_m[0] ? cwid1_m : "--";
        const char *cwid2 = cwid2_m[0] ? cwid2_m : "--";
        snprintf_(String, sizeof(String), "CW1: %s", cwid1);
        UI_PrintStringSmallBold(String, 0, 0, 0);
        snprintf_(String, sizeof(String), "CW2: %s", cwid2);
        UI_PrintStringSmallBold(String, 0, 0, 1);

        if (morse_wpm_effective != morse_wpm)
            snprintf_(String, sizeof(String), "WPM: %u/%u", morse_wpm, morse_wpm_effective);
        else
            snprintf_(String, sizeof(String), "WPM: %u", morse_wpm);
        UI_PrintStringSmallBold(String, 0, 0, 2);

        // Debug: Display received DTMF
        const char *dtmf_rx = gDTMF_RX_live[0] ? gDTMF_RX_live : "--";
        snprintf_(String, sizeof(String), "DTMF RX: %s", dtmf_rx);
        UI_PrintStringSmallBold(String, 0, 0, 3);
    } else {
        VFO_Info_t *tx_vfo = gTxVfo;
        VFO_Info_t *rx_vfo = &gEeprom.VfoInfo[gEeprom.TX_VFO ? 0u : 1u];
        const uint32_t tx_freq = tx_vfo->pTX->Frequency;
        const uint32_t rx_freq = rx_vfo->pRX->Frequency;
        const char *dtmf_on = gEeprom.DTMF_CW_ON_CODE[0] ? gEeprom.DTMF_CW_ON_CODE : "--";
        const char *dtmf_off = gEeprom.DTMF_CW_OFF_CODE[0] ? gEeprom.DTMF_CW_OFF_CODE : "--";

        snprintf_(String, sizeof(String), "TX: %u.%05u", tx_freq / 100000u, tx_freq % 100000u);
        UI_PrintStringSmallBold(String, 0, 0, 0);
        snprintf_(String, sizeof(String), "RX: %u.%05u", rx_freq / 100000u, rx_freq % 100000u);
        UI_PrintStringSmallBold(String, 0, 0, 1);
        snprintf_(String, sizeof(String), "ON DTMF: %s", dtmf_on);
        UI_PrintStringSmallBold(String, 0, 0, 2);
        snprintf_(String, sizeof(String), "OFF DTMF: %s", dtmf_off);
        UI_PrintStringSmallBold(String, 0, 0, 3);

    }

    {
        const char *pw = "?";
        const int power = gTxVfo->OUTPUT_POWER;
        if (power == 1) {
            pw = "20mW";
        } else if (power == 2) {
            pw = "125mW";
        } else if (power == 3) {
            pw = "250mW";
        } else if (power == 4) {
            pw = "500mW";
        } else if (power == 5) {
            pw = "1W";
        } else if (power == 6) {
            pw = "2W";
        } else if (power == 7) {
            pw = "5W";
        }
        snprintf_(String, sizeof(String), "PW: %s", pw);
        UI_PrintStringSmallBold(String, 0, 0, 5);
    }

    {
        uint16_t tenths_left = 0;
        if (txstatus == 2)
            tenths_left = (gCustomCountdown_10ms + 9u) / 10u;
        else if (txstatus == 3)
            tenths_left = (MORSE_GetWaitCountdown10ms() + 9u) / 10u;

        if (txstatus == 1) {
            UI_PrintStringSmallBold("STA:TX CWID", 0, 0, 6);
        } else if (txstatus == 2) {
            if (tenths_left > 0) {
                const uint16_t seconds = tenths_left / 10u;
                const uint16_t tenths = tenths_left % 10u;
                snprintf_(String, sizeof(String), "STA:TONE %u.%us", seconds, tenths);
            } else {
                snprintf_(String, sizeof(String), "STA:TONE 0.0s");
            }
            UI_PrintStringSmallBold(String, 0, 0, 6);
        } else if (txstatus == 3) {
            if (tenths_left > 0) {
                const uint16_t seconds = tenths_left / 10u;
                const uint16_t tenths = tenths_left % 10u;
                snprintf_(String, sizeof(String), "STA:WAIT %u.%us", seconds, tenths);
            } else {
                snprintf_(String, sizeof(String), "STA:WAIT 0.0s");
            }
            UI_PrintStringSmallBold(String, 0, 0, 6);
        } else {
            UI_PrintStringSmallBold("STA:QRV", 0, 0, 6);
        }
    }

    {
        const char *page_label = (gMorsePage == 0u) ? "1/2" : "2/2";
        const size_t label_len = strlen(page_label);
        const int label_x = (int)LCD_WIDTH - (int)(label_len * 4u) - 2;
        GUI_DisplaySmallest(page_label, (uint8_t)label_x, 49, false, true);
    }
    ST7565_BlitFullScreen();
    UI_DisplayMORSEStatus();

    
}

void UI_MORSE_ClearRxSetup(void)
{
    if (gMorseRxSetup && gMorsePrevRxValid) {
        gEeprom.CROSS_BAND_RX_TX = gMorsePrevCrossBand;
        gEeprom.DUAL_WATCH = gMorsePrevDualWatch;
        gEeprom.RX_VFO = gMorsePrevRxVfo;
        gTxVfo = &gEeprom.VfoInfo[gEeprom.TX_VFO];
        gRxVfo = &gEeprom.VfoInfo[gEeprom.RX_VFO];
        gCurrentVfo = (gEeprom.CROSS_BAND_RX_TX == CROSS_BAND_OFF || gEeprom.DUAL_WATCH != DUAL_WATCH_OFF)
            ? gRxVfo
            : gTxVfo;
        RADIO_SetupRegisters(true);
    }

    gMorsePrevRxValid = false;
    gMorseRxSetup = false;
}

