// The panel's own 128x64 OLED, on the UART2 connector (GPIO 43/44). See
// src/paneloled.cpp for why those pins and what they cost.
//
// Enabled with -DPANEL_OLED=1. Off, this compiles to nothing, so the console-owning build
// and the display-owning build differ by one flag.
#pragma once

#ifndef PANEL_OLED
#define PANEL_OLED 0
#endif

#if PANEL_OLED
// Releases Serial (UART0 owns the same pads), claims Wire1, and looks for the display. If
// nothing answers it hands the pins back and restores the console - so a board whose switch
// still routes 43/44 to the USB bridge keeps its logging and loses nothing.
bool paneloled_init(void);
bool paneloled_ready(void);

// The 7-bit address that answered, or 0 if no display was found at boot. Reported on the
// settings page, because with these pads given to I2C there is no console to print to - and
// "did it find the display" is exactly the question that needs answering then.
uint8_t paneloled_address(void);


// Redraws once a second. Does nothing at all if no display was found at boot: the pins went
// back to the console then and must not be taken again mid-run. A display that WAS found and
// has stopped answering gets the init sequence re-sent, with the bus left alone.
void paneloled_tick(void);
#endif
