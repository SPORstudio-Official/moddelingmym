# Code Reviewer (Android)

A standalone Android productivity app. Paste code, and it reviews it for errors and
gives a recommendation for every finding.

- **No generative AI.** Every finding comes from fixed, deterministic rules
  (bracket matching, string/comment lexing, language-specific checks such as
  Python 2 `print`, missing `;` in Java/C, `eval`, hard-coded passwords, unsafe C
  functions, invalid JSON, and so on).
- **Offline.** The app declares no network permission.
- **Languages:** JavaScript/TypeScript, Python, Java, C, JSON, HTML (auto-detect or pick one).
- **Productivity extras:** score out of 100, severity filters, tap an issue to jump to its
  line, copy a text report, and your draft is auto-saved between launches.

## Layout

```
android-app/
  app/src/main/assets/analyzer.js   rule engine (plain JS, also runs in Node for tests)
  app/src/main/assets/index.html    UI rendered inside the app's WebView
  tests/analyzer.test.js            unit tests for the rules
  smali/                            Dalvik source for the single Activity
  build/classes.dex                 classes.dex assembled from smali/ (smali 2.3.4 via apktool 2.4.1)
  build/dex-project/                minimal apktool project used only to run the smali assembler
  build/build_apk.py                builds + signs the APK (no Android SDK needed)
  release/CodeReviewer.apk          built app
  release/index.html                download page that starts the APK download automatically
```

## Run the tests

```
node --test android-app/tests/analyzer.test.js
```

## Build the APK

```
python3 android-app/build/build_apk.py
```

The build script writes the binary manifest, uses `build/classes.dex` (one Activity hosting a
WebView that loads `assets/index.html`), packages the web assets, and signs the APK with both a
JAR (v1) signature, for Android 5-6, and an APK Signature Scheme v2 block, for Android 7+.
The signing key is created in `android-app/signing/` on first build and is git-ignored. Builds
made with a new key cannot update an installed build signed with an older key; uninstall first.

### Regenerating classes.dex

`smali/` is the source. To assemble it, copy it into the dex project and run apktool's smali
assembler (apktool's later resource step fails in this setup, which is expected; the dex is
written before that step):

```
cp -r android-app/smali android-app/build/dex-project/
java -jar apktool_2.4.1.jar b android-app/build/dex-project
cp android-app/build/dex-project/build/apk/classes.dex android-app/build/classes.dex
```

`apktool_2.4.1.jar` is available from the npm package `apktool-jar`. It is not committed because
it is 17 MB. If `build/classes.dex` is missing, the build script falls back to a generator that
writes equivalent DEX directly.

## Install

Open `release/CodeReviewer.apk` on an Android phone (Android 5.0 or later), or open
`release/index.html` in the phone's browser, which downloads it automatically. You may need to
allow installs from your browser ("Install unknown apps").

## Limits

The rules are heuristics: they catch common mistakes and risky patterns, but they are not a
compiler or a full linter. Use them as a second check alongside your compiler, tests, and IDE.
