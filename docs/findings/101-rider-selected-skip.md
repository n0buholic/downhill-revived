# Early Rider Selected skip at high frame rates

2026-10-28. Cross/A pressed quickly after confirming a rider could flash a different pose,
then leave RIDER SELECTED unskippable until its animation ended. A delayed press worked.

The native input mapping is not the cause. Scripted Cross reproduces it at 240 FPS, while
the same input skips normally at 60 FPS. The shell latches the press in `D_0029DF74`, waits
for `D_0029DF70 >= 31`, then seeks the camera and rider animation to 98% and locks further
skip input with `D_0029DF48` (`func_001F1A90`).

Rider confirmation starts a type-7 animation blend of **500.1 ms** (`func_001F24B8`). When
that blend finishes, `func_00204F38` seeks back to its saved animation position. The skip
delay was still incremented once per update, so at 240 FPS the seek could happen while the
blend was active. The blend subsequently overwrote the seek, leaving the skip lock set.
That premature seek/reset also explains the transient pose/camera jump.

`port/patches_fps.py` now advances the shell's transition-age counter by `port_whole_step()`
at high rates, like other elapsed-field timers. Both writers (rider-selected and shop-flight)
use the same units. Early input remains latched, then executes after the original safe
delay; 60 FPS keeps the original increment. No matching source is changed.

Regression: `port/tools/test_rider_skip.py --exe EXE --iso ISO` drives early and late Cross
at 60 and 240 FPS with no saves or settings changes. On the previous installed binary,
60 FPS passed (41 fields early, 71 late); early 240 FPS failed at 573 fields, exceeding the
240-field completion budget. Separate RAM captures confirmed the skip lock remained set
while the camera animation offset had returned to zero.

After rebuilding the portable core and Linux host, all four cases pass: 60 FPS early/late
remain 41/71 fields; 240 FPS early/late complete in 53/255 fields. The late 240 case waits
240 fields before pressing. These are actual update counts; high-rate wall-clock pacing
adjusts elapsed time when rendering runs below the requested rate. The core's strict link,
signature checks and guest-address scan also pass. Controller input was scripted through
the existing pad harness; physical-controller timing and every rider model were not
separately tested.

Research: searched existing Downhill Domination skip documentation before changing code.
General walkthroughs describe X/Start movie skipping but do not document this shell timer.
The concrete evidence above is from the repository's matching shell/animation functions
and controlled live runs; no external code was copied.
