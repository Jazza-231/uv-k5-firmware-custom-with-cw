#include "morse.h"

#include "driver/bk4819.h"

#include "driver/system.h"

#include "radio.h"
#include "app/app.h"
#include <stdbool.h>
#include <string.h>
#include "driver/st7565.h"
#include "external/printf/printf.h"

#include "misc.h"
#include "ui/helper.h"
#include "ui/ui.h"
#include "ui/inputbox.h"
#include "ui/morse.h"
#include "app/generic.h"
#include "functions.h"
#include "settings.h"
#include "driver/uart.h"
#include "app/chFrScanner.h"

int txstatus =0;
bool txen = false;
char cwid1_m[MORSE_CWID_PART_LEN + 1] = MORSE_CWID_DEFAULT;
char cwid2_m[MORSE_CWID_PART_LEN + 1] = "";

static bool gMorseRemoteActive = false;
static uint16_t gMorseRemoteCountdown_10ms = 0;
static bool gMorseDtmfOverride = false;
static bool gMorsePrevDtmfDecode = false;
static bool gMorseExitPrimed = false;
static bool gMorseExitConsume = false;

static void TransmitMorse(const char *text);
static uint16_t MORSE_GetStopIntervalMs(void);
static void MORSE_SetDtmfOverride(bool enable)
{
    if (enable && !gMorseDtmfOverride) {
        gMorsePrevDtmfDecode = gRxVfo->DTMF_DECODING_ENABLE;
        gRxVfo->DTMF_DECODING_ENABLE = true;
        gMorseDtmfOverride = true;
        gSetting_live_DTMF_decoder = true;  // Enable live DTMF display for debugging
    } else if (!enable && gMorseDtmfOverride) {
        gRxVfo->DTMF_DECODING_ENABLE = gMorsePrevDtmfDecode;
        gMorseDtmfOverride = false;
        gSetting_live_DTMF_decoder = false; // Disable live DTMF display when exiting
    }
}

void MORSE_UpdateDtmfOverride(void)
{
    // Always keep DTMF decoding enabled in MORSE mode so we can receive remote activation commands
    MORSE_SetDtmfOverride(true);
}

void MORSE_ClearDtmfOverride(void)
{
    MORSE_SetDtmfOverride(false);
}

void MORSE_ResetExitPrime(void)
{
    gMorseExitPrimed = false;
    gMorseExitConsume = false;
}

static uint16_t MORSE_GetStopIntervalMs(void)
{
    uint16_t interval = morse_stop_interval_ms;
    if (interval < MORSE_STOP_INTERVAL_MIN_MS)
        interval = MORSE_STOP_INTERVAL_MIN_MS;
    if (interval > MORSE_STOP_INTERVAL_MAX_MS)
        interval = MORSE_STOP_INTERVAL_MAX_MS;
    return interval;
}

uint16_t MORSE_GetWaitCountdown10ms(void)
{
    if (txstatus != 3)
        return 0;
    if (gCustomCountdown_10ms > 0)
        return gCustomCountdown_10ms;
    if (gMorseRemoteActive && gMorseRemoteCountdown_10ms > 0)
        return gMorseRemoteCountdown_10ms;
    return (MORSE_GetStopIntervalMs() + 9u) / 10u;
}

static size_t MORSE_Strnlen(const char *s, size_t max_len)
{
    size_t len = 0;
    while (len < max_len && s[len] != 0)
        len++;
    return len;
}

const char *MORSE_GetCombinedCwid(void)
{
    static char combined[MORSE_CWID_TOTAL_LEN + 1];
    size_t len1;
    size_t len2;

    len1 = MORSE_Strnlen(cwid1_m, MORSE_CWID_PART_LEN);
    len2 = MORSE_Strnlen(cwid2_m, MORSE_CWID_PART_LEN);

    while (len1 > 0 && cwid1_m[len1 - 1] == ' ')
        len1--;
    while (len2 > 0 && cwid2_m[len2 - 1] == ' ')
        len2--;

    memcpy(combined, cwid1_m, len1);
    memcpy(combined + len1, cwid2_m, len2);
    combined[len1 + len2] = 0;

    if (combined[0] == 0) {
        strncpy(combined, MORSE_CWID_DEFAULT, MORSE_CWID_TOTAL_LEN);
        combined[MORSE_CWID_TOTAL_LEN] = 0;
    }

    return combined;
}

