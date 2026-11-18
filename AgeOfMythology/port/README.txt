Age of Mythology: The Titans, recompiled, as a ROCKNIX port (aarch64).

No game data is in this package: the program is derived from your own
installed game's executable, so it is only for the owner of the game.

1. On the device: ROCKNIX with SSH enabled (the root password is under System
   Settings in EmulationStation).
2. Copy the port:
     scp -r AgeOfMythology.sh aom root@DEVICE:/storage/roms/ports/
3. Copy your game directory's contents (not the installers):
     rsync -av --exclude='*.msi' --exclude='ar505enu.exe' --exclude='autopatcher.exe' \
         /path/to/game/ root@DEVICE:/storage/roms/ports/aom/game/
4. Start "Age of Mythology" from the Ports menu, or over ssh:
     ssh root@DEVICE bash /storage/roms/ports/AgeOfMythology.sh
   The log is aom/log.txt. aom.conf holds the executable (aomx.exe or aom.exe)
   and extra options; pad.cfg, if present, is the controller mapping.
