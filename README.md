# 🎞️ Soup Engine (v60: Half-Frame Diptych & Modernized Studio Edition)

***In-Camera Analog Film Simulation & 35mm Half-Frame Diptych Engine for the Canon EOS 5D Classic (2005)***

---
## 📖 Overview

Soup Engine is an in-camera film simulation and development engine running on the Canon EOS 5D Classic 133 MHz ARM946E-S processor via Magic Lantern memory environment.

This fork (jiijii0/soup-engine) is an half-frame first (Full Frame optional) version, transforming the 5D in a Half Frame camera. It operates like an analog film roll: you shoot freely at standard camera burst speeds without processing lag, then select the film stock (Film), set your desired parameters (Set) and finally develop  your captures when you choose via the in-camera batch darkroom (Lab).

This branch represents a comprehensive architectural overhaul:
- **True 35mm Half-Frame Diptych Engine**: Combines pairs of consecutive 3:4 vertical frames into a single, bordered 3:2 landscape diptych inspired by classic half-frame cameras (such as the Olympus Pen F).
- **Decoupled Three-Tier UI Architecture**: Separates the pure visual Film Magazine from the dedicated 4-row Settings Menu and the modal Batch Darkroom Lab.
- **Safe Button Bindings**: Eliminates accidental batch rendering by confining roll development to a deliberate confirmation sequence.
- **Persistent Configuration**: Stores stock selection, capture format, rendering source pipeline, resolution, and grain settings in `A:/ML/SE.CFG`, surviving battery pulls and power cycles.
- **Canon 5D menu colors**: Color scheme and menus mimicking the 5d Menus.
```
In Full-Frame Mode:
   IMG_0001.CR2          <- Original Canon RAW frame (untouched, archival)
   IMG_0001.FLM.JPG      <- Developed Full-Frame 3:2 film look

   In Half-Frame Mode:
   IMG_0001.CR2          <- Shot 1 RAW (Left half)
   IMG_0002.CR2          <- Shot 2 RAW (Right half)
   IMG_0001.HF.JPG       <- Finished 3:2 landscape diptych (Shot 1 + white divider + Shot 2)

```

---
## 🚀 What is New in Version 60

| Feature Area | Original Upstream Beta State | Version 60 State (`half-frame-5d`) |
| :--- | :--- | :--- |
| **Capture Geometry** |  Single 3:2 full-frame capture only. |  **Dual-Mode**: Full-Frame (3:2) or Half-Frame Diptych (paired 3:4 portrait slices stitched into 3:2). |
| **Workflow Timing** |  Forced auto-development after each frame (\~18s or more). |  **Analog Roll Workflow**: Zero capture delay (`se_auto_on = 0`); shoot your roll freely and batch develop when ready. |
| **Film Magazine UI** |  Film stocks and settings changes in one screen. |  **Simplified visual showcase**: clean profile descriptions |
| **Settings Management** |  No dedicated menu. |  **Dedicated 4-row Configuration Menu (`INFO` key)**: `FORMAT`, `SOURCE`, `RESOLUTION`, `GRAIN`. |
| **State Persistence** | In-memory defaults only; settings reset on power cycle. | **Full Persistence** to `A:/ML/SE.CFG` for all core engine parameters (`STOCK`, `FORMAT`, `SOURCE`, `SIZE`, `GRAIN`). |
| **Batch Development** | Armed full card sweep on single `SET` press. | **Decoupled Roll Development Lab Modal** accessed via `PLAY` or Direct Print; requires explicit two-step confirmation. |
| **`SET` Button Action** | Triggered full card development sweeps. | **Selects the highlighted film stock**, commits configuration, and immediately closes the menu to shoot. |

---
## 🕹️ In-Camera Navigation & Button Mapping

![In-Camera Navigation Flow](Flow.png)
The Soup Engine interface is invoked from the camera shooting state and organized across three distinct operational screens:
```
[ SHOOTING STATE ]
          |
          | (Press JUMP)
          v
   +--------------------------------------------------------+
   |  SE_SCREEN_STOCK: PURE FILM MAGAZINE                   |
   |  - Quick Control Dial: Cycle film stock profiles       |
   |  - SET: Select active stock and exit immediately       |
   |  - UP / DOWN: Quick-toggle Full-Frame / Half-Frame     |
   |  - LEFT / RIGHT: Quick-toggle Source (JPEG / RAW)      |
   |  - TRASH / MENU / JUMP: Exit interface                 |
   +-----------------------+-----------------+--------------+
                           |                 |
              (Press INFO) |                 | (Press PLAY)
                           v                 v
   +-------------------------------+ +-------------------------------+
   | SE_SCREEN_SETTINGS: CONFIG    | | ROLL DEVELOPMENT LAB MODAL    |
   | - UP/DOWN or Dial: Select row | | - SET: Confirm & start batch  |
   | - SET or LEFT/RIGHT: Toggle   | | - JUMP: Toggle NEW only / ALL |
   | - Auto-flushes to SE.CFG      | | - DEL / MENU: Cancel & return |
   | - INFO / MENU: Return to stock| +-------------------------------+
   +-------------------------------+

```
### Detailed Button Controls