char* morseVersion = "1.2.0";
uint8_t morse_wpm = MORSE_WPM_DEFAULT;
uint8_t morse_wpm_effective = MORSE_EFF_WPM_DEFAULT;
uint16_t morse_stop_interval_ms = MORSE_STOP_INTERVAL_DEFAULT_MS;
uint16_t morse_beep_num = MORSE_BEEP_COUNT_DEFAULT;
uint16_t morse_tone_hz = MORSE_TONE_HZ_DEFAULT;

#define MORSE_PARIS_WORD_UNITS    50u
#define MORSE_PARIS_ELEMENT_UNITS 31u
#define MORSE_PARIS_GAP_UNITS     19u

static uint16_t MORSE_ScaleToneHz(uint16_t freq)
{
    return (((uint32_t)freq * 1353245u) + (1u << 16)) >> 17;
}

static void MORSE_PrepareToneNoBeep(uint16_t freq)
{
    BK4819_EnterTxMute();
    BK4819_WriteRegister(BK4819_REG_70,
        BK4819_REG_70_MASK_ENABLE_TONE1 |
        (66u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
    BK4819_WriteRegister(BK4819_REG_71, MORSE_ScaleToneHz(freq));
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_EnableTXLink();
}

static uint16_t MORSE_GetDotMs(void)
{
    uint8_t wpm = morse_wpm;

    if (wpm < MORSE_WPM_MIN || wpm > MORSE_WPM_MAX)
        wpm = MORSE_WPM_DEFAULT;
    if (wpm == 0)
        wpm = MORSE_WPM_DEFAULT;

    {
        const uint32_t dot_ticks = (120u + (wpm / 2u)) / wpm;
        return (uint16_t)(dot_ticks * 10u);
    }
}

static uint16_t MORSE_GetGapUnitMs(void)
{
    const uint16_t dot_ms = MORSE_GetDotMs();
    const uint32_t dot_ticks = dot_ms / 10u;
    uint8_t char_wpm = morse_wpm;
    uint8_t eff_wpm = morse_wpm_effective;

    if (char_wpm < MORSE_WPM_MIN || char_wpm > MORSE_WPM_MAX)
        char_wpm = MORSE_WPM_DEFAULT;
    if (eff_wpm < MORSE_EFF_WPM_MIN || eff_wpm > MORSE_EFF_WPM_MAX)
        eff_wpm = MORSE_EFF_WPM_DEFAULT;
    if (eff_wpm >= char_wpm || eff_wpm == 0)
        return dot_ms;

    {
        const uint32_t total_word_ticks =
            ((120u * MORSE_PARIS_WORD_UNITS) + (eff_wpm / 2u)) / eff_wpm;
        const uint32_t element_ticks = dot_ticks * MORSE_PARIS_ELEMENT_UNITS;
        if (total_word_ticks <= element_ticks)
            return dot_ms;

        {
            const uint32_t gap_total_ticks = total_word_ticks - element_ticks;
            uint32_t gap_unit_ticks = (gap_total_ticks + (MORSE_PARIS_GAP_UNITS / 2u)) / MORSE_PARIS_GAP_UNITS;
            if (gap_unit_ticks < dot_ticks)
                gap_unit_ticks = dot_ticks;
            if (gap_unit_ticks > 0xFFFFu)
                gap_unit_ticks = 0xFFFFu;
            return (uint16_t)(gap_unit_ticks * 10u);
        }
    }
}

void morseDelay(uint16_t tms){
        uint16_t last_tenths = 0xFFFFu;
        const bool show_countdown =
            (txstatus == 3) ||
            ((txstatus == 2) && (tms >= 1000u));

        if (tms == 0) {
            gCustomCountdown_10ms = 0;
            gCustomTimeoutReached = true;
            return;
        }

        gCustomCountdown_10ms     = (tms + 9u) / 10u;
        gCustomTimeoutReached = false;
        if (show_countdown) {
            last_tenths = (gCustomCountdown_10ms + 9u) / 10u;
            UI_DisplayMORSE();
        }
        while (!gCustomTimeoutReached) {
            if (txstatus == 3) {
                APP_TimeSlice10ms();
            }
            if (KEYBOARD_Poll() == KEY_EXIT) {
                txstatus = 0;
                txen = false;
                gMorseExitPrimed = true;
                gMorseExitConsume = true;
                UI_DisplayMORSE();
                return;
            }
            if(txstatus==0){
                return;
            }
            if (show_countdown) {
                const uint16_t tenths_left = (gCustomCountdown_10ms + 9u) / 10u;
                if (tenths_left != last_tenths) {
                    last_tenths = tenths_left;
                    UI_DisplayMORSE();
                }
            }
        }
}

static bool MORSE_StartTx(void)
{
    RADIO_PrepareTX();
    return gCurrentFunction == FUNCTION_TRANSMIT;
}

static void MORSE_StopTx(void)
{
    if (gCurrentFunction != FUNCTION_TRANSMIT)
        return;

    if (!gFlagEndTransmission) {
        APP_EndTransmission();

        if (gEeprom.REPEATER_TAIL_TONE_ELIMINATION > 0)
            morseDelay((uint16_t)gEeprom.REPEATER_TAIL_TONE_ELIMINATION * 100);
    }

    FUNCTION_Select(FUNCTION_FOREGROUND);
    gFlagEndTransmission = false;
#ifdef ENABLE_VOX
    gVOX_NoiseDetected = false;
#endif
    RADIO_SetVfoState(VFO_STATE_NORMAL);

    if (gScreenToDisplay == DISPLAY_MORSE) {
        // Restore RX path after MORSE TX so audio/DTMF use the RX VFO.
        RADIO_SetupRegisters(false);
    }
}

void MORSE_RemoteActivate(void)
{
    gMorseRemoteActive = true;
    if (gMorseRemoteCountdown_10ms == 0)
        gMorseRemoteCountdown_10ms = 1;
}

void MORSE_RemoteDeactivate(void)
{
    gMorseRemoteActive = false;
    gMorseRemoteCountdown_10ms = 0;
    txstatus = 0;
    txen = false;
    if (gCurrentFunction == FUNCTION_TRANSMIT)
        MORSE_StopTx();
}

bool MORSE_RemoteIsActive(void)
{
    return gMorseRemoteActive;
}

void MORSE_RemoteTimeSlice10ms(void)
{
    if (!gMorseRemoteActive)
        return;

    // If exit was pressed during remote transmission, stop and deactivate
    if (gMorseExitPrimed) {
        MORSE_RemoteDeactivate();
        return;
    }

    if (gCurrentFunction == FUNCTION_TRANSMIT)
        return;

    if (gCurrentFunction != FUNCTION_FOREGROUND || g_SquelchLost)
        return;

    if (gScanStateDir != SCAN_OFF || gCssBackgroundScan)
        return;

#ifdef ENABLE_DTMF_CALLING
    if (gSetting_KILLED)
        return;
#endif

    if (gMorseRemoteCountdown_10ms > 0) {
        gMorseRemoteCountdown_10ms--;
        return;
    }

    RADIO_ApplyOffset(gTxVfo);
    if (!MORSE_StartTx()) {
        gMorseRemoteCountdown_10ms = 50;
        return;
    }

    TransmitMorse(MORSE_GetCombinedCwid());
    MORSE_StopTx();

    if (!gMorseRemoteActive)
        return;

    {
        uint32_t delay = MORSE_GetStopIntervalMs() / 10u;
        if (delay == 0)
            delay = 1u;
        if (delay > 0xFFFFu)
            delay = 0xFFFFu;
        gMorseRemoteCountdown_10ms = (uint16_t)delay;
    }
}

void PlayMorseTone(const char *morse) {
    const uint16_t dot_ms = MORSE_GetDotMs();
    const uint16_t dash_ms = (uint16_t)(dot_ms * 3u);
    const uint16_t element_gap_ms = dot_ms;

    while (*morse) {
        if (txstatus == 0)
            break;

        if (*morse == '.') {
            BK4819_ExitTxMute();
            BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);
            morseDelay(dot_ms);
            BK4819_EnterTxMute();
        } else if (*morse == '-') {
            BK4819_ExitTxMute();
            BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);
            morseDelay(dash_ms);
            BK4819_EnterTxMute();
        } else {
            morse++;
            continue;
        }

        BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);
        if (txstatus == 0)
            break;

        morse++;
        if (*morse)
            morseDelay(element_gap_ms);
    }
       
}

