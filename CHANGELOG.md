# Changelog

ArtemisLM PS5 0.12.8, by Joel Smith-Lowe. The version in `ps5/app/version.hpp` is the
version on the dashboard. The install file is `artemislm-ps5-app-v0.12.8.zip`.
`contentVersion` is `01.012.008`. Install this by copying the `PPSA98010` folder
once. The copy already running reloads itself, so Settings does not finish the
change. After this version is open, a later update closes the app. Open it
again from the home screen.

This title starts from PS5LM 0.9.0 by cobanov. The notes below are the work in
this tree. PS5LM's own release history is not repeated here.

## [0.12.8] - 2026-10-19

### Fixed

- An update reloaded the app that was already running, so the version stayed
  the same. The new files are written into the folder the console launched,
  marked so the console can start them, and the app closes. Open ArtemisLM
  again from the home screen.
- A disk image cannot be updated in place. Two folder copies are left as they
  are until one remains.

## [0.12.7] - 2026-10-19

### Fixed

- Moving a loaded model into `/data/ArtemisLM/models` unloads it first, copies
  the file, checks that the copy finished, removes the old file, and loads the
  new path. The library shows that path.
- Download on the phone starts the file, shows progress, and saves a `.gguf`
  in the folder from Settings. The partial file and a leftover zip are removed
  after the save checks out.
- A model that fills the GPU can spill the extra layers into memory. Set Memory
  fit to Spill to RAM, then load it again. A plan with almost no spare does
  this on its own. A model that still cannot fit is refused. If the server
  runs out of memory, the page says so.
- The screen while a model loads says ArtemisLM.

### Changed

- Library, Projects, and Settings hide the animated background so the text
  stays readable. Chat keeps it.
- The build removes a leftover title folder next to `PPSA98010`. A release zip
  that contains another title, or `ps5lm.elf`, is refused.

## [0.12.6] - 2026-10-19

### Fixed

- An update that had already unpacked could stop with "The staged app has an
  unsafe path" and leave the old app running. The installer copies that staged
  title. A symlink, or a name containing `..`, is still refused.
- Install this release by copying the `PPSA98010` folder once. The copy already
  on the console still has the old check, so Settings will refuse this zip.
  Later releases can install from Settings.

### Changed

- The phone message box is one composer. Tools, Attach, and New sit with Send.
  Each tool has a short description, and the ones that are on show under the
  message. A tool line in the chat opens when you tap it.
- Chat creates a project. The Projects tab lists `/data/ArtemisLM/projects` and
  can preview a web page. The console stores and reviews those files. It does
  not start Node or Python.

## [0.12.5] - 2026-10-19

### Added

- With nothing loaded, the phone page lists the models already on the console
  and can load one. It also opens the library, or Settings to add a folder.
- Tools sits above the message box. Search, the clock, projects, files, notes,
  and the reference tables start on. Attach adds a picture or a text file. A
  picture is sent when the loaded model has an image projector. New clears the
  chat.
- Projects lists `/data/ArtemisLM/projects`, can create a folder, and can read
  a file. A web page can be previewed there. The console still does not start
  Node or Python.

### Fixed

- Asking the chat to code an app uses the project tools. A review that says
  this console cannot start Node or Python means the process is not launched.
  The files are still saved.

## [0.12.4] - 2026-10-19

### Fixed

- A saved `/data/ArtemisLM/app-args.txt` that still names port 47281 is started
  on 8081. That file is what makes llama-server log "couldn't bind HTTP server
  socket, hostname: 0.0.0.0, port: 47281". The first line of app.log names the
  version and both ports. A library port that is already taken says so.

## [0.12.3] - 2026-10-19

### Fixed

- The page and the chat API are on port 8081 again. The library is on port 8082.
  These are the ports a browser on this console could open. 47281 never accepted
  a connection, so the television said the page did not open, and loading a model
  waited and then stopped while the GPU figure stayed near 1%.
- The television checks the page on the console's own address as well as on
  loopback. A port that does not open names the errno on screen.

## [0.12.2] - 2026-10-19

### Fixed

- Loading a model closes the idle page while a browser can still be reading it.
  The page text now stays alive until that request finishes, so the restart can
  replace the process.
- A model thread that cannot start is reported on the television and in the
  log. The loading notice is cleared and the page on port 47281 opens, so a
  model can be chosen again. A boot thread that cannot start runs that scan on
  the main thread, and the splash does not wait for a thread that was never
  created.
- The library keeps a request body when its Content-Length header uses any
  capitalisation, and when the header is absent and the body already arrived.

## [0.12.1] - 2026-10-19

### Fixed

- Loading a model restarts the app. The new process was opening its ports while
  the previous one still held them, so the load notice appeared and then the
  server stopped. The restart now closes ports 47281 and 47282 first, and the
  new process waits until they are free.
- With no model loaded, the page on port 47281 is the address a browser on the
  same network can open. The television shows that address. Opening `/v1` in a
  browser goes to the page.

## [0.12.0] - 2026-10-18

### Added

- Reference tables in the app for unit conversion, calendar dates, constants,
  elements, HTTP statuses, and short text codes. Nothing is downloaded and
  nothing is written under `/data`. The library port answers `POST /reference`.
  The phone page and LM Studio can call them, and a matching question on the
  TV is given the figure.
- Home, Console, and Settings show the current setup more clearly. A setting
  that leaves the measured default is marked. The phone settings page uses
  the same groups, including load at launch, sounds, and the scratch warning.

## [0.11.0] - 2026-10-18

### Added

- LM Studio can use the console as an agent. The library port answers MCP at
  `/mcp`. A script for LM Studio is `scripts/lmstudio/artemis-mcp.mjs`. An
  OpenAI client still uses port 47281. One model stays on the GPU.
