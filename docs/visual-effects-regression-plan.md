# Visual-effects regression plan for 1.0.1.0

This plan compares the released `1.0.0.0` build (baseline) with the fixed `1.0.1.0` build. Use the same disposable save for both builds by reloading the save before each run. Do not save after using the quest-stage commands below.

## Confirmed failure mechanism

Release `1.0.0.0` installed two process-wide presentation hooks whenever its transition hooks were enabled:

- `ImageSpaceModifierInstanceForm::Apply` (vtable slot `0x26`) is replaced by a function that calls Skyrim's original only when the plugin hooks are **disabled**. During normal play, every form-backed IMOD instance remains in the manager but contributes nothing to the rendered image.
- `FaderMenu::AdvanceMovie` calls Skyrim's original and then forces the movie background alpha to zero and the movie invisible outside the special new-game state. This suppresses ordinary `Game.FadeOutGame` calls.
- `EndLoad` also queues an unconditional hide for an open `FaderMenu`, which can terminate the fade-in half of a scripted travel sequence.

IDA 1.7.99 confirms that slot `0x26` is `ImageSpaceModifierInstanceForm::Apply`. Its native body validates the instance/form, normalizes strength, and tail-calls the image-space accumulator. The plugin's replacement skips that body globally. This affects `Apply`, `ApplyCrossFade`, transformations, word walls, scripted black/white fades, and other IMAD-backed effects—not just load-cover effects. Base cell/weather image spaces and the separate DOF-instance subclass are not intercepted by this particular hook.

## Fast baseline tests

Wait until the mod's post-load transition has completely finished before entering each command.

### Direct native IMOD dispatch

At any safe location, open the console and run:

```text
imod 001037E6
rimod 001037E6
```

`001037E6` is `FXTimeTravelImodStatic04`. Expected vanilla/fixed behavior: a conspicuous full-screen time-travel treatment appears; `rimod` clears it. Expected `1.0.0.0` behavior: no visible effect.

Two additional vivid controls are:

```text
imod 00109BA7
rimod 00109BA7
imod 001093E6
rimod 001093E6
```

These are `DA09BloomIMOD` and `ReturnFromSovengardeImod`.

### Safe script-driven ApplyCrossFade

Use a disposable save, then run:

```text
coc ThroatOfTheWorldExterior
setstage MQ206 60
```

SSEEdit maps `MQ206` stage `60`, index `0`, to `QF_MQ206_00036193.Fragment_8`. That fragment contains only:

```text
FXTimeTravelImodStatic04.ApplyCrossFade(4.0)
```

Expected vanilla/fixed behavior: the time-wound image treatment crossfades in over four seconds. Expected `1.0.0.0` behavior: no visible effect. Reload the original save before repeating this test because a completed quest stage does not execute twice.

### Script-driven FadeOutGame

Run:

```text
coc WhiterunStables
```

Hire Bjorlam and take the carriage to any hold capital. `CarriageSystemScript.SkipToDestination` calls:

```text
Game.FadeOutGame(true, true, 0.5, 3.0)
FastTravel(...)
Game.FadeOutGame(false, true, 1.0, 2.0)
```

Expected vanilla/fixed behavior: a three-second fade to black before travel and a delayed two-second fade from black after arrival. Expected `1.0.0.0` behavior: the FaderMenu transition is invisible or prematurely closed.

Dawnguard's `FerrySystemScript` uses the same durations and is a second natural control at the Solitude, Windhelm, or Dawnstar ferryman.

### Word-wall script

On a save that has not learned the Bleak Falls word, run:

```text
coc BleakFallsBarrow02
```

Approach and learn the word. `WordWallTriggerScript` calls `wordIMODWordLearned.Apply(1)`. Expected vanilla/fixed behavior: the word-learning full-screen effect is visible. Expected `1.0.0.0` behavior: it is absent. Use a fresh save for the second build because the word can only be learned once.

## Useful cells

`coc` takes the EditorID in the third column. Base-game FormIDs are included for verification. DLC FormIDs use `xx` for that plugin's runtime load-order byte.

