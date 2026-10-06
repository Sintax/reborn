# Combat autopilot: what the live runs showed (6 Oct 2026)

Solo Dojo_P, `-rbautopilot -rbcombat`, heroes Rath (Class_DeathBlade, sword) and Oscar Mike
(Class_ModernSoldier, rifle). Runs `debugloop/runs/20261006-04*`/`-05*`/`-06*-live-combat-dojo*` and
`*-play-s0-solo-dojo-smoke`.

## Census (who is an enemy)

- `WorldInfo.PawnList` + `APoplarPawn::IsEnemy` works. The Dojo shows minions only:
  `Pawn_M1Gun_Dojo` and `Pawn_M1Blade_Dojo`, kind `minion` (no PRI), health 643 at spawn, in waves of 4.
- `Actor.FastTrace` called through ProcessEvent returned false for minions in plain view. Do not use
  it for line of sight. `visible` is now "drawn by the renderer in the last 0.3 s"
  (`WorldInfo.TimeSeconds - LastRenderTime`).

## Aim

- Writing `Controller.Rotation` (directly or with `ClientSetRotation`) does not stick:
  `APoplarPlayerController` overrides `UpdateRotation`, and the yaw was back at the same stored value
  every tick. Aim goes through `PlayerInput.aTurn` / `aLookUp`, written after the engine tick.
- The input's turn speed is not a constant: 12-22k rotator units per second at input 1 for yaw;
  look-up is inverted (about -11k to -31k). The brain learns both while turning (`turn_rates` in
  `/combat`) and steers proportionally to the live error. Measured: median yaw error 0.3 degrees on
  visible targets.
- An early bug: aiming with the census bearing (up to 0.25 s old, relative to the view at census time)
  made the view swing past the target every time. Aim must use the live position and live view.

## Firing and abilities

- The console command `StartFire` does nothing in Battleborn: the buttons go through the game's own
  input package (`GD_Input_Poplar.upk`), not `[Engine.PlayerInput]` exec bindings. Oscar Mike's
  magazine stayed at 30 through 13 console "shots". `APoplarPlayerController::StartFire(0)` /
  `StopFire(0)` fire for real (2 kills in 80 s). Note: the plain autopilot (combat off) still uses the
  console command, so the ladder runs before 6 Oct never actually shot.
- `StartActionSkillBySlot(ASS_SlotOne/Two/Three)` works; slot cooldowns read through
  `GetActionSkillSlotCooldownTimeRemaining`. Rath's skills killed minions in the first runs.
- `Weapon.WeaponRange` reads 16384 for Rath's sword, so the brain learns its reach instead: 2.5 s of
  firing at a visible target with no health drop shrinks it (to 60 % of that distance, at least 200);
  a hit near or past the reach grows it back. Rath settled near 500 units and killed minions.
- Unknown still: whether `melee` (`StartOffHandFire`) and `altfire` do anything for each hero, and
  which slot is each hero's ultimate (slot 3 is used when 2+ enemies are within 1500 units).
