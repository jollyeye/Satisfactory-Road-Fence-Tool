# Road Maker 1.5.1 roadmap

Started 2026-11-24, after 1.5.0 (released 2026-11-22). Fixes first.

## Fixes
- **Dismantling roads broke the game's vehicle path network** (issue #11, muaxl, on 1.4.1; built 2026-11-24, to test):
  a mass dismantle of roads crashed in `AFGVehicleSubsystem::CleanNetworkIDOnElements` (a dead path element still in
  a network) a minute later, and the autosave in between had a broken network that no longer loads. The roads handed
  every vehicle path segment on them to the game as dismantle children, all in one frame, and could hand one that was
  already being removed (another piece of the same mass dismantle). Now: vehicle path segments are no longer children;
  when a road / intersection is dismantled, the segments on it are removed after it, one per frame, through the game's
  own dismantle, as if dismantled by hand (`RMRiders::QueuePaths`). Tracks and markings stay children, but never ones
  already destroyed or dismantled (`RMRiders::IsAlive`). `RM.DismantlePathsAsChildren 1` brings back the old way.
  The broken save itself cannot be repaired from the mod (the game's network data is corrupt in it).
- **Multiplayer rubberbanding walking on roads** (issue #10, CoolerTaurtis, 2026-11-23): questions asked, waiting.
  Suspect: long roads (> 125 m) are drawn in sections whose mesh components are made at run time with generated
  names, so a client standing on one cannot name it to the server (movement base). Fix candidate: fixed names +
  `SetNetAddressable`.

## Translations
- zh-Hans complete from muaxl (issue #11 attachment, 200/200), in the repo 2026-11-24: same entries and source texts as ours, only the 155 empty translations filled, placeholders checked. Next build compiles it (check `localization-editor-log.txt`).
