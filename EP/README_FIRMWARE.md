# EP Walkman firmware

STM32H743VIT6 · Waveshare 2.13" e-Paper (B) 104×212 B/W/R · 2× CS43131 · INMP441 mic · nPM1300 · 16 GB eMMC + SD card

## Build

0. In STM32CubeIDE: File → Import → General → Existing Projects into Workspace → select this `EP` folder (leave "Copy projects into workspace" unticked). The project is `walkman_set_fixed`.
1. Open `walkman_set_fixed.ioc` (your file, now with FatFs + USB MSC added; the original is in `walkman_set_fixed.ioc.bak`) and work through the checklist below.
2. Generate code. All app code is in separate files under `Core/` or inside `USER CODE` blocks, so regenerating keeps it.
3. Build and flash.

Tested build (GCC 14.3 from CubeIDE, `-O2`): **143 KB flash, 431 KB RAM_D1** (that figure includes the 32 KB stack reserve). It has **not been run on hardware yet**.

## CubeMX checklist (`walkman_set_fixed.ioc`)

Already set in the file: FatFs (SD) + USB Device MSC middleware, SDMMC "do not call", SysTick priority 0, stack/heap, I2S2 data format. The items below that CubeMX keeps as GUI-only settings still need checking before you regenerate:

| Where | Setting |
|---|---|
| RCC | SYSCLK 400 MHz, AHB 200 MHz, APBx 100 MHz. PLL2P = PLL3P = 49.152 MHz (N 98, FRACN 2490, P/Q/R 10). SAI1 ← PLL2, SPI123 ← PLL3, SDMMC ← PLL1Q (100 MHz) |
| SAI1 A | Master TX, I2S, 32-bit, **MCLK output enabled, MCK oversampling (512 fs)** |
| SAI1 B | **Synchronous slave TX** (DMA memory→peripheral) |
| I2S2 | Master RX, 48 kHz, **16 bits data on 32 bits frame** (the INMP441 needs 64 SCK per frame) |
| SPI1 | Transmit only, **8-bit**, prescaler 8 (6.1 MHz) |
| SDMMC1/2 | 4-bit, clock divider 2 (25 MHz). Project Manager → Advanced → **"Do not generate function call"** for both (`storage.c` brings them up, because the SD card is optional) |
| I2C1/I2C2 | Fast mode. CubeMX recalculates Timing for the 100 MHz kernel clock (hand value: `0x1042264B`) |
| NVIC | SysTick priority 0 (so timeouts work inside the USB interrupt) |
| FatFs | `USE_LFN` = 1, `VOLUMES` = 2, `FS_EXFAT` = 1, `FS_LOCK` = 8 |
| Project | Stack 0x8000 (minimp3 needs ~16 KB), heap 0x1000 |
| GPIO | PE12/PE13 can stay unassigned; `soft_i2c.c` sets them up as open-drain |

## Buttons (Walkman style)

| | Lists / menus | Now Playing |
|---|---|---|
| Up / Down | move | volume |
| Left | back | previous (hold = seek back) |
| Right | open | next (hold = seek forward) |
| Centre | open | play/pause · **hold = main menu** |
| Centre hold | add song to Favourites / playlist | |
| Power | click = main menu · hold = shut down (wakes on WKUP4) | |

## Storage layout

* **eMMC** (FatFs `1:`): `/Music`, `/Playlists/*.m3u`, `/System/settings.cfg`, `resume.cfg`, `queue.idx`, `library.idx`. Shows up as the USB drive (Settings → USB Drive, or plug in a cable).
* **SD card** (FatFs `0:`, optional): extra music and `/Mixtapes/MixTape NNN.wav`.

## Source map

