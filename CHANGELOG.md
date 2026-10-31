# Changelog

## Unreleased

### Kunimitsu II â€” back-turned attacks and combo collision

- Restore Kunimitsu's own BT 1 animation for both stance-side variants, retaining its frame-15 hit timing and BT 1,2 / BT 1,3 follow-ups.
- Redirect retained native BT 1 input routes to those restored attacks. Keep BT 2 and 1+2 input routes unchanged.
- Extend BT 1's collision coverage downward with an invisible point approximately 350 mm below her hand at impact, while retaining the existing arm collision points.
- Extend the kick in BT 1,3 approximately 100 mm forward at its first active frame (approximately 93 mm at the following frame).
- Apply the collision points to the default costume and all nine installed alternate outfits. Preserve existing bones and visible model geometry, textures, animations, hit timing, damage and Bound rules.
- Install the collision adjustment through a targeted update: four moveset bytes plus collision skeleton/spring metadata and the pack manifest. Back up affected files first; do not run the full importer, rebuild models or regenerate shared assets.
- Preserve the adjustments in the character definition for future imports. Collision points use the shared converter on Windows and Linux; Linux gameplay has not been retested for this adjustment.

Validation (2026-10-26): BT 1's restored animation was confirmed working in-game by the user. The collision update passed 24 relevant unit tests, geometry checks across all ten outfits, and an installation/idempotence check. In-game verification of the adjusted combo remains pending: d/f+2 -> b+4~b -> BT 1,3 -> SET.

Outstanding: Kunimitsu's open-hand weapon grip is a separate issue and is not fixed by these changes.
