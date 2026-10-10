Connect Four Game Brain - prebuilt for Raspberry Pi Zero (ARMv6, 32-bit OS)

1. FACES (Finn's GIFs)
   On the laptop, in the roger folder, double-click convert.bat. It fills
   roger/faces with .face files. Copy the "faces" folder (or the whole
   "roger" folder) onto the SD card's bootfs, next to this c4 folder.
   The game looks in: ~/faces, ~/roger/faces, /boot/firmware/faces,
   /boot/firmware/roger/faces (and the same under /boot).

2. ON THE PI
   rm -rf ~/c4 && cp -r /boot/firmware/c4 ~/c4
   cd ~/c4
   chmod +x brain bench test_ai test_brain
   ./brain --sim          full game on the screen with a fake gantry
   ./brain --serial /dev/serial0     with the real ESP32

   Keyboard: arrows or a/d move, Enter select, Esc back, B small board,
             Q quit. On Statistics: T test game, C twice = clear.
   ESP32 buttons: left/right move, select chooses.

   Options:  --faces DIR   --rotate 180 (upside-down screen)
             --text (old text screen)   --board (small board always on)

3. FILES IT WRITES
   ~/c4_log.txt            what happened, which faces loaded, frame timing
   ~/connect4_stats.json   statistics (same file Finn's connect4.py uses)
   ~/.config/connect4/settings.conf   last mode picked

If the screen stays on the text console, read ~/c4_log.txt or run
./brain --sim 2>&1 | head   to see the graphics error.

4. START AT BOOT
   bash /boot/firmware/roger/c4/install_autostart.sh      (no sudo)
   sudo reboot
   It asks for your password once. It copies roger to ~/roger, starts the
   game on the screen at every boot (no boot text, no login), restarts it if
   it crashes, lets the Shut down menu power off without a password, and
   Q drops you to a command line.
   Change how it starts: run it again with --sim or --serial,
                         or edit BRAIN_ARGS in ~/roger/c4/start.sh
   Turn it off:          bash ~/roger/c4/install_autostart.sh --remove

5. SCREEN-ONLY PLAY (no gantry)
   Menu > Options > Screen only, then pick how to play:
     Keyboard                    the keyboard plugged into the Pi
     Motion Controller buttons   the three buttons on the ESP32
   Games then play entirely on the display (full board, falling pieces) and
   the gantry stays parked. During a game only the chosen controls move
   pieces; menus always work with both. Remembered after a reboot. Turn it
   off from the same screen ("Turn off screen only"). Works with no ESP32
   plugged in (choose Keyboard), and from a gantry fault.

6. POWERING OFF
   Menu > Options > Shut down > Shut down. Wait until the green light stays
   off, then cut the power. Never pull power while Linux is running.