// Function to get Morse code for a character
const char* MorseCode(char c) {
    switch (toupper(c)) {
        case 'A': return ".-";
        case 'B': return "-...";
        case 'C': return "-.-.";
        case 'D': return "-..";
        case 'E': return ".";
        case 'F': return "..-.";
        case 'G': return "--.";
        case 'H': return "....";
        case 'I': return "..";
        case 'J': return ".---";
        case 'K': return "-.-";
        case 'L': return ".-..";
        case 'M': return "--";
        case 'N': return "-.";
        case 'O': return "---";
        case 'P': return ".--.";
        case 'Q': return "--.-";
        case 'R': return ".-.";
        case 'S': return "...";
        case 'T': return "-";
        case 'U': return "..-";
        case 'V': return "...-";
        case 'W': return ".--";
        case 'X': return "-..-";
        case 'Y': return "-.--";
        case 'Z': return "--..";
        case '1': return ".----";
        case '2': return "..---";
        case '3': return "...--";
        case '4': return "....-";
        case '5': return ".....";
        case '6': return "-....";
        case '7': return "--...";
        case '8': return "---..";
        case '9': return "----.";
        case '0': return "-----";
        case '/': return "-..-."; // Slash (/)
        case '-': return "-....-"; // Dash (-)
        default: return ""; // Ignore unknown characters
    }
}

