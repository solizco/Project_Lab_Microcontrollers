Connect Four Game Brain - prebuilt for Raspberry Pi 4 and Raspberry Pi 5 (64-bit Raspberry Pi OS)
=================================================================================================
This is the 64-bit build (Pi 4 and Pi 5). The Pi Zero needs the other build (c4_pi_zero.zip);
install_autostart.sh tells you if the wrong one is on the card.

PI 4, NO ESP32 (the normal setup now)
  The Pi drives everything itself: stepper driver, limit switch, servo, the three
  buttons and the shutdown button, straight from its GPIO pins.

  1. Install:   bash /boot/firmware/roger/c4/install_autostart.sh --gpio --rotate 180
                (it creates roger/pins.conf on the SD card, blank)
  2. Pins:      open roger/pins.conf on the SD card (from Windows is fine) and fill in
                the BCM GPIO number of each wire (GPIO17 -> 17, not the physical pin).
                Run the install script again if you set or change SERVO (it sets up
                the servo's hardware PWM), then reboot.
  3. Check:     ~/roger/c4/brain --check-pins
                shows the pins it read, any mistakes, whether the GPIO opened, the
                servo mode, and what the limit switch and buttons read right now.
  4. Servo:     game menu > Options > Servo calibration. Left/right move, select edits
                a value (the servo moves live), Test red/black drop, Save. The bottom
                line shows the live servo output (pulse width, on/off, verified).
     "Check signal" on that screen measures what really comes out of the servo pin.
  5. Servo test without the game (quit it with Q first):
                ~/roger/c4/brain --servo-test           (add --soft to try software PWM)
                moves only the servo: rest, red, rest, black, rest, and prints what
                the Pi's PWM is really outputting at each step. Every line "verified"
                but no movement = servo power or ground, not software.
  Nothing set up yet? The game still starts, on the screen only, and says what is missing.

  Wiring rules
    - Limit switch, buttons, shutdown button: GPIO to GND when pressed (pull-ups are on).
    - TMC2209: STEP, DIR, EN to their GPIOs, VIO to the Pi's 3.3 V (pin 1 or 17),
      GND to a Pi GND, motor power (VM) from the motor buck, never near the Pi.
    - Servo signal on GPIO 12, 13, 18 or 19 = smooth hardware PWM. Servo power from
      its own 5 V buck, servo ground joined to the Pi ground.
    - EN on GPIO 0-8 keeps the motor off while the Pi boots (those start HIGH).
    - Never put 5 V or the motor voltage on any GPIO. 3.3 V logic only.
  Logs: ~/c4_log.txt (lines starting with "gantry:" are the motion code; every drop
        logs its servo angles and the PWM state; "power:" lines report under-voltage).

  Fail-safes built in
    - The servo pin itself is measured at start-up (a built-in logic analyzer reads the
      pin's real level): if hardware PWM isn't reaching the pin, it switches to
      software PWM by itself and measures again. "the Pi IS sending the right signal"
      in the log means any remaining problem is servo power, ground or the wire.
    - Servo PWM is read back after every change. A channel found off is set up again;
      if the hardware PWM keeps refusing, it switches to software PWM by itself.
    - A failed re-home after a drop no longer ends the game: the piece is in, so the
      game continues, and the gantry homes again (in the background, then before the
      next move). Only a second failure stops the game.
    - The drop timeout counts silence, not time: a slow drop that keeps reporting
      progress is fine; a hung one still faults after 15 s.
    - Under-voltage on the Pi's 5 V supply is detected and shown on screen.
    - Limit switch: must read pressed for LIMIT_FILTER_MS (3 ms) with the motor
      paused, AND homing touches it twice (back off 3 mm, touch again slowly), so
      motor noise on the wire can't fake a home. False touches are counted in the log.
    - Lost steps: every re-home checks the distance travelled against where the
      gantry should have been, and logs a warning if the belt slipped or steps were
      missed (the re-home itself already corrects the position).
    - Buttons: a press must hold for BUTTON_STABLE_MS (25 ms), so noise blips on the
      button wires can't press anything. A button stuck down at start-up is reported
      and ignored until it is released.
    - Motor rest: after IDLE_MOTOR_OFF_S (300 s) without moving, the driver turns off
      (cooler, longer battery). The gantry homes again before its next move.
    - pins.conf mistakes are shown on the screen at start-up, not only in the log.
    - SELECT = press LEFT and RIGHT together (CHORD_SELECT = 1, the default). An
      arrow press waits up to CHORD_WINDOW_MS (150 ms) to see if the other arrow
      follows; a quick tap is sent as soon as it is released. After a select, both
      arrows are ignored until both are let go. BTN_SELECT can stay blank; if it is
      wired it still works too.
  Optional pins.conf settings: SERVO_HOLD (0 = limp between drops), SERVO_BOOT_TEST
  (1 = wiggle at start), SERVO_FORCE_SOFTWARE, LIMIT_INVERT, LIMIT_FILTER_MS,
  TWO_TOUCH_HOMING, BUTTON_STABLE_MS, IDLE_MOTOR_OFF_S (0 = motor always on),
  CHORD_SELECT, CHORD_WINDOW_MS.

OLD SETUP (ESP32 Motion Controller over the UART) still works: install with --serial.
  Pi 4: /dev/serial0 (Bluetooth is turned off so pins 8/10 get the full UART)
  Pi 5: /dev/ttyAMA0 (turned on with dtparam=uart0=on)
Screen: micro-HDMI port HDMI0 (next to the USB-C power port). Power: 5 V at 3 A or more.

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
   ./brain --serial /dev/serial0     with the real ESP32 (Pi 4; Pi 5: /dev/ttyAMA0)

   Keyboard: arrows or a/d move, Enter select, Esc back, B small board,
             Q quit. On Statistics: T test game, C twice = clear.
   ESP32 buttons: left/right move, select chooses.

   Options:  --faces DIR   --rotate 180 (upside-down screen)
             --text (old text screen)   --board (small board always on)

   ./brain --version      prints the build date. The same build date is shown
                          small in the bottom right of the main menu, so you
                          can tell the Pi is running the newest copy.

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
   pieces; menus always work with both. Turn it off from the same screen
   ("Turn off screen only"). Works with no ESP32 plugged in (choose
   Keyboard), and from a gantry fault.
   NOT remembered after power-off: every start is normal gantry mode with
   the Motion Controller buttons, so the Pi can't come up stuck in keyboard
   mode with no keyboard attached.
   With Screen only OFF (normal play), the buttons and keyboard both work.

5b. COIN TOSS
   Every game starts with a coin toss. Call Heads or Tails (vs R.O.G.E.R you
   call; in 1v1 Player One calls). Whoever wins the toss moves first. Colors
   never change: R.O.G.E.R / Player One are red, you / Player Two are black.

5c. GAME OVER
   When the game ends, any reaction still playing finishes its loop, then
   R.O.G.E.R's win or loss GIF plays once all the way through. Then a results
   screen: "YOU SUCK" (R.O.G.E.R won), "R.O.G.E.R SUCKS" (you won),
   "PLAYER ONE WINS!" / "PLAYER TWO WINS!" (1v1) or "DRAW", with Play again.
   Play again re-homes the gantry and goes back to mode selection.
   Select during the GIF skips straight to the results screen.

6. R.O.G.E.R'S FACE
   Idle loops during your turn. The moment you press drop he reacts (good or
   bad move) and that reaction loops until your next turn; it always finishes
   the loop it is on before idle comes back. ~/c4_log.txt shows each judgement.

7. SHUTDOWN BUTTON (on the Motion Controller)
   Pushbutton from ESP32 GPIO19 to GND. Hold it 2 seconds: the Pi shows
   "Shutting down..." and powers off cleanly, from any screen, even mid-game.
   Wait for the Pi's green light to stay off, then pull power or swap the SD.
   (Needs the updated Motion Controller firmware and the game running.)
   If the Pi refuses to power off, the game now stays running, shows the
   reason in a red box, and logs every attempt in ~/c4_log.txt (lines
   starting "power-off:"). The installer ends with "OK: power-off works
   without a password" when the permission is in place.
   Optional backup: a button from Pi pin 5 (GPIO3) to pin 6 (GND) also powers
   off cleanly, even if the game is frozen.

8. POWERING OFF
   Menu > Options > Shut down > Shut down. Wait until the green light stays
   off, then cut the power. Never pull power while Linux is running.

8. FAULTS: RECOVER WITHOUT A POWER CYCLE
   The red MOTION FAULT screen now has four choices (left/right, select):
     Back to menu     re-homes the gantry and goes back to mode selection
     Restart gantry   reboots the Motion Controller (like its EN button)
     Link debug       see below
     Options          screen only, GIF test, shut down
   New fault codes:
     0xFC  the Motion Controller can't hear the Pi (it keeps sending its
           boot READY). Check the wire from Pi pin 8 to its GPIO14.
     0xFB  the Motion Controller restarted mid-game. "brownout" as the
           reason means its supply dipped when the motor or servo started.
     0xFD  no reply after a drop (as before).

9. LINK DEBUG (Options > Link debug, or from the fault screen)
   Live view of the UART: frames and bytes each way, whether the board
   hears the Pi, its firmware version, last restart reason, homed, limit
   switch, corrupted frames, and the last few messages both ways. It pings
   the board every 3 s. A one-line verdict says which wire to check.
   Buttons along the bottom: Ping, Home, Column N (moves the gantry one
   column each press), Drop in N (drops a RED piece there, game untouched),
   Restart, Back. The tools work from the menu or a fault, not mid-game.
   Everything shown there is also written to ~/c4_log.txt.
   Needs Motion Controller firmware v2 for the board-side details; with
   the old firmware you still get the Pi-side counts and traffic.

10. FIXES IN THIS BUILD (Nov 9)
   - Re-zeroing: the Motion Controller homes on the limit switch after
     every drop in every mode (1v1 and vs R.O.G.E.R), so drift never builds
     up. Then the next turn drives the gantry out to the selected column.
   - If homing fails (for example after a cold power-up), the Pi restarts
     the Motion Controller once by itself, the same as pressing its EN
     button, before showing a fault.
   - The main menu shows how long the gantry has been homing. Past 25 s it
     points you to Options > Link debug.
   - Screen only can be switched on or off at any time from the menu, even
     while the gantry is still homing.
   Motion Controller firmware (main.cpp from the same delivery):
   - Buttons and the shutdown button keep working while the gantry moves
     or homes, so the menus no longer look frozen during a long home.
   - A failed home is noticed in about 9 s instead of 16 s.
   - If boot homing fails, the board restarts itself (like pressing EN), at
     most twice per power-up.

11. IF THE GANTRY ONLY HOMES AFTER PRESSING EN ON THE ESP32
   Watch two things after a cold power-up on battery (USB unplugged):
     - the ESP32's blue LED (firmware build 3): dark = the ESP32 is not
       running at all. Fast blink = homing, solid = homed, slow blink =
       homing failed and it is retrying by itself.
     - the main menu: "NO SIGNAL from the Motion Controller" means the Pi
       has not received one byte from it.
   LED dark + NO SIGNAL = the ESP32 did not start. No program can fix that,
   because no program is running. Hardware fix, on the breadboard:
     a) 10 uF capacitor from the ESP32's EN pin to GND (minus leg to GND).
        This holds it in reset until the power has settled.
     b) Optional, lets the Pi press EN by itself: a 1 k resistor from the
        wire that goes Pi pin 8 -> ESP32 GPIO14 over to the EN pin (keep
        the capacitor from a). When the Pi hears nothing for 6 s it holds
        that wire low for 0.3 s, which resets the ESP32. The game already
        does this; without the resistor it has no effect.
   LED blinking but the gantry does not move = the ESP32 runs and the motor
   driver is the problem; the board retries every 3 s and the Pi goes back
   to the menu by itself when homing succeeds.
