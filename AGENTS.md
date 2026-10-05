# Repository boundaries

Keep scripts, diagnostic probes, IDA queries, recordings, research notes and other
agent helpers outside this repository. Local support files live at
`J:\dev\Projects\SkyrimLoadProgress-support`.

Before changing transition rendering, read that directory's
`docs/transition-regression-checklist.md`; it records the accepted CS baseline
and the pending no-CS runtime checks. Build/test helpers target this repository
but write diagnostics to the external support directory.

Retain only mod source, assets, build configuration, product documentation and
this essential instruction file here. Do not move vendored dependency scripts.