| Purpose | Cell FormID | `coc` EditorID | Relevant effect |
|---|---:|---|---|
| Carriage fade | `000165A0` | `WhiterunStables` | `FadeOutGame` out/in |
| Carriage/ferry control | `00070469` | `SolitudeStables` | `FadeOutGame` out/in |
| Carriage control | `00016774` | `WindhelmStables` | `FadeOutGame` out/in |
| Carriage control | `00018BE0` | `RiftenStables` | `FadeOutGame` out/in |
| Word wall | `000371DD` | `BleakFallsBarrow02` | `wordIMODWordLearned.Apply(1)` |
| Elder Scroll/time wound | `00009748` | `ThroatOfTheWorldExterior` | MQ206 crossfades |
| Mercer/Karliah sequence | `00025E24` | `SnowVeilSanctum02` | strike, black frame, wake-up IMODs |
| Meridia sequence | `0004624F` | `KilkreathRuins02` | bloom-to-white crossfade |
| Eye of Magnus route | `00091872` | `Labyrinthian03` | whiteout IMOD in MG06 scene |
| Vampirism cure | `000093BE` | `MorthalExterior01` | VC01 ritual blackout |
| Fort Dawnguard | `xx001DBA` | `DLC1DawnguardHQ01` | Dawnguard scripted effects |
| Volkihar Keep | `xx000803` | `DLC1VampireCastleGuildhall` | vampire transformation effects |
| Apocrypha/book transition | `xx03109D` | `DLC2Book01Dungeon` | book enter/exit IMOD chain |

Other verified nearby cells: `SnowVeilSanctum01` (`00015208`), `KilkreathRuins01` (`00015255`), `KilkreathRuins03` (`00027D1C`), `Labyrinthian01` (`00083559`), `DLC1VampireCastleDungeon01` (`xx00285B`), and `DLC1VampireCastleBossRoom` (`xx019DFE`).

## Quest and stage triggers

These mappings come from the vanilla QUST VMAD fragment tables in SSEEdit and the matching 1.6.1170 source scripts.

| Console trigger | Fragment | Native/script effect | Safety |
|---|---|---|---|
| `setstage MQ206 60` | `Fragment_8` | `FXTimeTravelImodStatic04.ApplyCrossFade(4.0)` | Preferred; one effect call only |
| `setstage MQ206 70` | `Fragment_12` | Static03 crossfade plus teaches/unlocks Dragonrend words | Disposable save only |
| `setstage MQ206 88` | `Fragment_69` | Static01 two-second crossfade | Quest setup may be incomplete |
| `setstage MQ206 89` | `Fragment_73` | Static03 two-second crossfade | Quest setup may be incomplete |
| `setstage MQ206 90` | `Fragment_23` | Final ten-second time-wound crossfade plus scene actions | Destructive/alias-dependent |
| `setstage MQ201 100` | `Fragment_15` | Fade to black for Diplomatic Immunity carriage transition | Removes all player inventory; avoid outside disposable save |
| `setstage TG05SP 10` | `Fragment_0` | Strike/fall IMOD, control lock, and player move | Requires TG05 scene aliases; destructive |
| `setstage TG05 60` | `Fragment_15` | Wake-up crossfade and player move | Requires TG05 setup; destructive |
| `setstage MQ305 260` | `Fragment_35` | Return-from-Sovngarde IMOD and player move | Requires MQ305 setup; destructive |

MQ101's long new-game fade is stage `10`, `Fragment_4`: `Game.FadeOutGame(false, true, 14.0, 15.0)`. Test it by starting a real new game, not with `setstage`, because its aliases, loading state, and title-sequence menu are part of the test. MQ101's one-second quick-start fade is startup stage `0`, fragment index `3` (`Fragment_27`) and cannot be selected independently with the console's `setstage` command.

## Before/after acceptance

Record the direct IMOD, MQ206 stage 60, carriage, word-wall, and real-new-game cases on `1.0.0.0`. A fix build passes only if:

1. All four ordinary IMOD/fader cases visibly return.
2. The custom loading transition still covers ordinary cell loads without flashing the intermediate scene.
3. The real-new-game Bethesda/title sequence still uses its native delayed fade from black.
4. `rimod` removes a directly applied modifier and later scripted modifiers continue to work, proving no stale instance was left behind.

Release `1.0.1.0` stops replacing `ImageSpaceModifierInstanceForm::Apply`, preserves script-owned FaderMenus, and closes only an ordinary fade-in claimed by the active load epoch. The retained-frame compositor still covers native effects while its loading layer is active, and MistMenu suppression remains load-specific. Release `1.0.0.0` remains the regression baseline.
