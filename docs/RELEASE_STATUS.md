# Release revision after audition — 8 September 2026

Luke requested these changes after testing the 7 September integration:

- Restore Compressor's original peak detector and 360 × 160 meter, with
  Threshold, Ratio, Attack and Release in one row beneath it. No detector switch.
- Remove Clack Learn and sieve-state text/ports. Retain its corrected gate,
  sieve protection and three reduction meters.
- Remove Fuzz's new match-mode, match-gain and Learn controls/ports. Retain
  the original voice and internal automatic normalization.
- Remove Shape completely. The collection is now seventeen plugins: sixteen
  ordinary Makefile bundles and separately built SuprNAM.

Band blend alignment, Forge, Echo/Space design, Phase, clean bass paths and
NAM/host latency fixes are retained. These removals are deliberate product
choices, not deferred work. Do not restore them without Luke's request.

## Validation and deployment

Local build and regression tests pass, and the revised full UI bundle builds.
The full native Pi suite also passed. The service is active, and live discovery
reports seventeen Supr plugins with Shape absent. Compressor/Clack/Fuzz report
their original port contracts without any removed controls. The freshly
captured post-reboot board restored exactly after stripping obsolete symbols.
No saved presets were modified.

The deployed browser confirms the 360 × 160 Compressor meter and four knobs
sharing one row. Learn, sieve status, Held Peak, Level Match and Match Gain
are absent. No browser or service errors were reported. Checksums of the
three revised installed bundles and every current staged UI file match.
Current main asset: `main-CfJUvzzC.js`, SHA-256
`f70a5db00bfce5f6c5398c71254c9d89c6ea31f918085db295e790f2edb0998d`.
Old unused web assets remain after a non-deleting sync; the installed index
references the new bundle. Removed source files and Shape's installed bundle
were archived outside active source/discovery directories for recovery.
The host and NAM binaries are unchanged from the tested integration.

Pi: `lukepi4@pi4.local`, verified using `HostKeyAlias=pi4` on this machine.
Browser: `http://pi4.local`. Hard-refresh after installation.
The current board is captured immediately before restart; only Shape and
removed control symbols are stripped during restoration, preserving every
other value and routing choice. Saved presets are not overwritten.

Private revision logs/backups: `~/supr-revision-after-audition/` on the Pi
and local ignored `build/release/`. Original integration backups remain at
`~/supr-release-20260907/backup/`. The previous release report is in Git history.
Restore the affected bundles/UI from the revision archive with the service
stopped if rollback is needed; do not overwrite newer preset data blindly.

Luke approved committing and pushing both repositories on 8 September 2026
after these requested removals. He also asked to stop further stability checks.
See [RELIABILITY.md](RELIABILITY.md) and [ROADMAP.md](ROADMAP.md) for continuity.

Companion UI commit: `92c26dbb0edf75b0229686b3fb059a3767e0af91`. The plugin commit containing this report is its matching revision.