| File | What it does |
|---|---|
| `app.c` | Boot sequence (e-ink → battery → storage → DACs → Hysteresis/resume → menu), main loop, sleep timer, standby, USB mode |
| `ui.c` | Every Figma screen: Main Menu, Files, Playlists, Now Playing, Settings, MixTapes, plus Set Time, Custom Text, About |
| `gfx.c`, `ui_assets.c` | Two-plane framebuffer and the Tiny5 font. Icons and wallpaper are generated from the Figma frames by `tools/gen_assets.py` |
| `epd.c` | UC8151D driver: full colour refresh (OTP waveform) and fast black/white partial-window refresh (register LUTs) |
| `display.c` | Display state machine: compares frames, refreshes only the changed rectangle fast, defers red to one colour pass when idle |
| `audio_out.c` | SAI A+B DMA from one shared buffer, PLL2 switching between 48 k and 44.1 k families |
| `cs43131.c` | DAC bring-up (sequences from the Linux `cs43130` driver), volume, dual-mono mapping |
| `mp3_stream.c` | minimp3 + ID3v2 (title/artist/album/APIC) + Xing seek + WAV |
| `player.c` | Queue, shuffle with history, repeat, 3 s crossfade, resume |
| `library.c` | Folder listing, `.m3u` playlists, shuffle-all index |
| `mixtape.c`, `mic_in.c` | Plays a song clip and records the mic, saves song + voice as a WAV |
| `albumart.c` | TJpgDec + Floyd–Steinberg dithering to black/white/red |
| `npm1300.c`, `soft_i2c.c` | Battery %, charging and VBUS over bit-banged I2C on PE12/PE13 |
| `storage.c` | eMMC FatFs driver, optional SD card, USB mass-storage handover |
| `walkman_config.h` | **All wiring choices in one place** |

Third-party code: minimp3 (CC0), TJpgDec (ChaN, free licence), Tiny5 font (SIL OFL 1.1).

## Things to know

* **Display: about 0.5 s per change, colour shows a few seconds later.** The UI renders full frames into RAM. `display.c` refreshes only the changed rectangle using a fast black/white waveform. Red can only be drawn by the panel's ~15 s factory waveform, so new red appears black at first. One colour pass runs 6 s after the last button press, and a press during it cancels it. Anything that moves with the buttons is drawn black, so normal navigation never needs a colour pass; the wallpaper never changes. A ghost-clearing colour pass runs after every 40 fast updates.
* **The fast waveform is outside Waveshare's spec for this tri-colour glass.** It comes from GxEPD2's driver for the black/white UC8151D panel. Expect lighter blacks and some ghosting until a colour pass. If blacks look faint, raise `EPD_FAST_FRAMES` in `walkman_config.h`. If it looks bad on your panel, the fix is a black/white panel with real partial refresh (e.g. Waveshare 2.13" V4, 0.3 s).
* "Brightness" and "Equaliser" from the Figma list are left out: the panel has no front light, and the EQ was marked "future".
* Audio Quality 16/32-bit takes effect from the next track.

## Schematic review (from `epaltiumfiles.zip`)

I rebuilt the netlist from the `.SchDoc` geometry myself, so **confirm these with Altium's Compile / ERC**:

**Probably unconnected (port on the sheet, but no wire to the MCU pin):**
* PE5 `SAI1_SCK_A`: `SAISCK` reaches both DACs but not the MCU
* PE7/PE8 `DAC1_RST`/`DAC2_RST`: `RESET_L`/`RESET_R` stop at a port
* PB10/PB11 `I2C2`: also, the STM32 sheet port is spelled `I2C_SDA`, not `I2C2_SDA`
* PC1/PA0 `SDMMC2_CK/CMD`. The SD and eMMC port names also differ between sheets (`SDMMCLK` vs `SD_CLK`, `eMMCD0` vs `eMMC_D0`)
* nPM1300 SCL/SDA (`SCL_NPM`/`SDA_NPM`) vs the MCU side (`nPMSCL`/`nPMSDA` on PE13/PE12)

**Electrical:**
* **CS43131 VL = 1.8 V and eMMC VCCQ = 1.8 V, but the STM32 runs at 3.3 V.** The SAI, I2C, reset and SDMMC2 lines would drive 3.3 V into 1.8 V I/O. Either run the MCU VDD at 1.8 V (the H743 supports 1.62–3.6 V) or add level shifters.
* CS43131 **VP is on the 1.8 V rail.** Check the datasheet's VP range; it's normally the higher, battery-level supply.
* INMP441 **CHIPEN is on the same net as I2C1_SCL and PD5**, and its **L/R pin is floating**. CHIPEN should go to 3.3 V, and L/R to GND (left).
* DAC_R **ADR is floating.** The firmware tries 0x30 and then 0x31, but it should be tied low.
* PE12/PE13 have no hardware I2C, so the PMIC uses bit-banged I2C. If you re-spin the board, PB8/PB9 (I2C1) or PD12/PD13 (I2C4) would give it hardware I2C.
* The SD card-detect line goes only to a pull-up on 1.8 V, so the firmware just retries the card.
* nPM1300 SHPHLD is unconnected. That's fine, because power on/off uses STM32 standby with WKUP4.
