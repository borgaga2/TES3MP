# TES3MP OpenMW 0.51 port checkpoint

The active compatibility branch is `work/openmw-051-compat`, targeting
`openmw-0.51-merge` through PR #1. Its engine baseline is still OpenMW 0.48.0.
Do not advertise this checkpoint as a completed 0.51 port.

The target upstream release is `openmw-0.51.0`, commit
`f4bec41444214a7903bebd178389ca22ca13f646` from `OpenMW/openmw`.

## Compatibility work

The multiplayer code now uses the baseline engine's scoped draw-state and AI
setting enums, vector-based movement/rotation, spell-vector iteration, active
spell accessors, and actor-aware spell removal. Spellbook replacement collects
IDs before mutating its vector. Active-spell snapshots carry their stacking,
caster, and timestamp metadata and exclude spellbook/equipment constant effects.

Summon synchronization uses the engine's effect-to-actor multimap, fills pending
actor IDs, avoids inserting duplicate actor IDs, and modifies the summoner's
actual active-spell store rather than a copy. AI travel and escort preserve the
received repeat flag. Map tooltip updates use the current marker structures.
Height fields use `osg::Referenced` ownership without unsupported physics-object
cloning. Early client shutdown only destroys multiplayer state after initialization.
The interpreter test fixture implements TES3MP's context-tracking interface.

The server registers shared configuration options before loading configuration,
parses CLI options first, retains configuration-manager changes, and provides
`--help` and `--version` without starting networking or requiring server scripts.
Ubuntu CI smoke-tests the help/version commands for both executables.

## Remaining integration

1. Merge the exact 0.51 release and reconcile engine changes with multiplayer
   hooks. A preview against the preceding compatibility head found 368 conflicts
   with default merge options, or 351 with `-Xignore-space-change`. These are
   previews, not completed or committed merges.
2. Rebuild both executables and the editor/tools on the new baseline, then check
   Ubuntu and macOS CI. Keep the engine version truthful until upstream code is
   integrated.
3. Audit active-spell synchronization against the changed effect lifecycle,
   including resolved magnitudes, effect indices/flags, resistance/reflection,
   stacked-spell identity, and saved-state timestamps. Compilation alone does not
   validate these gameplay semantics.
4. Run a two-client/server gameplay check covering connection, movement, cell
   changes, death/resurrection, inventory/equipment, spells, summons, and dynamic
   record overrides. This requires game content and a runnable graphics session.

## Validation

Local validation used GCC 13, a Debug build with warnings treated as errors,
pinned RakNet/MyGUI, and system Google Test 1.14. The client, server, and unit-test
executables compile and link; all 952 tests from 98 suites pass. All 29 multiplayer
translation units also pass focused compiler checks. Server help/version and
configuration precedence checks pass, including resource paths containing spaces
and CLI overrides. Client help/version startup checks pass. These checks do not cover live multiplayer gameplay
or macOS; the engine is still 0.48.0.
Build instructions and pinned networking/MyGUI dependencies are in
`.github/workflows/cmake.yml`. With unit tests enabled, run
`./openmw_test_suite` from the build directory; test startup with:

```sh
./tes3mp-server --help
./tes3mp-server --version --resources ./resources
./tes3mp --help
./tes3mp --version --resources ./resources
```
