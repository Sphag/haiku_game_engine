# Source watching (US-025)

`asset_converter watch` converts a project once, then converts it again each time its sources change. Code is in `tools/asset_converter/` (`watch.hpp`, `watch.cpp`, `watch_linux.cpp`, `watch_win32.cpp`). The runtime does not use it, so FR-03 holds.

```text
asset_converter watch --project <dir> --output <dir> [--jobs <n>] [--report <file|->] [--max-passes <n>]
```

- Each pass prints the summary line and every diagnostic to stderr, the same as `cook`. `--report` rewrites the JSON report after each pass.
- `--max-passes <n>` stops after `n` passes, the first included. Without it, `watch` runs until the process is stopped.
- The exit code is that of the last pass: `0` when it has no errors, `1` when it does, and `2` for a usage problem or a watch that cannot start.
- Keep `--output` outside `--project`. When the output is inside the project, the existing scan reports each cooked file as an unregistered source (a warning) on every pass. Nothing loops, but the output is noisy.

## What starts a pass

1. The OS reports a change under the project. Windows uses `ReadDirectoryChangesW` over the whole tree. Linux uses inotify, with a watch on every directory, and adds watches for directories that appear later.
2. Notifications for paths under the output directory are dropped. They are the converter's own writes.
3. The debouncer waits for 300 ms without a notification, or 2 s after the first one, whichever comes first. A burst of saves becomes one pass.
4. The session takes a snapshot of every source file (relative path and SHA-256 of its bytes) and compares it with the snapshot of the last pass. A pass runs only when they differ.

Step 4 is the loop guard. A pass writes only into the output directory, so it cannot change its own inputs. A touch that leaves the bytes unchanged, an event for a file that was rewritten with the same bytes, and the converter's own writes all produce equal snapshots and start no pass.

## What a pass does

A pass is an ordinary `cook`. The import and cook caches (see [asset-converter.md](asset-converter.md)) reuse everything whose keys did not change, so only the changed assets and their dependents are converted again.

- A renamed source keeps its GUID when its sidecar moves with it, and the new path is converted.
- A deleted dependency is reported as a missing dependency. Its dependents fail with a diagnostic and are not converted again.
- A source that fails to import keeps its previous `<guid>.cooked`, so the runtime keeps the last working version. The failure is in the report and on stderr. Fixing the source starts another pass.
- A lost OS notification (an overflow, or a directory that was moved or deleted on Linux) is treated as a change, so the next pass checks the whole project.

## Replacing results at runtime

The converter writes each output with a rename over the old file, so a reader sees either the old bytes or the new ones. A consumer that holds a handle calls `AssetManager::reload(handle)` for each GUID that the pass converted, and then `apply_reloads()` at its frame boundary. See [runtime-assets.md](runtime-assets.md). A failed reload keeps the current payload and reports the reason through `reload_error()`.

On Windows, a rename over a file that a reader holds open can fail. The converter retries a few times, as `asset_converter` already does for every output. If every attempt fails, it reports `PUBLISH_FAILED`, the previous output stays, and the next change starts a pass.

## Tests

- `tests/src/converter_watch_tests.cpp` [unit]: the debouncer's quiet window and maximum wait, snapshot hashing and skipping, and the session driven by a scripted change source. The cases cover no-change notifications, one pass per change, output-directory notifications, rename, a deleted dependency, and a broken source followed by a repair. A missing output directory is a usage error.
- The same file [integration]: the real OS watch on a scratch project. It covers an edit with the output inside the project (one pass, then none from the converter's own writes), a source added in a new directory, and a rename.
- `tests/cli/`: `watch` without `--output` exits `2`, and `watch --max-passes 1` exits `0` with a valid report.

## Not in this story

- No editor or runtime process runs this loop yet. The editor hot-reload work (US-066) and the runtime's frame loop will consume the reports.
- No debounce or loop guard exists for a project whose sources keep changing faster than the pass takes. A pass always reads the latest bytes, so the work is not lost, but a constant stream of writes keeps the converter busy.
