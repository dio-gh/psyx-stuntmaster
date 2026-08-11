# Native in-game license viewer — RE findings & plan (P5(B))

Goal (user requirement): custom in-game menu option(s) that display the embedded
third-party license texts **while the game runs**, rendered by the retail engine
(native), not a host overlay. Licenses are already embedded in the exe (P5(A))
and reachable via `stuntmaster.exe --licenses`.

Status: **investigation only, no guest-touching code yet.** All findings below
are read-only (symbols + disassembly of the resident boot EXE `SLUS_006.84`,
overlay 0). Addresses are guest virtual; re-extract symbols with
`stuntkit.py extract <cue> GAME_REL.SYM` + `psyq_sym.py` (both gitignored).

## What already works and is reusable (the host-menu bridge)

`game/retail_hle.cpp` (`--experimental-host-menu`) already injects an
`Options > Display` menu by repurposing the retail **Sound** menu (4 rows) and
**writes host-supplied strings that the retail font system renders** — that is
the crux: host text on screen, natively, already works today for short strings
("Frame Rate / 60 HZ", "Resolution / 1920x1080", "Widescreen / ON").

Retail menu-item struct offsets already in use (from the bridge + confirmed by
disassembly):
- `+0x00` next item (linked list)
- `+0x18` item hash (u32) — identity
- `+0x20` value text object (`xcTextObj*`)
- `+0x30` value index (u16)
- `+0x38` inline value-string buffer (host writes here via `writeText`)

`writeText(runtime, addr, sv)` just loads `sv`+NUL into guest RAM. It is length-
agnostic, so the **only** limit on host text is (a) the target buffer's size and
(b) the text object's on-screen box/clip. The resolution string (~10 bytes) is
the longest written today; the inline `+0x38` buffer size is **not yet known**
(writing a multi-KB license there would overflow into adjacent fields — unsafe
until sized).

## No cheap long-scroll shortcut: credits is an FMV

`_ShowCredits__9feMenuMgr` (0x80010A54) → `gsPlayMovieCredits__4Game`
(0x8002CB28): the credits roll is a **movie**, not a native scrolling-text
screen. So there is no existing long-text scroller to hijack. Long license text
must be rendered through the font system directly.

## The font system (native text rendering) — overlay 0, always resident

Source `C:\devsys\psx\xclib` (`XCFONT.CPP`, `XCFONTDC.CPP`, `XCDO.CPP`). All in
overlay 0 (the resident main EXE) — no overlay-swap gating.

Key functions:
- `xcFontDC::xcFontDC(xcTextObj&)` `__8xcFontDCR9xcTextObj` @0x800C4E20 — builds a
  font draw context from a text object.
- `xcFontDC::Draw()` `Draw__8xcFontDC` @0x800C4ED4
- `xcFontDC::MakePolys(POLY_FT4*, xcPolyHandleFT4*)` @0x800C505C — emits glyph
  `POLY_FT4` quads into the OT (one textured quad per glyph).
- `xcFontDC::PushJustTrans(short,short)` @0x800C4F44 — justify/translate.
- `xcFontDC::GetSize(short*)` @0x800C53D0, `GetWidthLine(long)` @0x800C5514 —
  **multi-line aware** (per-line width), so the renderer already understands
  multi-line strings.
- `xcFont` @0x800915A0, `FindLetter__C6xcFontUc` @0x80091C64 (glyph by char code),
  `ReloadData__6xcFont` @0x80091B20.
- `xcTextObj::sDraw` @0x800AE828, `xcTextObj::FindNamedData` @0x800AE798.

### xcTextObj layout (from the ctor disassembly @0x800C4E20)

The ctor copies these `xcTextObj` fields into the `xcFontDC`:

| xcTextObj offset | type | → xcFontDC | notes |
|---|---|---|---|
| `+0x01` | u8 | `+0x80` | style byte |
| `+0x02` | u8 | `+0x7C` | style byte |
| `+0x04` | 9×? | — | row-major 3×3 transform (scale/rot + X/Y translate at elems 2,5); see `copyValueTextStyle` |
| `+0x28` | u32 | `+0x78` | |
| `+0x2D` | u8 | — | **current frame index** |
| `+0x30` | u32 | `+0x70` | |
| `+0x34` | u32 | `+0x84` | passed to helper @0x800C55FC |
| `+0x38 + frame*4` | u32[] | `+0x74` | **array of data/text pointers by frame** (same array the bridge writes strings under) |