- `POST /v1/agent` asks the loaded model, and can write a short plan first.

## [0.10.0] - 2026-10-18

### Added

- Agent mode. Settings names a coordinator and a worker. The console keeps one
  model on the GPU. Plan writes a short plan, then the answer, on the loaded
  model. Hand off, from the page, restarts onto the worker after the plan. On
  the TV the loaded model writes both.
- Sampling can set a repeat penalty, a minimum probability, and a seed. They
  apply the next time a model loads.
- The page can send back 8, 16, or 32 earlier messages. A search can keep 3,
  5, or 8 results.

## [0.9.0] - 2026-10-18

### Added

- The console title, the phone page and the release name are ArtemisLM PS5.
  Updates come from `joelsmithlowe/ArtemisLM-PS5`. The home folder stays
  `/data/ArtemisLM`.
- Its own install, title PPSA98010, content id
  `UP9000-PPSA98010_00-ARTEMISLM0000001`. PS5LM and PS5LM Plus can stay
  installed. They keep using `/data/PS5LM/models`.
- A home folder at `/data/ArtemisLM`. The first launch creates that folder,
  `/data/ArtemisLM/models`, `/data/ArtemisLM/cache`, `/data/ArtemisLM/projects`, and
  `/data/ArtemisLM/memory`.
- The library always reads `/data/ArtemisLM/models`. It also reads
  `/data/PS5LM/models`, the same PS5LM folder on an M.2 or USB drive,
  `ArtemisLM/models` on those drives, and a loose `.gguf` in the USB root.
  Settings can add one more absolute folder under `/data` or `/mnt`.
- A model found outside the home folder can stay where it is, be moved when
  asked, or be moved on the next scan. A move copies the model, its projector
  and the other parts of a split file, then removes the old copy. The loaded
  model is left alone. One model can be deleted from the TV or the browser.
  The copy leaves 1 GiB free. A file already in the home folder at the same
  size is not copied again.
- Library, Hugging Face search and Settings on the phone and desktop page.
  That API is on port 47282. Chat stays on 47281. A download goes to the folder
  chosen in Settings, which defaults to `/data/ArtemisLM/models`. The page
  lists each installed model and what it can do.
- With no model loaded, port 47281 still serves that page. Library can
  download a model, and Settings can add a folder. Loading a model restarts
  the app onto that model, and the chat API comes back.
- Chat and the OpenAI and Anthropic APIs listen on port 47281. The library
  listens on 47282. The paths are the same. Port 47282 also lists models at
  `/v1/models`, describes the app at `/artemis`, and accepts `/load` and
  `/unload`. The page can load a model from Library. The page draws a paper
  plane on the phone or computer, and Home draws the same kind of ring while
  nothing is loaded. Neither one takes memory from the model.
- A calculator for a plain sum, on the TV and in the browser. A question that
  names two currencies also reads the European Central Bank rate.
- Web search also runs for stock, score, forecast, population and bitcoin,
  with weather, price, rate and the currency codes. A question that says
  research or scrape also reads the text and the links of one result.
- With file tools on, the browser chat can create a project in
  `/data/ArtemisLM/projects`, next to the models folder. A project is a Node
  server, a static page, or a Python program. The console stores the files
  and reviews them in this process. It cannot start Node or Python. The
  review has to pass before the reply calls the build ready. Copy the folder
  off with FTP and run it on a computer. One file inside a project can be
  deleted. The browser can list projects, scratch, models, USB, and the M.2.
- Short notes live in `/data/ArtemisLM/memory`. Each note is kept as an
  experimental hashed word vector, not loaded into the model. A later question
  gets a note only when the words are close. The rest of the chat is unchanged.
- The app checks GitHub at launch for a newer release of this repository.
  Settings, Sound and storage, App update, or the phone page, downloads that
  zip, checks its checksum, and installs it into `/data/homebrew/PPSA98010`,
  then restarts. Models and settings in `/data/ArtemisLM` stay. The first copy
  of the title folder still has to be placed there. A zip with a bad path or
  a bad checksum is refused.
- Settings on the TV and on the phone page for flash attention, context cap,
  cache type, prompt batch, slots, sampling, reply length, reasoning, the
  system prompt, and the library switches above.
- Credits on the console, and the app icon.

### Performance

- The model stays on the GPU. The launch flags stay `-ngl 999`, `-fit off`,
  `-lm none`, flash attention on, `-ctxcp 1` and `-cram 0`. That is the path
  measured with the 27B load. There is no layer slider, and the process does
  not lock memory or turn mmap off.
- Memory spare is now a setting. Measured keeps 0.75 GiB, the spare from that
  test, and it is the default. Tight keeps 0.45 GiB so a longer context can
  fit. Tight was not measured here, and a load can fail. Roomy keeps 1.10 GiB.
  None of the three moves a layer off the GPU.
- Prompt batch Safe is 256 tokens with a 64-token ubatch, the size a
  submission on this GPU finishes. Medium is 512/128 and Wide is 1024/256. A
  wider text batch can stall the device. Vision and decision models stay at
  2048, because an image or a decision prompt has to fit in one ubatch.
- The CPU uses the cores the console allows, minus two. Using every allowed
  core has shut the console down, so that count was left as it is.

### Fixed

- A question about today, a price or a rate calls the clock and then web
  search, on the TV and in the browser. A search that leaves the year out
  gets the console's date on the query.
- The Home system prompt is passed with `-sys` and applied when the request
  has no system message of its own. A client that already sent one keeps it.
- Source paths in the release binary are inside the project. The machine that
  built the package is not named in it.

### Unchanged

- A decision model can still stop answering after a long run of requests.
  Language models are not affected. Restart the app or load the model again.
