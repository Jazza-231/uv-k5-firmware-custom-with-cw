#ifndef UI_MORSE_H
#define UI_MORSE_H

#include <stdint.h>

void UI_MORSE_Init(void);
void UI_DisplayMORSE(void);
void UI_DisplayMORSEStatus(void);
void UI_MORSE_NextPage(void);
void UI_MORSE_PrevPage(void);
uint8_t UI_MORSE_GetPage(void);
void UI_MORSE_ClearRxSetup(void);
extern bool canStart;
#endif