The ctor then calls a helper @0x800C55FC (sibling of GetWidthLine) and
memset-like @0x80036574 (a2=0x24=36 bytes) to init the DC's poly region.

## Open questions before building

1. **Where does the glyph string live, and how big can it be?** `+0x38[frame]`
   points at the string/data the DC renders. Need to confirm it is a plain
   NUL-terminated char array the host can repoint at a large host-filled buffer,
   and find a **safe guest RAM region** to hold multi-KB license text (or page it
   in small chunks into an existing bounded buffer).
2. **Box / clip / scroll.** Does the text object clip to a fixed rect? Is there a
   scroll offset field, or must the host repaginate and swap pages? `GetSize` +
   `GetWidthLine` suggest layout is measured, not clipped, but the on-screen
   window is unknown.
3. **Does a menu row render multi-line?** If writing a `\n`-containing page into
   an existing menu row's text object renders multiple lines within a large-enough
   box, the whole viewer can reuse the bridge with near-zero new RE (host just
   paginates the embedded text and cycles pages with left/right). This is the
   cheapest thing to test first (needs one live look via `--publication-dump`).

## DECISION (user, 2026-08-11): HYBRID — menu-triggered HOST overlay for now

Full native ownership of the HUD/GUI menu system (rendering long text via
`xcFontDC`) is deferred. Near-term: **menu options trigger a host-rendered
overlay** that is pageable/scrollable. This reuses the host's existing overlay
text pipeline (`presentation/debug_overlay.cpp`: a 5×7 font rasterized to an RGBA
bitmap, uploaded + blitted by the presenter, exactly like the debug/notification
overlays) — no guest font RE needed to render the text. The retail-menu bridge is
used only as the *trigger* (a new `HostMenuCommand`), not to render the body.

Host-overlay build pieces:
1. **Text layout (portable, testable):** word-wrap + uppercase-fold the embedded
   license text to N columns, paginate/scroll. Unknown glyphs already blank
   gracefully, so missing punctuation degrades, not crashes. Extend the 5×7 font
   with the high-value punctuation (`. , : ; ' " ( ) - /`). Lowercase glyphs are a
   later polish (uppercase-fold is the first cut).
2. **Presenter overlay (GL):** a `license` overlay render path mirroring
   `drawDebugOverlay` — rasterize the current page, upload, blit centered over the
   frame with a dim backdrop. A scroll offset (top line) drives which slice shows.
3. **Input:** host-side nav (Up/Down/PgUp/PgDn or pad) to scroll, a key/button to
   close; must not feed the guest while the viewer is open.
4. **Menu trigger:** repurpose the currently-blanked 4th Display-menu row as
   "Licenses" (and/or per-component), emitting a new `HostMenuCommand::show_licenses`
   through the existing sink → main loop → presenter activates the viewer.

## Pause-menu entry (user wants it here, not Display) — RE findings 2026-08-11

The host overlay (viewer) is done and validated via the 'L' key. The remaining
ask is a **navigable, selectable menu entry**. First attempt painted a 4th row
onto the shared Display screen (`Menu_Sound`), but that screen row is NOT a
member of the Display menu's item list (only 3 items), so the cursor can't reach
it. A real entry means injecting a real item into a menu's item list.

**The in-game pause menu is `xc/game_mnu.txt`** (separate from the front-end
`FE_MNU.TXT`), parsed by the same `ParseMenu` and hash-callback machinery:
```
MENU Menu_Pause Menu_Pause
  BUTTON Resume
  GOTO Sound Sound
  SHKSELECT Menu_Vibration
  GOTO Exit Exit
END MENU
```
- `SelfInit__8gameMenu` @0x80037D0C finds the menu by hash (`0xC26666F2` =
  "Menu_Pause") and binds item callbacks by ID hash via `SetCallback` @0x8005CEE8
  (e.g. `_ResumeGame` for hash `0xC6ABBE31`). Same pattern as `SelfInit__9feMenuMgr`.
