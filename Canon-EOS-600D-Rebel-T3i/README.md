# Soup Engine — Canon EOS 600D / Rebel T3i (Firmware 1.0.2)

Best shot at a 600D port adapted from the 60D version. 
!!Full transparency!!: this was vibe-coded with Claude. 
I don't own any of the other supported cameras to compare how they behave, so this has only been tested on my own 600D running firmware **1.0.2** — everything works on hardware.

## How to Install
1. Make sure your 600D is on **Canon firmware 1.0.2** with Magic Lantern installed.
2. Copy `card/ML/modules/soup.mo` into `ML/modules/` on your SD card.
3. Copy the `card/ML/FILM/` folder into `ML/` on your SD card (this holds the 21 film stocks).
4. Turn on the camera, open Magic Lantern (`Trash` button) -> **Modules**, enable `soup.mo`, and restart the camera.

## How to Use It
- **Turn it on:** Go to `Shoot -> Soup Film` in Magic Lantern, switch `Film` to `ON`, and pick a stock. (You can also press `SET` on the plain shooting screen to open the visual stock picker.)
- **Shoot & wait:** Take a photo and wait for the orange `FILM` progress bar in the top-right corner to finish.
- **View your film photos:** Canon's normal playback only shows the original `DCIM` files, so **press `SET` on a photo in playback** to toggle between your normal shot and the developed film version. On a computer, the developed JPEGs are saved in `FILM/100CANON/` on the root of your SD card.

## Quick Note on Formatting
When formatting your SD card, always format **inside the camera** with Magic Lantern's **Keep Magic Lantern** option enabled. That wipes your old photos (`DCIM` and `FILM`) while keeping `ML/modules/soup.mo` and your `ML/FILM/` presets intact.

## Building from Source
sh src/tools/build-600d.sh /path/to/magiclantern_simplified