### 1. Main Film Magazine (`SE_SCREEN_STOCK`)

![Main Film Magazine Screen](Screenshot_3.png)
- **`JUMP`**: Opens the Soup Engine magazine overlay.
- **Quick Control Dial (or `WHEEL_UP` / `WHEEL_DOWN`)**: Seamlessly rotates through available `.fs` film stocks.
- **`SET`**: Locks in the highlighted stock, flushes choice to `A:/ML/SE.CFG`, and immediately dismisses the overlay so you can shoot.
- **`UP` / `DOWN` Arrows**: Rapidly toggles between **Half-Frame Diptych (3:4)** and **Full-Frame (3:2)** capture without entering settings.
- **`LEFT` / `RIGHT` Arrows**: Rapidly toggles the rendering pipeline source between **Preview JPEG** (fast) and **Raw Sensor Data** (deep pipeline).
- **`INFO`**: Opens the structured **Settings Menu**.
- **`PLAY`**: Opens the dedicated **Roll Development Lab** confirmation modal.
- **`TRASH` / `MENU` / `JUMP`**: Closes the interface, restoring standard Canon display operation.

### 2. Configuration Settings Menu (`SE_SCREEN_SETTINGS`)

![Configuration Settings Menu](Screenshot_2.png)

Accessible by pressing **`INFO`** while the magazine is open:
- **Row 0: `FORMAT`** -> Toggle between `HALF-FRAME (3:4 DIPTYCH)` and `FULL-FRAME (3:2)`.
- **Row 1: `SOURCE`** -> Toggle between `JPEG` (high-speed DIGIC-rendered baseline) and `RAW` (pure sensor pipeline).
- **Row 2: `RESOLUTION`** -> Toggle between `FULL` and `HALF` output image sizing.
- **Row 3: `GRAIN`** -> Toggle between `AS THE STOCK SAYS` (profile-governed grain synthesis) and `OFF` (clean sensor).
- **Navigation**: Move rows with **`UP`/`DOWN`** or the **Dial**; toggle values with **`SET`** or **`LEFT`/`RIGHT`**.
- **Exit**: Press **`INFO`** or **`MENU`** to return to the Film Magazine.

### 3. Roll Development Lab (`g_roll_confirm_pending`)

![Roll Development Lab Modal](Screenshot_1.png)

Accessible by pressing **`PLAY`** while on the Film Magazine:
- Displays an on-screen darkroom slate detailing pending shots: e.g. `DEV 12 NEW DIPTYCHS (4 ALREADY DONE)`.
- **`SET`**: Confirms and immediately launches batch development, switching to the active progress screen (`draw_working`).
- **`JUMP`**: Toggles batch target between **NEW un-rendered shots only** and **re-developing ALL shots** on the card.
- **`DEL` / `MENU` / `PLAY`**: Cancels the modal and returns to the clean stock browser.

---
## 🎞️ Half-Frame Diptych Geometry & Optical Alignment

In Half-Frame mode, holding the camera in standard landscape orientation yields vertical **3:4 portrait** slices, recreating the authentic framing experience of classic 35mm half-frame cameras.

### Sensor Crop Geometry
- **Shot 1 (Left Half)**: Slices the center 50% width (columns 25% through 75%) and temporarily stages intermediate row data to `A:/ML/SHOT1.TMP`.
- **Shot 2 (Right Half)**: Slices the center 50% width of the subsequent frame, retrieves Shot 1 from `SHOT1.TMP`, injects a solid vertical white separator line, grades both halves with the active `.fs` stock curves, and encodes the final landscape diptych to `IMG_xxxx.HF.JPG` (named after Shot 1 sequence number). `SHOT1.TMP` is automatically deleted upon completion.

### Diptych Resolution & Divider Width

| Pipeline Tier | Crop per Shot (3:4) | Divider Width | Final Landscape Diptych (3:2) | Est. Processing Time |
| :--- | :--- | :--- | :--- | :--- |
| **`SOURCE: JPEG` (HALF)** | 620 × 832 px | 8 px | **1248 × 832 px (~1.04 MP)** | ~10–15 seconds total |
| **`SOURCE: JPEG` (FULL)** | 1240 × 1664 px | 16 px | **2496 × 1664 px (~4.15 MP)** | ~12–18 seconds total |
| **`SOURCE: RAW` (Max Res)** | 1632 × 2184 px | 16 px | **3276 × 2184 px (~7.15 MP)** | ~35–45 seconds total |