- `AddItem__6hdMenu` @0x8005D0BC appends to the item list at `hdMenu+0x18`
  (list-append helper @0x80037510). Navigation is `InputPadUp/Down__8gameMenu`.
- **Injection strategy (mirrors HostDSP):** overlay `game_mnu.txt` (size-preserving,
  via `addCdFileOverride`, same as the FE_MNU.TXT override) to add a `Licenses`
  item to `Menu_Pause`, bind its callback to `host_menu_callback_address`
  (0x80002FF0) in a `Menu_Pause`-push pre-hook, and dispatch it to
  `HostMenuCommand::show_licenses`.

**OPEN PROBLEM — the new row needs a text object.** ParseMenu item directives are
constructors that bind to existing named text objects in the screen overlay
(`Menu_Pause`). HostDSP worked because `Menu_Sound` had a spare 4th row (Stereo)
to borrow. For a 5th pause row we need a spare text object in the `Menu_Pause`
screen (or another screen with capacity). **Next RE step:** determine how many
text objects the `Menu_Pause` screen has vs. the 4 it uses — if it has a spare,
this is straightforward (mirror HostDSP: reuse the spare row's objects + add the
item to the list); if not, we must source/relocate a text object, which is deeper.

**Reusable plumbing already in place (trigger-agnostic):** `HostMenuCommand::
show_licenses`, the main-loop atomic + `live_presenter->openLicenseViewer()`, and
the sink handler. Only the *emitter* (which menu item raises the event) is being
reworked from the Display-row painting to a real pause-menu item.

## Pause-menu item construction — VERIFIED recipe (2026-08-11)

External resources the user provided (kept OUTSIDE the repo, never committed):
`../ghidra-inferred-src/` (decompiled original PSX source — matches our target
binary) and `../ReChan/` (a separate, further-along **PC source port** that
reimplements the menu system wholesale via its own `feCustomMenuMgr`; its
approach doesn't port to us since we interpret the original binary, but its
PSX-accurate struct headers + ctor addresses confirm the layouts below).

Confirmed struct layouts (ReChan `hdmenu.h`/`hdmenuitems.h` + Ghidra):
- `hdMenuItem` (28B): `+0x00` ccMinNode(next,prev) · `+0x08` vtable · `+0x0C`
  data(=textObj) · `+0x10` callback `s32(*)(hdMenuItem*)` · `+0x14` flags ·
  `+0x18` itemID hash.
- `hdItemButton` (28B, ctor 0x8005E118, **vtable 0x800CD7A0**): `data(+0x0C)` =
  label `xcTextObj*`; `SelectItem` invokes `+0x10` callback. **This is our item.**
- `hdMenu` (36B): `+0x08` vtable · `+0x0C` menuID · `+0x10` curItem · `+0x14`
  menuColor · `+0x18` itemList (ccMinList). `AddItem` appends to itemList.
- `xcTextObj`: `+0x30` = `xcFont*` · `+0x38[frame]` = string ptr · `+0x2D` frame ·
  `+0x04` row-major 3x3 transform (X translate elem2 @+0x0C, **Y translate elem5
  @+0x18**) · style bytes +0x01/+0x02 (confirmed via `MakePolys` @0x800C505C:
  it reads chars from `+0x74`(=obj+0x38[frame]) and glyphs from `+0x70`(=obj+0x30)).

**Build recipe (host, in a Menu_Pause push pre-hook, once):**
1. Reserve a guest RAM region for {cloned xcTextObj, hdItemButton, "LICENSES\0"}.
   OPEN: pick a safe region — the host patch arena is near 0x80003000; confirm
   free space or use a fixed unused block. (Only remaining impl detail.)
2. Clone an existing pause item's `xcTextObj` (e.g. Exit's `data`/+0x0C) into the
   region — inherits a valid font (+0x30), style, and transform.
3. Repoint the clone's `+0x38[frame]` at the "LICENSES" host string; add one row
   height to its Y-translate (+0x18) so it sits below the last row.
4. Build the `hdItemButton` in the region: `+0x08`=0x800CD7A0, `+0x0C`=clone,
   `+0x10`=`host_menu_callback_address` (0x80002FF0), `+0x18`=a chosen itemID hash.
