# Install noteChad for the current user

Build an unsigned native Windows Installer package containing exactly `bin/noteChad.exe`. No notes, samples, Python runtime or developer tools are installed.

## Requirements

- Windows x64; a normal, non-elevated terminal.
- To build: the repository's `build.cmd` prerequisites and **64-bit CPython 3.11 or 3.12 with `msilib`**. Python 3.13+ removed `msilib`. No third-party packaging downloads are needed; `msilib` uses Windows Installer and the native cabinet API.
- Commands below use `python` after confirming it is a supported interpreter. If needed, replace `python` with `py -3.11` or the full path to an installed supported Python. Do not alter another application's Python environment.

## Rebuild and verify

Run in the checkout root using Command Prompt. Close noteChad before a subsequent install or upgrade.

```bat
python -c "import sys,struct,msilib; print(sys.version); assert struct.calcsize('P') == 8"
call build.cmd
if errorlevel 1 exit /b 1
python installer\build.py --version 0.1.1
if errorlevel 1 exit /b 1
python -m unittest discover -s tests -p "test_installer*.py" -v
if errorlevel 1 exit /b 1
python installer\verify.py --report bin\noteChad-Setup-x64.verify.json
```

The generated files are already covered by the repository's ignored `/bin/` directory:

| File | Purpose |
| --- | --- |
| `bin/noteChad-Setup-x64.msi` | Self-contained unsigned installer |
| `bin/noteChad-Setup-x64.msi.json` | Version, product/upgrade GUIDs and SHA-256 receipt |
| `bin/noteChad-Setup-x64.verify.json` | Read-back MSI tables and extracted payload hash evidence |

The verifier reads the MSI database without executing installer actions, extracts its embedded cabinet with `SetupIterateCabinetW` into a unique scratch directory, compares the extracted executable with both the receipt and `bin/noteChad.exe`, then removes the scratch directory. It never registers the product or launches the editor. This is cabinet extraction, **not** a live install or an `msiexec /a` administrative installation. Keep the receipt alongside the MSI; hashes detect mismatch, not publisher authenticity.

## Install only when authorized

Use a normal Command Prompt. The `/qb` switch provides standard Windows Installer progress without a custom wizard. No administrator prompt is expected. **If policy blocks installation or any elevation prompt appears, cancel; do not elevate.**

```bat
msiexec.exe /i "%CD%\bin\noteChad-Setup-x64.msi" /qb /norestart /L*v "%CD%\bin\noteChad-install.log"
```

Expected effects:

- Executable: `%LOCALAPPDATA%\Programs\noteChad\noteChad.exe`.
- Current-user Start menu: `%APPDATA%\Microsoft\Windows\Start Menu\Programs\noteChad.lnk`.
- Windows Installed Apps / Programs and Features: `noteChad`, with the standard MSI uninstall action.
- HKCU-only `Software\Classes\Applications\noteChad.exe` registration supplies the quoted Open With command and `.md` / `.txt` supported types. It does **not** write extension defaults, `UserChoice`, PATH, services or machine registry entries. Explorer's picker may require **Choose another app** / browsing to the installed EXE; visibility is shell-version-dependent and is not live-qualified here. Leave **Always use this app** unchecked to keep existing defaults.
- Notes remain where they are. The MSI has no note-directory access, wildcard deletion or app-launch action.

### Verify an authorized live install

Run in PowerShell from the same checkout after installation. These checks are read-only. A failed assertion is not success; examine the MSI log before proceeding.

```powershell
$receipt = Get-Content .\bin\noteChad-Setup-x64.msi.json -Raw | ConvertFrom-Json
$exe = Join-Path $env:LOCALAPPDATA 'Programs\noteChad\noteChad.exe'
if ((Get-FileHash $exe -Algorithm SHA256).Hash -ne $receipt.payload_sha256) { throw 'Installed EXE hash mismatch' }
$link = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\noteChad.lnk'
if (!(Test-Path $link)) { throw 'Missing Start menu shortcut' }
$shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($link)
if ($shortcut.TargetPath -ne $exe) { throw 'Wrong shortcut target' }
$wi = New-Object -ComObject WindowsInstaller.Installer
if ($wi.ProductState($receipt.product_code) -ne 5 -or $wi.ProductInfo($receipt.product_code,'InstalledProductName') -ne 'noteChad' -or $wi.ProductInfo($receipt.product_code,'VersionString') -ne $receipt.version) { throw 'Windows Installer product registration mismatch' }
$open = (Get-Item 'HKCU:\Software\Classes\Applications\noteChad.exe\shell\open\command').GetValue('')
if ($open -ne ('"' + $exe + '" "%1"')) { throw 'Wrong Open With command' }
'Installed executable, shortcut, MSI registration and Open With command verified.'
```

