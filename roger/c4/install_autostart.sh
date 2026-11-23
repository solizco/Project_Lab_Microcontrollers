#!/bin/bash
# =====================================================================
# install_autostart.sh -- make the Pi start the Connect 4 game at boot
# =====================================================================
# Run ON THE PI, as your normal user (not with sudo):
#     bash /boot/firmware/roger/c4/install_autostart.sh
#     add --sim         to start in sim mode (no ESP32, fake gantry)
#     add --serial      to switch back to the real ESP32
#     add --rotate 180  for a screen mounted upside down (can be combined with the others)
#     (with neither --sim nor --serial, it keeps whatever start.sh already uses;
#      first time = real ESP32)
# Works on the Pi Zero (32-bit build) and the Pi 5 (64-bit build): it finds the
# right serial port by itself (Pi Zero: /dev/serial0, Pi 5: /dev/ttyAMA0 on pins 8/10).
# Undo:
#     bash ~/roger/c4/install_autostart.sh --remove
#
# What it does (safe to run again, it never adds anything twice):
#   1. Copies the roger folder from the SD card's bootfs to ~/roger
#      (only when run from bootfs) and makes the programs executable.
#   2. Writes ~/roger/c4/start.sh, the launcher. Edit BRAIN_ARGS in it to
#      change how the game starts (for example --sim, --rotate 180).
#   3. Adds a few lines to your login profile: when the Pi logs in on the
#      screen (tty1), it runs start.sh. Logins over SSH are not affected.
#   4. Turns on console auto-login, the UART for the ESP32 (and turns off
#      the serial login shell on it), and stops the screen from blanking.
#   5. Lets your user power off / reboot without a password (only those
#      commands), so the game's Shut down menu works.
#   6. Quiet boot: no boot text, rainbow splash or login banner, so the Pi
#      goes straight from power-on to the game.
#   7. Optional power button on the Pi itself (pin 5 / GPIO3 to pin 6 / GND):
#      a press powers off cleanly. The usual shutdown button is on the Motion
#      Controller (GPIO19, hold 2 s) and needs nothing here.
# It asks for your password once (for sudo). Reboot afterwards.
# Pressing Q in the game drops you to a normal shell.
set -u

MARK_BEGIN="# >>> connect4 autostart >>>"
MARK_END="# <<< connect4 autostart <<<"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="$HOME/roger"

say()  { printf '\n== %s\n' "$*"; }
warn() { printf '   WARNING: %s\n' "$*"; }

if [ "$(id -u)" -eq 0 ]; then
    echo "Run this WITHOUT sudo (it uses sudo itself where needed), so it sets up your user, not root."
    exit 1
fi

# login profile: bash reads ~/.bash_profile if it exists, otherwise ~/.profile
if [ -f "$HOME/.bash_profile" ]; then PROFILE="$HOME/.bash_profile"; else PROFILE="$HOME/.profile"; fi

# print a file without our marked block
without_block() { awk -v b="$MARK_BEGIN" -v e="$MARK_END" '$0==b{skip=1;next} $0==e{skip=0;next} !skip' "$1"; }

remove_block() {
    [ -f "$PROFILE" ] || return 0
    if grep -qxF "$MARK_BEGIN" "$PROFILE"; then
        without_block "$PROFILE" > "$PROFILE.tmp-c4" && mv "$PROFILE.tmp-c4" "$PROFILE"
        echo "   removed the autostart lines from $PROFILE"
    fi
}

# Which Pi is this? The Pi 5 puts the pins 8/10 UART on /dev/ttyAMA0 (its
# /dev/serial0 is the separate 3-pin debug connector), and it needs uart0 on.
MODEL="${C4_TEST_MODEL:-$(tr -d '\0' < /proc/device-tree/model 2>/dev/null)}"
case "$MODEL" in
    *"Raspberry Pi 5"*) PI5=1; SERIAL_DEV=/dev/ttyAMA0 ;;
    *)                  PI5=0; SERIAL_DEV=/dev/serial0 ;;
esac

# Options for this run
MODE=""; ROTATE=0; REMOVE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --sim)    MODE=sim ;;
        --serial) MODE=serial ;;
        --rotate) shift; [ "${1:-}" = "180" ] && ROTATE=1 ;;
        --rotate180|--flip) ROTATE=1 ;;
        --remove) REMOVE=1 ;;
    esac
    shift