// Function to transmit Morse code from a text string
void TransmitMorse(const char *text) {
        const uint16_t gap_unit_ms = MORSE_GetGapUnitMs();
        const uint16_t letter_gap_ms_u16 = (uint16_t)(gap_unit_ms * 3u);
        const uint16_t word_gap_ms_u16 = (uint16_t)(gap_unit_ms * 7u);
        
        const uint16_t beep_count = morse_beep_num;
        const uint16_t dot_ms = MORSE_GetDotMs();

        if (beep_count > 0u) {
            txstatus = 2;
            UI_DisplayMORSE();

            for (uint16_t i = 0; i < beep_count; i++) {
                BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, true);
                BK4819_TransmitTone(false, morse_tone_hz);
                morseDelay(dot_ms * 3u);
                BK4819_EnterTxMute();
                BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);
                if (txstatus == 0) {
                    UI_DisplayMORSE();
                    return;
                }
                if ((i + 1u) < beep_count) {
                    morseDelay(dot_ms * 5u);
                    if (txstatus == 0) {
                        UI_DisplayMORSE();
                        return;
                    }
                }
            }

            morseDelay(letter_gap_ms_u16);
            if (txstatus == 0) {
                UI_DisplayMORSE();
                return;
            }
        } else {
            // Configure the tone generator without the BK4819_TransmitTone unmute delay.
            txstatus=1;
            UI_DisplayMORSE();
            MORSE_PrepareToneNoBeep(morse_tone_hz);
        }
        if (beep_count > 0u) {
            txstatus=1;
            UI_DisplayMORSE();
        }
        while (*text) {
            if (txstatus == 0)
                break;

            if (*text == ' ') {
                morseDelay(word_gap_ms_u16);
                text++;
                continue;
            }

            const char *morse = MorseCode(*text);
            if (morse[0] != '\0') {
                PlayMorseTone(morse);
                if (txstatus == 0)
                    break;
            }

            if (text[1] != '\0' && text[1] != ' ')
                morseDelay(letter_gap_ms_u16);
            text++;
        }
        
        if (txstatus != 0)
            txstatus =3;

        UI_DisplayMORSE();
        BK4819_ToggleGpioOut(BK4819_GPIO5_PIN1_RED, false);
        
}