### Viewfinder Optical Framing Guide

The 5D Classic optical viewfinder provides physical alignment for 3:4 vertical frames:
- **Left Boundary**: Aligns with the viewfinder far-left outer autofocus point (\~25% frame mark).
- **Right Boundary**: Aligns with the viewfinder far-right outer autofocus point (\~75% frame mark).
- **Accuracy**: Keeping your subject composed between the two horizontal outer AF points guarantees \~98.2% framing precision for the cropped half-frame image.

---
## ⚙️ Persistent Configuration Engine (`A:/ML/SE.CFG`)

Settings are stored in human-readable plain text on the CompactFlash card at `A:/ML/SE.CFG`. The file is read at camera startup and updated whenever parameters change in the UI:
```
STOCK VELVIA
   FORMAT HF
   SOURCE JPEG
   SIZE HALF
   GRAIN ON

```
### Parameter Reference
- **`STOCK <name>`**: The filename stem of the active stock in `A:/ML/FILM/`. Matched by string name to prevent index drifting when stocks are added or removed.
- **`FORMAT <HF|FF>`**:
  - `HF`: Enables 2-shot 3:4 diptych capture and stitching.
  - `FF`: Standard full-frame 3:2 capture.
- **`SOURCE <JPEG|RAW>`**:
  - `JPEG`: Grades the camera high-speed embedded IFD0 baseline JPEG. Highly recommended (\~10-18 seconds per frame during batch develop).
  - `RAW`: Executes custom software sensor decoding directly from sensor Bayer data (\~90-150 seconds per frame).
- **`SIZE <HALF|FULL>`**: Selects rendering output scale (Half resolution for fast web sharing vs Full resolution).
- **`GRAIN <ON|OFF>`**: Toggles procedural film grain synthesis on or off.

---
## 📦 Installation & Updating

### Prerequisites
- A **Canon EOS 5D Classic** body (original 2005 model, firmware version 1.1.1).
- A bootable CompactFlash card with Magic Lantern installed.

### Step-by-Step Installation
1. **Backup Existing Autoexec**:

Ensure you have a copy of your card working `AUTOEXEC.BIN` stored safely on your computer.
2. **Download or Compile**:

Compile the payload using the build instructions below or copy the pre-built files:
- Copy `SE.BIN` to the card root (`A:/ML/SE.BIN` or `A:/SE.BIN`).
- Copy `AUTOEXEC.BIN` to the card root (replaces Magic Lantern loader with the Soup Engine hook).
- Ensure stock look files exist in `A:/ML/FILM/*.fs` along with `A:/ML/FILM/INDEX.TXT`.
3. **Power On**:

Insert the card and switch on the camera. Press **`JUMP`** to open the Film Magazine.

---
## 🔨 Building from Source

Soup Engine is built as an air-gapped freestanding bare-metal C99 binary compiled for the ARM946E-S architecture.

### Toolchain Requirements
- `arm-none-eabi-gcc` cross-compiler toolchain.
- Host build tools: GNU `make`, `gcc`, `python3`.

### Compilation Steps
```
# Clone the repository and switch to the half-frame branch
   git clone https://github.com/<your-fork>/soup-engine.git
   cd soup-engine/src
   git checkout half-frame-5d

   # Clean previous build artifacts
   make clean

   # Compile the ARM payload and assemble binary distribution files
   make arm && make payload

```
Upon a successful build, the toolchain outputs:
- `src/ml/build/AUTOEXEC.BIN` (ML loader trampoline).
- `src/ml/build/SE.BIN`  (Soup Engine core payload).

---
## 🛠️ Diagnostics & Safety Recovery

The camera architecture contains safety mechanisms to prevent hardware lockups:
- **Fail-Safe Startup**: If `SE.BIN` is missing or damaged on the card, `AUTOEXEC.BIN` gracefully falls back and boots standard Canon firmware without crashing.
- **Low Battery Guard**: Batch development is inhibited if the battery indicator drops below two bars, preventing power cutoffs during card writes.
- **Emergency Recovery Files**:
  - `A:/ML/SE.NOUI`: If created on the card, disables all LCD drawing and button hooks while keeping raw image capture operational. Experimental
  - `A:/ML/SE.ALL`: If placed on the card, triggers a one-time development pass across all unprocessed raw files and then automatically self-deletes. Experimental
  - `A:/ML/SE.LOG`: Check this file via a card reader if you need to inspect camera boot and development logs.