done

# How the game starts. Keep what start.sh already says (read it BEFORE step 1
# replaces the folder), unless --sim or --serial was given on this run.
if [ -f "$DEST/c4/start.sh" ] && grep -q "^BRAIN_ARGS=" "$DEST/c4/start.sh"; then
    KEEP_ARGS="$(grep "^BRAIN_ARGS=" "$DEST/c4/start.sh" | head -1)"
else
    KEEP_ARGS="BRAIN_ARGS=\"--serial $SERIAL_DEV\""
fi
[ "$MODE" = sim ]    && KEEP_ARGS='BRAIN_ARGS="--sim"'
[ "$MODE" = serial ] && KEEP_ARGS="BRAIN_ARGS=\"--serial $SERIAL_DEV\""
if [ "$PI5" = 1 ]; then                                  # moved from a Pi Zero: fix the port
    KEEP_ARGS="$(echo "$KEEP_ARGS" | sed 's|/dev/serial0|/dev/ttyAMA0|')"
fi
if [ "$ROTATE" = 1 ] && ! echo "$KEEP_ARGS" | grep -q -- "--rotate 180"; then
    KEEP_ARGS="$(echo "$KEEP_ARGS" | sed 's|"$| --rotate 180"|')"
fi

if [ "$REMOVE" = 1 ]; then
    say "Removing the game autostart"
    remove_block
    if [ -f /etc/sudoers.d/020-connect4-poweroff ]; then
        sudo rm -f /etc/sudoers.d/020-connect4-poweroff && echo "   removed the passwordless power-off rule"
    fi
    echo "   Done. The game will no longer start at boot (auto-login, UART and quiet boot are left as they are)."
    exit 0
fi

