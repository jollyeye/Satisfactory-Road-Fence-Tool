# Notes for Claude working on Road Maker

## Localization: every text players can read must be translatable (since 1.5.0)

Road Maker ships translations (zh-Hans by muaxl, GitHub issue #9). Any new or changed text has to go through the
localization system, or it shows in English for everyone.

- **C++:** never `FText::FromString` / `FString::Printf` for text players see. Use
  `NSLOCTEXT("RoadMaker", "<UniqueKey>", "English text")`, with `FText::Format` and `{0}` placeholders for numbers
  and names (`FText::AsNumber`). Log lines and internal names (for example `ARMBuildableMarking::VehicleName`, which
  looks up the sign icons) stay plain.
- **Assets (`Tools/create_assets.py`):** always `text("English text", "<Key>")` with a fixed key. It goes into the
  string table `/RoadMaker/Text/ST_RoadMaker` (namespace `RoadMakerAssets`) through `URMEditorTools::TableText`.
  A `text()` without a key is not translatable. Key style: `<ToolName>.Name` / `<ToolName>.Desc` for buildings
  (the building, its descriptor and its recipe share the key), `BuildMode.*`, `Menu.*`, `Customizer.*`,
  `RoadLines.*`, `RoadMaterial.*`, `Input.*`, `Schematic.*`.
- **Keys never change** once released: changing a key throws its translations away. Changing the English text of an
  existing key is fine (the translation is then marked out of date).
- **PO files:** `Content/Localization/RoadMaker/<culture>/RoadMaker.po` (tracked). The build imports them and compiles
  the .locres (`Config/Localization/RoadMaker.ini`, GatherText step in `Tools/update-and-package.bat`; no export).
  For every new text, add an entry with an empty `msgstr` to each PO: `msgctxt "RoadMaker,<Key>"` (C++) or
  `msgctxt "RoadMakerAssets,<Key>"` (assets), `msgid` = the English text.
- **Check after a build:** `D:\SFMods\localization-editor-log.txt` and the manifest
  `Content/Localization/RoadMaker/RoadMaker.manifest` (UTF-16) should list the new texts.
- **At each release:** list the new and changed texts for the translators. For 1.5.0: comment on issue #9 for muaxl
  (only with the user's go-ahead) with the PO's untranslated entries, including the new 1.5.0 buildings.
