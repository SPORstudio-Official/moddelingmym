# Living Notes

**A local-first desktop notebook for ideas that keep growing.** Write without folders, find notes by text and topics, connect related ideas, and let old thoughts come back at the right time.

## Download for Windows

[**Download LivingNotes.exe**](https://github.com/SPORstudio-Official/moddelingmym/releases/latest/download/LivingNotes.exe)

The link downloads the standalone Windows app directly. No Python installation or account is required. (The first Windows build is published automatically by the GitHub Actions workflow after it completes.)

## What it does

- **Living notes:** every save keeps a previous title and body snapshot; browse history and restore an earlier version.
- **Branches and merges:** explore an alternate version of an idea or append another note while keeping the source intact.
- **Connected memory:** link notes and explore their relationships on the knowledge map.
- **Rediscovery:** choose when each note should resurface after it has gone untouched.
- **Decision records:** capture the decision, reasons, alternatives, expected outcome, and actual result.
- **Creative notebook:** keep character, setting/timeline, and research references with a creative note; use the main body for plot and fragments.
- **Search-first:** search titles, note text, topics, and other note details; browse by note type or resurfacing status without building a folder tree.
- **Knowledge timeline:** review when notes were created, changed, branched, and connected.
- **Own your data:** export notes, links, and version history to portable JSON at any time.

## Privacy and storage

Notes are stored on your computer in a SQLite database and are not sent to a server. On Windows the database is `%APPDATA%\LivingNotes\living-notes.db`. Export a JSON backup from **Export everything**.

## Run from source

Requires Python 3.10+ with Tkinter.

```bash
python living_notes.py
```

## Build the Windows executable

The repository's GitHub Actions workflow builds a single-file Windows `.exe` with PyInstaller on pushes to `main` and this development branch. It publishes the executable to the `living-notes-latest` GitHub Release. To build locally on Windows:

```powershell
python -m pip install pyinstaller
pyinstaller --noconfirm --clean --onefile --windowed --name LivingNotes living_notes.py
```

The executable will be at `dist\LivingNotes.exe`.
