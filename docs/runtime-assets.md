# Runtime asset handles (US-024)

Code is in `engine/assets/`. The runtime links this library and never the converter (FR-03): it reads cooked files only.

## Cooked files

- `cooked_container.hpp/.cpp`: the `OWCK` container that wraps every `<guid>.cooked` file (magic, `u32` version 1, `u64` payload size, payload). The converter writes it with `write_cooked_container`, and the runtime reads it with `read_cooked_container`. Both sides share one definition.
- `texture_format.hpp/.cpp`: the `OWTX` texture payload, moved here from the converter so the runtime can decode textures. `fallback_texture()` returns a valid 1x1 magenta texture.

## Handles

`asset_handle.hpp` provides `AssetManager<T>` and `AssetHandle<T>`.

- **States.** `LoadState::Loading`, `Ready`, `Failed`. `get()` returns the payload when Ready and the manager's fallback otherwise. `error()` names the reason for a Failed load. An empty handle is Failed with a null `get()`.
- **Requests.** `request(guid)` returns at once. A job reads `<cooked_root>/<guid>.cooked`, checks the container, and then calls the decoder. A duplicate request while a handle is alive shares the same entry and the same decode.
- **Reference lifetime.** The manager holds each entry weakly. Copying a handle shares the entry. When the last handle goes, a running load is cancelled, its result is dropped, and the payload is freed. The next request loads the asset again, which is how a reload works.
- **Failures.** A failed entry is not retried while any handle still refers to it. Release all handles to retry.
- **Publication.** The decoder returns a complete value or an error. The job publishes under the entry lock, and only if the load is still Loading and not cancelled. A partial payload never becomes visible.
- **Job ended without publishing.** If the JobSystem cancels or the job throws, the handle moves to Failed with the job's reason, so no handle waits forever.
- **World closure.** `close()` cancels every load that has not published, and later requests return failed handles without starting a job. Loaded payloads stay available. The destructor closes the manager.
- **Reloads (US-025).** `reload(handle)` decodes the cooked file again on a job. The current payload stays Ready and visible while it runs. A success is staged, and `commit_reload()` on the handle, or `apply_reloads()` on the manager, moves it in. Call the commit at a safe point such as the frame boundary. A failure keeps the current payload and sets `reload_error()`. A reload is refused while another is in flight, and `close()` cancels any reload in progress. `reloading()` is true while a job runs or a result waits for commit.

## Rules for users

- The JobSystem must outlive the manager's last request. Handles may outlive both.
- `request()` and `close()` run on one thread. Handles may be read and released from any thread.
- The decoder runs on a worker thread, so it must not touch main-thread state.

## Tests

- `tests/src/assets_cooked_tests.cpp` [unit]: container layout, round trip, and rejection of a bad header, magic, version, or size; the fallback texture is valid.
- `tests/src/assets_handle_tests.cpp` [integration]: loading and Ready states, missing files, duplicate requests, release and reload, format errors, a throwing decoder, consumer destruction mid-decode and while queued, world closure, a texture cooked by the converter loaded through a handle, and reloads (staged until commit, a failed rebuild keeping the working payload, a missing file, refusal while in flight, and close cancelling a reload).

## Not yet covered

- Model payloads (`OWMD`) still live in the converter, because they depend on glm and the mesh cooker. Scene and render stories will move them when the runtime first needs them.
- Resource packs (US-060) are in [runtime-packs.md](runtime-packs.md). File watching is in the converter (see [source-watch.md](source-watch.md)). Nothing in the runtime calls `reload` yet, because no frame loop or editor exists to call it.
- No world module exists yet, so world closure is the manager's `close()`.
