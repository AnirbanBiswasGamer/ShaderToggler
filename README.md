# ShaderToggler
Reshade 5.1+ add-on to toggle game shaders on/off in groups based on a key press. It allows you to configure these groups from within the add-on as well. 
This addon is meant to toggle a game's shaders on/off, not ReShade effects. 

Available for **both 64-bit (x64) and 32-bit (x86)** games with full feature parity.

---

## How to use

### 64-bit Games
Place `ShaderToggler.addon64` into the directory where the 64-bit game executable is located. In most cases, this is where the ReShade 5.1+ 64-bit DLL (`dxgi.dll`, `d3d11.dll`, etc.) is installed.

### 32-bit Games
Place `ShaderToggler.addon32` (or `ShaderToggler.addon`) into the directory where the 32-bit game executable is located. ReShade loads both `.addon32` and `.addon` for 32-bit processes. Both files are provided in the release.

> **Note:** Be sure to use the ReShade version with Add-on support (the unsigned version from <https://reshade.me>). When you start your game, open the ReShade overlay (`Home` key by default) and check the `Add-ons` tab to see ShaderToggler's controls and settings.

---

## Creating & Managing Toggle Groups
1. Open the ReShade overlay and go to the `Add-ons` tab -> `Shader Toggler`.
2. Click **New** to create a toggle group. By default, its name is `Default` and toggle key is `Caps Lock`.
3. Click **Edit** to configure:
   - **Name:** Set a custom description for the group.
   - **Key shortcut:** Click the box and press any key combo (including `Ctrl`, `Alt`, and `Shift`).
   - **Is active at startup:** Check this box if you want the group to automatically be active when the game launches.
   - **Invert selection:** When checked, the group will hide all *other* shaders instead of the marked ones.
4. Each group can also be enabled or disabled directly by clicking the `[x]` checkbox next to its name.

### Master Toggle Key
In the settings, you can assign a **Master Toggle Key** or click **Toggle All Groups Now**:
- Pressing the master key will temporarily turn off all active groups and remember their states.
- Pressing it again will restore only the groups that were previously active.

---

## Marking Shaders (Hunting Mode)
1. Ensure the element you want to toggle (e.g. HUD, UI, shadows, fog) is visible on screen.
2. Click **Change Shaders** on the toggle group you wish to edit. The hunting overlay will appear in the top-left corner.
3. The add-on collects active shaders over a configurable number of frames (default 250) so you only browse shaders actually in use.
4. Use the hunting keys to navigate and mark shaders:
   - **Holding down** the previous/next keys will scroll continuously through shaders.
   - Hold **Ctrl** while pressing previous/next to quickly navigate through *only* the shaders already marked in the current group.
5. While stepping through shaders, the currently active shader will temporarily turn off in the scene so you can see what it renders.
6. Once you find the shader rendering the target element, press the **Mark/Unmark** key to add it to the group.
7. Click **Done** when finished, then click **Save all Toggle Groups** to save your changes to the INI file.

### Configurable Hunting Hotkeys (TKL / Laptop Support)
Under the **Hunting keyboard shortcuts** header in the settings, all 9 hunting actions can be bound to any key on your keyboard:
- **Pixel Shaders:** Previous (default `Numpad 1`), Next (default `Numpad 2`), Mark/Unmark (default `Numpad 3`)
- **Vertex Shaders:** Previous (default `Numpad 4`), Next (default `Numpad 5`), Mark/Unmark (default `Numpad 6`)
- **Compute Shaders:** Previous (default `Numpad 7`), Next (default `Numpad 8`), Mark/Unmark (default `Numpad 9`)
- Click **Reset Hunting Keys to Defaults** at any time to restore the default Numpad layout.

---

## Configuration Profiles
- By default, settings are saved to `ShaderToggler.ini` in the game executable's folder.
- **Game-Specific Profiles:** ShaderToggler also supports per-game INI files (`ShaderToggler.<ExecutableName>.ini`).
- The active profile is displayed in the UI, and a switch button allows toggling between the default and game-specific profile.

---

## Changelog

### v1.3.0
- **x86 (32-bit) Full Modern Parity:** Complete 32-bit build (`ShaderToggler.addon32` / `ShaderToggler.addon`) supporting all modern features, Large Address Aware (`/LARGEADDRESSAWARE`), and optimized for DirectX 9, DirectX 11, OpenGL, and Vulkan.
- **Configurable Hunting Keys (#41, #38, #20, #9):** Added full UI remapping for all 9 shader hunting actions, allowing full usage on laptops, 60%, 75%, and Tenkeyless (TKL) keyboards without a physical Numpad.
- **Continuous Key Repeat (#22):** Holding down Next or Previous shader keys now scrolls smoothly through shaders with autorepeat.
- **Master Toggle Shortcut (#43):** Added a global master hotkey and button to toggle all groups off or restore their previous active states with one key press.
- **Per-Game INI Profiles (#3):** Support for `ShaderToggler.<GameExeName>.ini` with seamless in-game profile switching.
- **Inverted Selection & UI Checkboxes (#23):** Added option to invert group selection and direct UI checkboxes to toggle groups with the mouse.
- **Compute Dispatch Event:** Registered `addon_event::dispatch` to ensure compute shaders are intercepted during dispatch.
- **Pipeline Unbind Fix:** Fixed pipeline state tracking when shaders are unbound (`handle == 0`) in D3D9/D3D11 to prevent false shader blocking.
- **Performance & Thread-Safety:** Replaced exclusive write locks with reader shared locks (`std::shared_lock`) on draw call lookups with fast-path zero-handle handling.
