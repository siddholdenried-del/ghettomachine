# One-way instrument release sync

GhettoMachine owns shared instrument source. BongDrums and BongSynth own browser
bridges, saved-format compatibility and UI. The workflow exports only its fixed
GPL/MIT allowlist from a committed SHA on public firmware main, then optionally
proposes downstream pull requests. Local firmware files, radio, hardware and
bundled binaries are never copied. It cannot flash a device.

Start in dry-run mode. Merge compatible downstream bridges first. Verify that
the approved unified source (including both catalogs' level tables) is on public
GhettoMachine main. Then configure a short-lived GitHub App credential or a
fine-grained token restricted to Contents and Pull requests writes on only
BongDrums/BongSynth; set BONG_ENGINE_SYNC_TOKEN and opt in with
BONG_ENGINE_SYNC_ENABLED=true. Do not use a broad personal token. Each sync PR
must pass the instrument's independent build and tests before merge.

Downstream main packages the tested browser files and exact corresponding Git
source. Production publishing still belongs in the canonical Audio Tools
Worker repository, with a separate deployment credential used only there.
Promote one serialized, tested Worker release without replacing other apps,
verify the live paths and exact manifest hashes, and keep the prior version
available for rollback.
