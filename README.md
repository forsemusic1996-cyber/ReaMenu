# FSR ReaMenu

Native Windows REAPER extension that adds a persistent, XML-configurable menu to the **MIDI Editor** menu bar.

This is the MIDI Editor counterpart of [ReaCustomMenu](https://github.com/danloadFR/ReaCustomMenu). It keeps the same core behavior:

- menus are described in `ReaMenu.xml`;
- actions can use numeric REAPER command IDs or named IDs such as `_RS...`;
- nested submenus and separators are supported;
- the menu is registered through `AddCustomizableMenu` and populated through `hookcustommenu`;
- MIDI Editor action context is selected with the `MIDI Editor` keyboard section.

## Installation

1. Download `reaper_ReaMenu.dll` from the latest GitHub Actions artifact.
2. Copy it to REAPER's `UserPlugins` folder.
3. Copy `ReaMenu.xml` to REAPER's resource folder.
4. Restart REAPER and open a MIDI Editor.

## Configuration

Edit `ReaMenu.xml` while REAPER is closed, or replace it and restart REAPER. The root element is `<menus>` and each menu is a `<mainmenu>`:

```xml
<menus>
  <mainmenu id="fsr_reamenu_midi_editor" name="FSR ReaMenu">
    <action command="40142" name="Insert note" />
    <menu name="Tools">
      <action command="_RS1234567890" name="My script" />
    </menu>
    <separator />
  </mainmenu>
</menus>
```

Replace the example commands with the IDs from your MIDI Editor action list when customizing the menu.

## Build

The repository contains a GitHub Actions workflow for a reproducible x64 MSVC Release build on `windows-2022`. The generated DLL is uploaded as the `FSR-ReaMenu-windows-x64` artifact.

## License

MIT. See [LICENSE](LICENSE).
