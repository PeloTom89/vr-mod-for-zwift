@echo off
rem VR mod for Zwift: puts Zwift's own graphics settings back (zvr_loader.exe kept them as backups when it installed
rem the VR ones). Windows asks for administrator rights, because Zwift is installed under Program Files.
rem To keep Zwift's own settings while still using the mod, also set vr_graphics = 0 in zwift_vr.ini.
"%~dp0zvr_loader.exe" --restore