5. `AddItem` it to the pause menu (call `AddItem__6hdMenu` @0x8005D0BC via
   `beginInterruptCall`, or manually append to itemList at menu+0x18).
6. Guard so it is injected once per menu instance (the pre-hook runs every push).

Dispatch: the existing `host_menu_callback_address` boundary already routes to
`dispatchHostMenuCallback`; add a branch for the chosen itemID hash → emit
`HostMenuCommand::show_licenses` (plumbing already wired to `openLicenseViewer`).

**Status:** RE complete + verified. Implementation (guest-RAM construction +
AddItem + live positioning iteration) is the next phase; it is real guest-memory
code that needs careful implementation and live test cycles.

## Pause-menu live recon (2026-08-11) — the target and the wall

Live diagnostics (read-only, via the reliable `feMenuMgr::PushMenu` hook at
`host_menu_push_address`) resolved the actual in-game menu tree:
- **The in-game pause menu is front-end (`feMenuMgr`), not `gameMenu`.** My
  first hooks (Activate/Input on `gameMenu`, `game_mnu.txt`'s `Menu_Pause`) never
  fired — that menu is not what the pause opens. `game_mnu.txt`/`Menu_Pause`
  appears unused by this build's pause flow.
- **Pause root = "MAIN MENU" = `Menu_Title`, hash `0x062B99E2`.** Items (live):
  Resume Game (`hdItemButton`, vtable `0x800CD7A0`), Quit Game (`hdItemGoto`
  `0x800CD740`), Load Game (button), Save Game (button), Options (goto). Shown via
  `SetTopMenu`, not `PushMenu` (so it doesn't log on open; navigating into Options
  is what triggered the dump).
- Options = `Menu_GameOption` `0xC073AB79` (Controller Settings / Sound Options /
  Display). "Display" is the host's `HostDSP` (`0xB1AF7E45`) entry.
- Text prims are ~`0x3C` bytes; `hdItemButton` layout confirmed live.

**The wall: no spare prim anywhere.** Retail `FE_MNU.TXT` shows every screen
defines exactly the prims its items use — `Menu_Title` has 5, `Menu_GameOption`
has 3 (Controller/Sound/**CREDITS**). The host "Display" entry did NOT add a row;
it **repurposed the CREDITS button** (Credits→Display). So there is no spare row
to relabel for a new "Licenses" entry in MAIN MENU (or anywhere convenient).

**Consequence:** a natively-rendered new MAIN MENU row requires **creating a new
text prim and inserting it into the `Menu_Title` overlay's section/draw structure**
(`xcSection`/`FindNamedData`, packed overlay data) so it renders — on top of the
already-understood item build + `AddItem` splice. That overlay-section insertion
is the deep, multi-session xclib work; the item construction alone is not enough
because `hdMenu::Update` only recolors — the overlay draws the prims.

Options from here (decision pending with user):
1. **Full native** — implement prim creation + overlay-section insertion +
   positioning. True menu ownership; large, multi-session, high live-iteration.
2. **Hybrid** — a real (navigable/selectable) `hdItemButton` spliced into the
   MAIN MENU item list with a cloned-but-undrawn text object (valid for SetColour),
   while the HOST overlay draws the "LICENSES" label at the row position, synced to
   menu visibility/selection. Avoids section insertion; costs host↔guest sync and
   a font-style mismatch.
3. **Accept the working `L` hotkey** (+ a pad shortcut) and defer the menu row.

## ✅ DONE — full-native MAIN MENU "Licenses" row (implemented + user-validated, 2026-08-11)

A real, navigable, game-rendered **"Licenses"** row now sits in the pause MAIN
MENU (`Menu_Title`). Selecting it (X) fires the host callback → `show_licenses`
→ `openLicenseViewer()`. It renders through the retail font/overlay system, is
present the first time the pause menu opens, and is user-validated.

Implementation (all in `game/retail_hle.cpp`, `RetailHle::ensureLicensesMenuItem`
+ `injectLicensesOnSetTopMenu`):
- **Trigger:** hook `MenuMgr::SetTopMenu` (`set_top_menu_address` 0x8005F7DC, `$a0`
  = manager), which `SelfInit__9feMenuMgr` calls when the pause menu is built —
  so the row exists before first display. Also called from the `PushMenu` pre-hook
  as a belt-and-suspenders. Idempotent (skips if the row's item hash is present).
- **The overlay problem, solved by relocation:** the packed section leaves no
  slack after the title overlay's prim array (the next overlay begins immediately
  after it), so it cannot grow in place. The whole overlay (header + 6 prim
  entries) is copied into the reserved **menu-object arena** (`0x80004000`, see
  `R3000Runtime::menu_object_arena_base`) with one extra slot; a title text object
  is cloned there, its `+0x38[frame]` string repointed to a host `"Licenses"`
  buffer. The relocated overlay is referenced back by rewriting **both** the
  section overlay-inventory entry (`section+0x14`, drawn by `Draw__9xcSection`)
  **and** the owning `xcScreen` entry (`section+0x0C` → screen `[count@0][ovl@+4…]`,
  so `SetVisible` still toggles it with the menu).
- **Item + list:** an `hdItemButton` (vtable `0x800CD7A0`, `+0x0C`=clone,
  `+0x10`=`host_menu_callback_address` 0x80002FF0, `+0x18`=`licenses_item_hash`
  0x4C494345) is built in the arena and appended to the `ccMinList` tail
  (`menu+0x18` head / `+0x1C` tail). `dispatchHostMenuCallback` matches the hash
  and emits `show_licenses`. `SelectItem__12hdItemButton` invokes `+0x10` with
  `$a0`=item (verified by disasm at 0x8005E190), which the dispatch reads.
- **Layout:** six rows re-spaced evenly across the original top..bottom extent
  (fixed-size frame can't grow), nudged down ~2 px (`menu_rows_down_bias`) to
  sit centered. Extra spread was avoided — it pushes the bottom row onto the
  frame border. All row Y translations are `screenY<<16` at text object `+0x18`.

Live reference graph captured during recon (addresses are per-run dynamic):
`section=0x8012ADD4`, `overlay_inv=0x8012C918`, `screen_inv=0x8012C8D4`,
`target_overlay=0x8012D3A0` (6 prims), owning screen `scr[key=0x062B99E2]`.

The verified facts below remain the reference for the layouts/addresses used.

---

Original goal: a real, navigable, game-rendered **"LICENSES"** row in the pause
MAIN MENU that, when selected (X), fires the host callback → `show_licenses` →
opens the viewer.

Verified facts (do not re-derive):
- Pause MAIN MENU = `Menu_Title`, menuID hash **`0x062B99E2`** (found at runtime;
  its object address is dynamic, e.g. was `0x8012AE1C` one run). It is a
  **feMenuMgr** menu; `gameMenu`/`game_mnu.txt` is NOT the pause path.
- Reliable hook to reach it: `inspectHostMenuPush` (`host_menu_push_address`
  `0x80010D08`, feMenuMgr::PushMenu; gpr[4]=manager, gpr[5]=pushed menu). Fires on
  any submenu push. Find MAIN MENU by walking manager+0x30 (menu list; next at
  menu+0x00) matching menuID (+0x0C)==`0x062B99E2`. Inject ONCE (guard: FindItem
  for our chosen id; also survive menu rebuilds).
- `hdMenuItem` (28B): +0x00/04 ccMinNode(next,prev) · +0x08 vtable · +0x0C
  data(=textObj) · +0x10 callback `s32(*)(hdMenuItem*)` · +0x14 flags · +0x18 id.
  `hdItemButton` vtable **`0x800CD7A0`** (ctor 0x8005E118), SelectItem fires +0x10.
  (hdItemGoto vtable 0x800CD740, hdMenuItem base 0x800CD7D0, hdSndItemSelection
  0x800CD640.)
- `hdMenu`: menuID +0x0C, curItem +0x10, itemList ccMinList at +0x18 (head +0x18,
  tail +0x1C). `AddItem` = append to tail (manual: newItem.next=0, newItem.prev=
  oldTail, oldTail.next=newItem, list.tail=newItem). No guest call needed.
- `xcTextObj` (~`0x3C` bytes): +0x00 type byte (NOT a list link) · +0x01/+0x02
  style · +0x04 3x3 transform (X-translate @+0x0C, **Y-translate @+0x18**) · +0x2D
  frame · +0x30 `xcFont*` · +0x38[frame] string ptr. Confirmed by `MakePolys`
  @0x800C505C (reads chars from +0x38[frame], glyphs via +0x30) and ReChan headers.
- Reserved guest RAM: **`0x80004000`–`0x8000E000`** is free ("reserved by the
  kernel and never allocated by retail", `r3000_runtime.hpp`, between patch_arena
  end 0x80004000 and interrupt_stack 0x8000E000). Add a named `menu_object_arena`
  constant there for {cloned xcTextObj, hdItemButton, "LICENSES\0"}.

**THE OPEN PROBLEM (the whole reason this is "deep"):** `hdMenu::Update` only
recolors the selected item; the **overlay/screen draws the text prims**. There is
NO spare prim to reuse (retail FE_MNU.TXT: every screen defines exactly its
prims). So the cloned/created text prim must be inserted into `Menu_Title`'s
overlay draw structure or it will not render (the earlier Display-row failure was
exactly this — painted but not navigable; here it'd be navigable but invisible).

NEXT RE STEP (start here after compaction): understand how the overlay draws its
prims and how to insert a new one. Read decompiled `ghidra-inferred-src/FE/`:
`OXSCREEN.c`, `OXSCRMGR.c`, `OXOVL.c`, and `xclib/XCDO.c` / `XCSOS.c`. Key symbols:
`FindNamedData__9xcPrimObjP12xcSectionMan`, `Draw__9xcPrimObj` (0x800AE430),
`sDraw__9xcPrimObj` (0x800AE2A8), `XCon_DrawResetPrim`, `GetTextObj__9xcOverlay`,
`SetPrimPosA__FP9xcPrimObjss` (0x80090F6C — sets a prim's on-screen X/Y by type).
Determine: does the overlay walk a prim linked-list/array I can append a clone to,
or are prims baked into packed `xcSection` blobs (harder)? That answer decides
whether inserting a clone is a few writes or needs section surgery.

Then implement: clone an existing MAIN MENU text prim (e.g. "Options" @+0x0C) into
the menu-object arena, repoint +0x38 string → "LICENSES", set Y-translate (+0x18)
one row below the last, INSERT it into the overlay draw structure (the step above),
build an hdItemButton (vtable 0x800CD7A0, +0x0C=clone, +0x10=`host_menu_callback_
address` 0x80002FF0, +0x18=a chosen id hash) in the arena, append to MAIN MENU
itemList, and add a `dispatchHostMenuCallback` branch for that id → `show_licenses`.
Position/rendering will need several live `L`-key-style test cycles with the user.

Tools: disassemble with `tools/disassemble.py <exe> <va>` (needs `pip install
capstone`); extract the EXE via `stuntkit.py extract <cue> SLUS_006.84 <scratch>`
(never into the repo). Diagnostics that print to `std::cout` land in
`<Documents>\Stuntmaster\logs\stuntmaster.log` (use `std::endl` to flush). Build:
VS18 bundled cmake at `C:\Program Files (x86)\Microsoft Visual Studio\18\
BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\`, build dir
`build/agent-pres`, target `stuntmaster` (+ `stuntmaster_core_tests`).

## Incremental plan (cheapest-first; each step user-verifiable)

- **Step 0 (RE, done here):** map the bridge + font layout (this doc).
- **Step 1 (probe, low risk):** add a "Licenses" row to the injected Display menu
  and write a **multi-line** page (a few lines of real license text) into its
  value object. Capture via `--publication-dump` + a user live look to answer
  open-question 3 (does the row render multi-line, and how big is the box?).
- **Step 2a (if multi-line rows work):** host-paginate the embedded license text;
  left/right cycles pages, a second row cycles component. Pure host logic on top
  of the proven bridge — likely the whole feature, cheaply.
- **Step 2b (if rows are single-line/clipped):** escalate to a dedicated
  full-screen `xcTextObj` the host allocates/populates and draws via
  `xcFontDC::Draw` each frame, with a host scroll offset. This is the LARGE path
  and needs the open questions (buffer, box, OT integration) fully resolved.

Verification is `--publication-dump` (does the license screen render into a
captured frame?) plus a mandatory user live look for legibility/scroll.
