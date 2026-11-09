# Versioning Rules

Format: `X.Y.Z` (non-negative integers; not SemVer).

- `X`: Change only when explicitly requested by the user.
- `Y`: +1 for added or improved functionality.
- `Z`: +1 for bug fixes.
- Increment each applicable component once per release batch, even if multiple changes qualify.
- Components are independent: never reset or carry values. Increment both `Y` and `Z` when both apply.
- No qualifying changes = no version change.
- Read the current project version first. Keep active version references consistent; preserve historical versions.

PinTray-specific release gate: keep `X = 0` during development. Change `X` to `1` only after the owner explicitly confirms that PinTray is ready to publish to Microsoft Store.
Current development version: `0.5.4`.
