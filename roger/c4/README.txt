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
     Motion Controller buttons   the three buttons on the ESP32 (default)
     Keyboard                    the keyboard plugged into the Pi
   Games then play entirely on the display (full board, falling pieces) and
   the gantry stays parked. During a game only the chosen controls move
   pieces; menus always work with both. Remembered after a reboot. Turn it
   off from the same screen ("Turn off screen only"). Works with no ESP32
   plugged in (choose Keyboard), and from a gantry fault.
   With Screen only OFF (normal play), the buttons and keyboard both work.

5b. COIN TOSS
   Every game starts with a coin toss. Call Heads or Tails (vs R.O.G.E.R you
   call; in 1v1 Player One calls). Whoever wins the toss moves first. Colors
   never change: R.O.G.E.R / Player One are red, you / Player Two are black.

6. R.O.G.E.R'S FACE
   Idle loops during your turn. The moment you press drop he reacts (good or
   bad move) and that reaction loops until your next turn; it always finishes
   the loop it is on before idle comes back. ~/c4_log.txt shows each judgement.

7. SHUTDOWN BUTTON (on the Motion Controller)
   Pushbutton from ESP32 GPIO19 to GND. Hold it 2 seconds: the Pi shows
   "Shutting down..." and powers off cleanly, from any screen, even mid-game.
   Wait for the Pi's green light to stay off, then pull power or swap the SD.
   (Needs the updated Motion Controller firmware and the game running.)
   Optional backup: a button from Pi pin 5 (GPIO3) to pin 6 (GND) also powers
   off cleanly, even if the game is frozen.

8. POWERING OFF
   Menu > Options > Shut down > Shut down. Wait until the green light stays
   off, then cut the power. Never pull power while Linux is running.