static void MORSE_Key_MENU(bool bKeyPressed, bool bKeyHeld) {
    if (!bKeyHeld && bKeyPressed) {
        // Ignore MENU while waiting or otherwise busy (prevents re-starting TX mid-cycle).
        if (txstatus != 0 || txen || gCurrentFunction == FUNCTION_TRANSMIT || gMorseRemoteActive) {
            return;
        }
        gMorseExitPrimed = false;
        gMorseExitConsume = false;
        txen=true;
        while(txen){
            // Check if exit was requested
            if (gMorseExitPrimed) {
                txen = false;
                break;
            }
            
            if (!MORSE_StartTx()) {
                txstatus = 0;
                UI_DisplayMORSE();
                txen = false;
                break;
            }
            TransmitMorse(MORSE_GetCombinedCwid());
            MORSE_StopTx();
            
            // Check if exit was requested during transmission
            if (gMorseExitPrimed) {
                txen = false;
                break;
            }
            
            morseDelay(MORSE_GetStopIntervalMs());
        }
    }
}
static void MORSE_Key_EXIT(bool bKeyPressed, bool bKeyHeld)
{
    if (!bKeyPressed) {
        // Key release clears any pending consume from the TX abort press.
        if (gMorseExitConsume)
            gMorseExitConsume = false;
        return;
    }

    if (bKeyHeld)
        return;

    if (gMorseExitConsume) {
        gMorseExitConsume = false;
        return;
    }

    // If currently transmitting or waiting, stop TX and stay in MORSE.
    if (gCurrentFunction == FUNCTION_TRANSMIT || txstatus != 0 || txen || gMorseRemoteActive) {
        if (gCurrentFunction == FUNCTION_TRANSMIT)
            MORSE_StopTx();
        if (gMorseRemoteActive)
            MORSE_RemoteDeactivate();
        txstatus = 0;
        txen = false;
        gMorseExitPrimed = true;
        UI_DisplayMORSE();
        return;
    }

    // Idle: exit immediately on a single press.
    gMorseExitPrimed = false;
    MORSE_ClearDtmfOverride();
    UI_MORSE_ClearRxSetup();
    gRequestDisplayScreen = DISPLAY_MAIN;
}
static void MORSE_Key_UP_DOWN(bool bKeyPressed, bool pKeyHeld, int8_t Direction)
{
    const bool waiting = (txstatus == 3);
    if (!waiting && (txstatus != 0 || txen))
        return;
    if (!pKeyHeld && bKeyPressed && Direction==1) {
        gTxVfo->OUTPUT_POWER++;
    } else if(!pKeyHeld && bKeyPressed && Direction==-1){
        gTxVfo->OUTPUT_POWER--;
    }

    if(gTxVfo->OUTPUT_POWER==8){
        
        gTxVfo->OUTPUT_POWER=1;
    }
    else if(gTxVfo->OUTPUT_POWER==0){
        
        gTxVfo->OUTPUT_POWER=7;
    }
    UI_DisplayMORSE();
    morseDelay(10);
}
void MORSE_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    switch (Key) {
        case KEY_MENU:
            MORSE_Key_MENU(bKeyPressed, bKeyHeld);
            break;
        case KEY_EXIT:
            MORSE_Key_EXIT(bKeyPressed, bKeyHeld);
            break;
        case KEY_1:
            if (!bKeyHeld && bKeyPressed) {
                UI_MORSE_PrevPage();
                UI_DisplayMORSE();
            }
            break;
        case KEY_4:
            if (!bKeyHeld && bKeyPressed) {
                UI_MORSE_NextPage();
                UI_DisplayMORSE();
            }
            break;
        case KEY_UP:
            MORSE_Key_UP_DOWN(bKeyPressed, bKeyHeld,  1);
            break;
        case KEY_DOWN:
            MORSE_Key_UP_DOWN(bKeyPressed, bKeyHeld, -1);
            break;
        default:
            if (!bKeyHeld && bKeyPressed)
            break;
    }
}