The editor can then be opened from the Start menu. Opening a test note and checking shell picker behavior is a separate authorized application/UI acceptance step.

## Upgrades and uninstall

The upgrade family is fixed: `{9A209231-AFB0-4614-AC71-DC87D9A96687}`. Product codes are stable for a given version and change for each new version; package codes are regenerated on every build. Increment `--version` for **every release with changed executable bytes**, including a new UI build if the prior version has already been installed. MSI compares three fields, within `255.255.65535`. Rebuilding the same version is for pre-install qualification, not an update mechanism.

Install the higher-version MSI with the same install command. The package detects older products in this current-user upgrade family and schedules their removal inside the installation transaction before new files are installed. It detects newer versions before launch conditions and refuses downgrades. The 0.1.0 → 0.1.1 upgrade was verified on a current-user installation. No separate uninstall is needed to correct the spelling in Start. Rollback, downgrade and standalone uninstall have not been exercised live.

Uninstall through Windows Installed Apps, or use the exact ProductCode from the build receipt. From PowerShell, after authorization:

```powershell
$receipt = Get-Content .\bin\noteChad-Setup-x64.msi.json -Raw | ConvertFrom-Json
$log = Join-Path $PWD 'bin\noteChad-uninstall.log'
$p = Start-Process msiexec.exe -ArgumentList @('/x', $receipt.product_code, '/qb', '/norestart', '/L*v', ('"' + $log + '"')) -Wait -PassThru
if ($p.ExitCode -notin @(0,3010)) { throw "Uninstall failed: $($p.ExitCode)" }
$exe = Join-Path $env:LOCALAPPDATA 'Programs\noteChad\noteChad.exe'
$link = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\noteChad.lnk'
$wi = New-Object -ComObject WindowsInstaller.Installer
if ((Test-Path $exe) -or (Test-Path $link) -or $wi.ProductState($receipt.product_code) -eq 5) { throw 'Uninstall left owned product resources' }
'Uninstall verified.'
```

Use the receipt for the **currently installed version**, not a newly built, not-yet-installed version. MSI removes its executable, shortcut and owned registry values. It leaves unrelated/user-created files intact, including notes; nonempty installation directories may remain. Never delete a note folder to make uninstall look clean.

## Troubleshooting and qualification limits

- **Unsigned package:** no Authenticode publisher signature is supplied. Windows may warn or organizational policy may block it. Do not bypass policy or run as administrator. The Installed Apps publisher label is metadata, not a verified signature.
- **Missing `msilib`:** use an existing supported x64 CPython 3.11/3.12. Do not run a random downloaded setup program or modify Hermes.
- **Installer exit code:** `0` is success; `3010` means success/restart requested, not proof a restart is necessary for this package. Other codes require inspecting `bin/noteChad-install.log`. `1618` means another install is active; wait rather than terminate it. `1638` often indicates a same-version product/rebuild conflict; bump the release version, or uninstall only after explicit approval.
- **Payload verification fails:** rebuild after `build.cmd` finishes, keep the MSI/JSON pair together, and rerun verification against that exact binary. Do not reuse a fixture MSI for a changed binary.
- **Extraction tool:** `expand.exe` on the qualification host copied the cabinet rather than extracting its member; verification uses Windows SetupAPI instead. No downloaded fallback is used.
- **Scope:** automated qualification covers construction, table inspection and actual cabinet hash extraction only. No production install, live registry changes, disposable identity creation, full ICE validation, UI install, live upgrade/rollback or uninstall has been executed by these scripts. Machine policy and Explorer picker behavior remain live acceptance gates.
