# FSR ReaMenu

Native Windows REAPER extension that adds a persistent, XML-configurable menu to the **MIDI Editor** menu bar.

This is the MIDI Editor counterpart of [ReaCustomMenu](https://github.com/danloadFR/ReaCustomMenu). It keeps the same core behavior:

- menus are described in `ReaMenu.xml`;
- actions can use numeric REAPER command IDs or named IDs such as `_RS...`;
- nested submenus and separators are supported;
- the menu remains available to REAPER's Customize Menus editor through `AddCustomizableMenu`;
- the top-level item is inserted into the real Windows menu bar of the active MIDI Editor;
- menu items are populated from the same XML structure as ReaCustomMenu and use REAPER action IDs.

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

The extension uses REAPER's `MIDIEditor_GetActive()` API and the active editor's
native menu handle. This is required because `AddCustomizableMenu()` can register
a menu for REAPER's menu editor, but its keyboard-section parameter does not attach
a new top-level item to the built-in MIDI Editor menu bar.

## Build

The repository contains a GitHub Actions workflow for a reproducible x64 MSVC Release build on `windows-2022`. The generated DLL is uploaded as the `FSR-ReaMenu-windows-x64` artifact.

## License

MIT. See [LICENSE](LICENSE).