# ---------------------------------------------------------------------
say "1. Installing the game files to $DEST"
SRC_ROGER="$(cd "$HERE/.." && pwd)"
case "$SRC_ROGER" in
    /boot/*)
        if [ ! -x "$HERE/brain" ] && [ ! -f "$HERE/brain" ]; then echo "   brain not found next to this script ($HERE)"; exit 1; fi
        rm -rf "$DEST"
        cp -r "$SRC_ROGER" "$DEST" || { echo "   copy failed"; exit 1; }
        rm -rf "$DEST/__pycache__"
        echo "   copied $SRC_ROGER -> $DEST"
        echo "   game version: $("$DEST/c4/brain" --version 2>/dev/null || echo unknown)"
        ;;
    "$DEST")
        echo "   already running from $DEST, nothing to copy"
        ;;
    *)
        echo "   running from $SRC_ROGER (not bootfs and not ~/roger); using it in place"
        DEST="$SRC_ROGER"
        ;;
esac
C4="$DEST/c4"
chmod +x "$C4"/brain "$C4"/bench "$C4"/test_ai "$C4"/test_brain "$C4"/test_ui 2>/dev/null
if "$C4/brain" --version >/dev/null 2>&1; then
    echo "   $("$C4/brain" --version) runs on this Pi (${MODEL:-unknown model}, $(uname -m))"
else
    warn "the game program does not run on this Pi (${MODEL:-unknown model}, $(uname -m))."
    warn "Use the c4 folder built for it: Pi 5 = the 64-bit build (c4_pi5.zip), Pi Zero = c4_pi_zero.zip."
fi
if [ -d "$DEST/faces" ] && ls "$DEST"/faces/*.face >/dev/null 2>&1; then
    echo "   faces: $(ls "$DEST"/faces/*.face | wc -l) face files found"
else
    warn "no .face files in $DEST/faces. Run convert.bat on the laptop and copy the faces folder into roger."
fi

# ---------------------------------------------------------------------
say "2. Writing the launcher $C4/start.sh"
cat > "$C4/start.sh" <<EOF
#!/bin/bash
# Started automatically at boot on the screen (tty1). Edit BRAIN_ARGS to change
# how the game starts, for example:
#   BRAIN_ARGS="--serial $SERIAL_DEV"           real ESP32 (normal)
#   BRAIN_ARGS="--sim"                           no ESP32, fake gantry
#   BRAIN_ARGS="--serial $SERIAL_DEV --rotate 180"   screen mounted upside down
$KEEP_ARGS

cd "\$(dirname "\$0")"
while true; do
    ./brain \$BRAIN_ARGS
    code=\$?
    [ \$code -eq 0 ] && exit 0      # Q pressed (or shut down): back to the shell
    [ \$code -eq 3 ] && exit 0      # power-off was refused: its message is on screen
    echo
    echo "The game stopped (exit code \$code). See ~/c4_log.txt."
    echo "Restarting in 5 seconds. Press Ctrl+C now for a command line instead."
    sleep 5 || exit 0
done
EOF
chmod +x "$C4/start.sh"
echo "   $KEEP_ARGS"

# ---------------------------------------------------------------------
say "3. Starting the game at login on the screen ($PROFILE)"
remove_block >/dev/null
cat >> "$PROFILE" <<EOF
$MARK_BEGIN
# Start the Connect 4 game when logged in on the screen (tty1), not over SSH.
# Remove with: bash ~/roger/c4/install_autostart.sh --remove
if [ "\$(tty)" = "/dev/tty1" ] && [ -x "$C4/start.sh" ]; then
    "$C4/start.sh"
fi
$MARK_END
EOF
echo "   added"

# ---------------------------------------------------------------------
say "4. System settings (needs sudo)"
if command -v raspi-config >/dev/null 2>&1; then
    sudo raspi-config nonint do_boot_behaviour B2 && echo "   console auto-login: on" || warn "could not turn on auto-login (raspi-config > System Options > Boot / Auto Login > Console Autologin)"
    sudo raspi-config nonint do_serial_cons 1 && echo "   serial login shell: off" || warn "could not turn off the serial console (raspi-config > Interface Options > Serial Port)"
    sudo raspi-config nonint do_serial_hw 0 && echo "   serial port hardware: on" || warn "could not enable the serial port (raspi-config > Interface Options > Serial Port)"
else
    warn "raspi-config not found; set auto-login and the serial port by hand"
fi
if [ "$PI5" = 1 ]; then
    for CFG in /boot/firmware/config.txt /boot/config.txt; do
        [ -f "$CFG" ] || continue
        if grep -q "^dtparam=uart0=on" "$CFG"; then
            echo "   Pi 5 UART on pins 8/10 (/dev/ttyAMA0): already on"
        else
            sudo cp "$CFG" "$CFG.bak-connect4-uart" && echo "dtparam=uart0=on" | sudo tee -a "$CFG" >/dev/null \
                && echo "   Pi 5 UART on pins 8/10 (/dev/ttyAMA0): on (backup at $CFG.bak-connect4-uart)"
        fi
        break
    done
fi
for CMD in /boot/firmware/cmdline.txt /boot/cmdline.txt; do
    if [ -f "$CMD" ]; then
        if grep -q "consoleblank=" "$CMD"; then
            echo "   screen blanking: already set in $CMD"
        else
            sudo cp "$CMD" "$CMD.bak-connect4" && sudo sed -i '1 s/$/ consoleblank=0/' "$CMD" && echo "   screen blanking: off (backup at $CMD.bak-connect4)"
        fi
        break
    fi
done
for G in video dialout tty; do
    id -nG | tr ' ' '\n' | grep -qx "$G" || { sudo usermod -aG "$G" "$USER" && echo "   added $USER to group $G"; }
done

# ---------------------------------------------------------------------
say "5. Power off without a password"
ME="$(id -un)"
RULE=/etc/sudoers.d/020-connect4-poweroff
TMP_RULE="$(mktemp)"
{
    echo "# Added by install_autostart.sh (Connect 4): lets $ME power off / reboot without a"
    echo "# password so the game's Shut down menu works. Only these commands are allowed."
    echo "$ME ALL=(root) NOPASSWD: /usr/sbin/shutdown, /sbin/shutdown, /usr/sbin/poweroff, /sbin/poweroff, /usr/sbin/reboot, /sbin/reboot, /usr/bin/systemctl poweroff, /usr/bin/systemctl reboot, /bin/systemctl poweroff, /bin/systemctl reboot"
} > "$TMP_RULE"
# A broken sudoers file can lock you out of sudo, so it is checked before it is installed.
if sudo visudo -cf "$TMP_RULE" >/dev/null; then
    sudo install -m 0440 -o root -g root "$TMP_RULE" "$RULE" && echo "   $ME can now power off from the game menu"
else
    warn "the power-off rule failed its check and was NOT installed"
fi
rm -f "$TMP_RULE"

# ---------------------------------------------------------------------
say "6. Quiet boot (straight from power-on to the game)"
touch "$HOME/.hushlogin" && echo "   login banner: off"
for CMD in /boot/firmware/cmdline.txt /boot/cmdline.txt; do
    [ -f "$CMD" ] || continue
    ADDED=""
    for TOK in quiet loglevel=3 logo.nologo vt.global_cursor_default=0; do
        KEY="${TOK%%=*}"
        if ! tr ' ' '\n' < "$CMD" | grep -q "^${KEY}\(=\|$\)"; then
            sudo sed -i "1 s/\$/ $TOK/" "$CMD" && ADDED="$ADDED $TOK"
        fi
    done
    if [ -n "$ADDED" ]; then echo "   boot text: hidden (added:$ADDED)"; else echo "   boot text: already hidden"; fi
    break
done
for CFG in /boot/firmware/config.txt /boot/config.txt; do
    [ -f "$CFG" ] || continue
    if grep -q "^disable_splash=1" "$CFG"; then
        echo "   rainbow splash: already off"
    else
        sudo cp "$CFG" "$CFG.bak-connect4" && echo "disable_splash=1" | sudo tee -a "$CFG" >/dev/null && echo "   rainbow splash: off (backup at $CFG.bak-connect4)"
    fi
    break
done

# ---------------------------------------------------------------------
say "7. Power button on the Pi (optional: pin 5 / GPIO3 to pin 6 / GND)"
# The main shutdown button lives on the Motion Controller (GPIO19, hold 2 s);
# the game powers off when it sees it. This optional Pi-side button works even
# if the game is frozen: the kernel turns a press into a power-key event and
# Linux powers off cleanly (its default action).
for CFG in /boot/firmware/config.txt /boot/config.txt; do
    [ -f "$CFG" ] || continue
    if grep -q "^dtoverlay=gpio-shutdown" "$CFG"; then
        echo "   Pi power button: already enabled"
    else
        sudo cp "$CFG" "$CFG.bak-connect4-button" && echo "dtoverlay=gpio-shutdown" | sudo tee -a "$CFG" >/dev/null \
            && echo "   Pi power button: enabled (backup at $CFG.bak-connect4-button)"
    fi
    break
done
if [ -f /etc/systemd/logind.conf.d/connect4-button.conf ]; then          # an earlier version made it reboot
    sudo rm -f /etc/systemd/logind.conf.d/connect4-button.conf && echo "   Pi power button: set back to power off (was reboot)"
fi

# ---------------------------------------------------------------------
say "Checking for other programs that start at boot"
FOUND=0
for F in "$HOME/.bash_profile" "$HOME/.profile" "$HOME/.bashrc"; do
    [ -f "$F" ] || continue
    if without_block "$F" | grep -nE "connect4\.py|gifface\.py|python3? .*roger" ; then
        warn "the lines above in $F also start something on login"; FOUND=1
    fi
done
if crontab -l 2>/dev/null | grep -nE "@reboot"; then warn "crontab has @reboot entries (crontab -e to view)"; FOUND=1; fi
for U in /etc/systemd/system/*.service; do
    [ -f "$U" ] || continue
    if grep -qE "connect4|gifface|roger" "$U"; then warn "systemd service $(basename "$U") looks related (sudo systemctl disable $(basename "$U"))"; FOUND=1; fi
done
[ $FOUND -eq 0 ] && echo "   nothing else found"
[ $FOUND -eq 1 ] && echo "   If one of these is Finn's Python menu, turn it off, or both will fight over the screen."

say "Checking that the game can power off (shutdown button / Shut down menu)"
sudo -k      # forget the password typed above, so this tests the rule the way the game uses it
if sudo -n -l /usr/sbin/shutdown -h now >/dev/null 2>&1 || sudo -n -l /sbin/shutdown -h now >/dev/null 2>&1; then
    echo "   OK: power-off works without a password"
else
    warn "FAILED: '$ME' still needs a password to power off. Look at: sudo cat $RULE"
fi

say "Done. Reboot to start the game:  sudo reboot"
echo "   To shut down later: game menu > Options > Shut down. Wait for the green light to go off, then unplug."
