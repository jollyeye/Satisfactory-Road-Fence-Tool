# Releasing Road Maker on ficsit.app

## 1. Build the release zip (Alpakit Release)
1. Close the game.
2. Run `D:\SFMods\update-and-package.bat` once (pulls the latest code, assets and materials) and make sure it ends with ALL DONE.
3. Open the project in Unreal Editor (`D:\SFMods\SML\FactoryGame.uproject`).
4. In the toolbar click **Alpakit** › **Alpakit Release**.
5. In the list, tick **RoadMaker**. Under targets tick **Windows** and **WindowsServer** (leave LinuxServer off unless the Linux cross-compile toolchain is installed).
6. Click **Alpakit Release!** and wait for "Packaging finished".
7. The zip is at `D:\SFMods\SML\Saved\ArchivedPlugins\RoadMaker\RoadMaker.zip`.

## 2. Create the mod on ficsit.app (first release only)
1. Go to https://ficsit.app and sign in (GitHub or Google).
2. Click your profile › **New Mod**.
3. Fill in the fields from `ficsit-page.md` (name, mod reference `RoadMaker`, short and full description) and upload `RoadMaker_logo.png` as the logo.
4. Save.

## 3. Upload the version
1. On the mod's page click **New Version**.
2. Upload `RoadMaker.zip`, stability **Release**, paste the changelog from `ficsit-page.md`.
3. Submit. The site checks the zip (this can take a few minutes); new mods may also wait for approval.

## Later updates
Raise `SemVersion`/`VersionName` (and `Version`) in `RoadMaker.uplugin`, run the bat, do step 1 again, then step 3 with a new changelog.
