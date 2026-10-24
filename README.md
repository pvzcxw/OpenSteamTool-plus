<div align="center">
  <img src="docs/logo-animated.svg" width="180" alt="OpenSteamTool logo">

  <h1>OpenSteamTool</h1>

  <p>
    <strong>Open-Source Steam Unlock Tool</strong>
  </p>

  <p>
    <img src="https://img.shields.io/badge/C%2B%2B-20%2B-2ea44f?logo=cplusplus&logoColor=white" alt="C++ 20+">
    <img src="https://img.shields.io/badge/CMake-3.20%2B-2ea44f?logo=cmake&logoColor=white" alt="CMake 3.20+">
    <img src="https://img.shields.io/badge/Windows-only-d73a49?logo=windows&logoColor=white" alt="Windows only">
    <a href="https://deepwiki.com/OpenSteam001/OpenSteamTool">
      <img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki">
    </a>
  </p>

  <p>
    <a href="README.md">
      <img src="https://flagcdn.com/w40/us.png" width="22" alt="United States flag">
      English
    </a>
    &nbsp;|&nbsp;
    <a href="README_ES.md">
      <img src="https://flagcdn.com/w40/es.png" width="22" alt="Spain flag">
      Español
    </a>
    &nbsp;|&nbsp;
    <a href="README_ZH.md">
      <img src="https://flagcdn.com/w40/cn.png" width="22" alt="China flag">
      中文
    </a>
  </p>
</div>

## Feature

### Core Unlocks
- Unlock an unlimited number of unowned games.
- Unlock all DLCs for unowned games.
- Support auto load depot decryption keys from Lua config.
- Support auto manifest download via `opensteamtool` / `steamrun` / `wudrm` upstream APIs (default is `opensteamtool`), or a custom Lua endpoint (see [Manifest via Lua](#manifest-via-lua)).
- Support downloading protected games or DLCs that require an access token.
- Support binding manifest to prevent specific games from being updated.

### Hot Reload
- Adding, modifying, deleting, or overwriting `.lua` files in any watched directory automatically triggers a reload. No restart, no offline/online toggle needed.

### Injection
- Add optional game-process library injection through `[inject]` in `opensteamtool.toml`.
- Configure `enabled`, `library_x64`, and `library_x86`; the injected library must match the target process architecture.`library_x64` and `library_x86` may be absolute paths, or relative paths resolved from the Steam root directory.

### Family Sharing and Remote Play
- Bypass Steam Family Sharing restrictions for games that have been added to the library with `addappid` in Lua. All accounts in the Steam Family that participate in sharing must use OpenSteamTool for this to work.

### Compatible with games protected by Denuvo and SteamStub
- SteamStub-only games do not require configuring `AppTicket`. OpenSteamTool can reuse Steam's local ConfigStore ticket and forge the requested AppId through a SteamDRMP off-by-four ticket parsing vulnerability, without injecting into the game process.
- Denuvo-protected games still require explicit ticket data. OpenSteamTool stores `AppTicket` and `ETicket` through the platform credential store.
- Use `setAppTicket(appid, "hex")` and `setETicket(appid, "hex")` in Lua config to write these values to the platform credential store automatically.
- Denuvo verification has a 30-minute validity window. After this window expires, authorization may fail with Denuvo error code `88500005`; refresh the ticket data before retrying.
- AppTicket priority: explicit tickets have the highest priority, including tickets configured by `setAppTicket` and existing cached `AppTicket` credential values. If no explicit AppTicket is available, OpenSteamTool falls back to the forged local ConfigStore ticket path.
- SteamID priority: read cached `SteamID` first; if missing, parse from explicit `AppTicket`. On Windows, the credential store backend currently uses `HKCU\Software\Valve\Steam\Apps\<AppId>`. The Linux backend is not implemented yet.

#### Extracting tickets with `extract_tickets`

The `extract_tickets` tool dumps the `AppTicket` and `ETicket` hex strings you need for `setAppTicket` / `setETicket`. Run it on a machine where Steam is running and logged into an account that **owns** the target game.

1. Build the tools (see [Build](#build)); the binary lands in `build/tools/Release/extract_tickets.exe`.
2. Run it with the target AppId (or run it with no argument and type the AppId when prompted):
   ```powershell
   extract_tickets.exe 1361510
   ```
3. It reads the Steam install path from the registry, loads `steamclient64.dll`, and writes everything into an `<appid>/` folder next to the executable:
   - `appticket.bin` — raw app ownership ticket (binary)
   - `eticket.bin` — raw encrypted app ticket (binary)
   - `tickets.txt` — plain-text summary with the hex strings:
     ```
     appid:1361510
     appticket(184 bytes):14000000...
     eticket(143 bytes):...
     ```
     A ticket that could not be obtained is reported as `appticket:null` / `eticket:null`.
4. Paste the hex strings from `tickets.txt` into your Lua config:
   ```lua
   setAppTicket(1361510, "14000000...")
   setETicket(1361510, "...")
   ```

> **Note:** Tickets are only valid when extracted from an account that **genuinely owns** the game.

### Stats and Achievements
- Enable stats and achievements for unowned games.
- Uses `setStat(appid, "steamid")` to configure which SteamID's achievement data to pull.
- If no `setStat` is configured for an app, OpenSteamTool queries `https://stats.opensteamtool.com/{appid}` when `[stats] enable_api = true` (default).
- Priority: `setStat` > stats API when enabled and valid > hardcoded preset SteamID `76561198028121353`.

### Online Fix
- Add `-onlinefix` to the Steam launch parameters to enable 480-based online play in games that use lobby matchmaking. The current limitation is that only one such game can run at a time.To revert, simply remove -onlinefix from the launch parameters — online play returns to normal on the next launch.

## What did the fork do?
This fork has fixed numerous minor issues, including patterns and manifests, as well as the fixes of Denuvo.

## Future
- Steam Cloud synchronization support.(This is a huge project)

## Usage
1. Run `build.bat` from the project root to build the project.
2. Copy generated `dwmapi.dll`, `xinput1_4.dll` and `OpenSteamTool.dll` to the Steam root directory.
3. Create Lua directory (for example `C:\steam\config\lua`) and place Lua scripts there. The DLL will automatically load and execute them.
4. Lua example:
```lua
addappid(1361510) -- unlock game with appid 1361510

addappid(1361511, 0,"5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0") -- unlock game with appid 1361511 depotKey is "5954562e7f5260400040a818bc29b60b335bb690066ff767e20d145a3b6b4af0" 

addtoken(1361510,"2764735786934684318") -- add access token ("2764735786934684318") for game with appid 1361510 
-- No Longer Supported:
--pinApp(1361510) -- pin game with appid 1361510 to prevent it from being updated

setManifestid(1361511,"5656605350306673283") -- pin depotid:1361511 manifest_gid:5656605350306673283, size defaults to 0
setManifestid(1361511,"5656605350306673283", 12345678) -- same but with explicit size

setAppTicket(1361510,"0100000000000000...") -- write AppTicket to the credential store; on Windows: HKCU\Software\Valve\Steam\Apps\1361510\AppTicket

setETicket(1361510,"0100000000000000...") -- write ETicket to the credential store; on Windows: HKCU\Software\Valve\Steam\Apps\1361510\ETicket

setStat(1361510, "76561197960287930") -- use the specified SteamID's achievement data for appid 1361510
-- If not configured, the stats API is used when enabled; otherwise default SteamID 76561198028121353 is used.
```

All function names are **case-insensitive**. `setAppTicket`, `setappticket`, `SetAppticket`, `SETAPPTICKET` etc. are all equivalent. The same applies to every registered function (`addAppId`, `AddToken`, `SETManifestid`, etc.).

### Configuration (optional)

Rename `opensteamtool.example.toml` to `opensteamtool.toml` and place it in the Steam root directory (next to `steam.exe`).
If no config file is found, built-in defaults are used — no auto-creation.
The file is watched while Steam is running; valid changes are hot-reloaded without restarting Steam.

```toml
# opensteamtool.toml — Archivo de configuración de OpenSteamTool  
# Ubicación de almacenamiento: <Steam>/opensteamtool.toml  
# Este archivo se carga al iniciar y se recarga automáticamente en caliente tras modificaciones.

[log]
Nivel de detalle de todos los archivos de registro (solo aplicable a la compilación de depuración).  
Valores opcionales: trace, debug, info, warn, error
level = "debug"

[manifest]
# Consultar la API superior utilizada por el código de solicitud de lista de almacén.
#   "20770407"     → https://20770407.xyz/manifest/{depotId}/{gid}
#   "SDM"          → https://steamapi.993499094.xyz/manifest/{depotId}/{gid}
#   "manifestdex" → https://manifest.manifestdex.com/{gid} (el agente de usuario ManifestDeX se añadirá automáticamente)  
#   "opensteamtool" → https://manifest.opensteamtool.com/{gid} (por defecto)  
#   "wudrm"         → http://gmrc.wudrm.com/manifest/{gid} (recomendado para usuarios chinos)
#   "steamrun"      → https://manifest.steam.run/api/manifest/{gid}
Si en <Steam>/config/lua/manifest.lua se define fetch_manifest_code(gid) o
fetch_manifest_code_ex(app_id, depot_id, gid), estas funciones Lua tendrán prioridad sobre la configuración de URL que se indica a continuación.
#
# --- manifest.lua referencia ---
# El lado C++ proporciona a los scripts Lua las funciones http_get(url, [headers]) y
# http_post(url, body, [headers]). Ambas devuelven (body_string, status_code) en caso de éxito, y (nil, error_msg) en caso de fallo.
#
# -- Ejemplo: intenta usar wudrm (texto puro uint64), si falla retrocede a steamrun (JSON).  
# -- Devuelve una cadena de números para evitar pérdidas de precisión en doble precisión superiores a 2^53.
function fetch_manifest_code(gid)
-- wudrm responde por ejemplo "10570517747114638659"
local body, st = http_get("http://gmrc.wudrm.com/manifest/" .. gid)
if st == 200 and body then return body end
-- steamrun responde {"contenido":"1666836470726104466"}
body, st = http_get("https://manifest.steam.run/api/manifest/" .. gid)
if st == 200 and body then
local code = body:match('"content":"(%d+)"')
if code then return code end
end
return nil
end
#
# -- Versión extendida con app_id y depot_id:
# function fetch_manifest_code_ex(app_id, depot_id, gid)
#     local body, st = http_get("https://your-api.com/manifest-code/" .. app_id .. "/" .. gid)
#     if st == 200 and body and body:match("^%d+$") then
#         return body
#     end
#     return nil
# end
url = "opensteamtool"

# manifest Tiempo de espera HTTP solicitado (milisegundos).  
# timeout_resolve_ms — Resolución DNS        (por defecto: 5000)  
# timeout_connect_ms — Gestión TCP          (por defecto: 5000)  
# timeout_send_ms     — Transmisión de solicitud         (por defecto: 10000)  
# timeout_recv_ms     — Descarga del cuerpo de respuesta         (por defecto: 10000)
timeout_resolve_ms = 5000
timeout_connect_ms = 5000
timeout_send_ms    = 10000
timeout_recv_ms    = 10000

[stats]
# Cuando no se ha configurado la función Lua setStat(appid, "steamid"), se consulta automáticamente https://stats.opensteamtool.com/{appid} para obtener el SteamID recomendado.  
# Prioridad: cuando está activada y funcional, setStat > API de estadísticas > SteamID predeterminado fijo.
enable_api = true

# Directorio de configuración adicional de Lua (opcional).  
# Los archivos se cargarán después de la carpeta predeterminada <Steam>/config/lua.  
# La carpeta predeterminada siempre se carga por último, por lo que los archivos personalizados del usuario tienen prioridad.  
# Ejemplo: cargar un directorio personalizado en otro disco:
#   [lua]
#   paths = ["D:/my-steam-config/lua"]
[lua]
# paths = []

[inject]
# Inyección opcional de bibliotecas en el proceso del juego.  
# Las bibliotecas inyectadas deben coincidir con la arquitectura del proceso objetivo.
enabled = false
# library_x64 = "OpenSteamTool.GameHook.x64.dll"
# library_x86 = "OpenSteamTool.GameHook.x86.dll"

[cloud]
# Función opcional de redirección de almacenamiento en la nube Steam, compatible con juegos no bloqueados ("lua"), proporcionada por CloudRedirect (https://github.com/Selectively11/CloudRedirect).  
# Una vez activada, OpenSteamTool carga cloud_redirect.dll dentro de Steam, registrando cada juego añadido mediante addappid() como una aplicación de redirección y enrutando las solicitudes RPC de nube Steam a través del motor de almacenamiento en la nube de CloudRedirect.
#
# Inicio de sesión del proveedor (Google Drive / OneDrive / carpeta local) aún debe pasar por la aplicación complementaria integrada de CloudRedirect; OpenSteamTool solo aloja DLL.
enabled = false
# La ruta de cloud_redirect.dll. Puede ser una ruta absoluta o relativa al directorio raíz de Steam.
# Si no se especifica, el valor predeterminado es "<Steam>/cloud_redirect.dll".
# library = "cloud_redirect.dll"

[remote]
# Imagen de metadatos opcional. Si no se especifica, se utilizará GitHub y jsDelivr como alternativa de respaldo.
# La imagen personalizada reemplazará la fuente remota predeterminada y debe incluir tres marcadores de posición: {channel}, {component} y {sha256}.
#
# url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
# url_template = "https://fast.jsdelivr.net/gh/OpenSteam001/steam-monitor@{channel}/{component}/{sha256}.toml"
```

### Manifest via Lua

Two manifest code functions are supported:

#### `fetch_manifest_code(gid)`

Basic function that receives only the manifest GID.

#### `fetch_manifest_code_ex(app_id, depot_id, gid)` *(recommended)*

Extended function that receives `app_id`, `depot_id`, and `gid`. Allows constructing API endpoints that require app identification.

The C++ runtime provides two Lua helpers:

| Function    | Signature                          | Returns             |
| ----------- | ---------------------------------- | ------------------- |
| `http_get`  | `http_get(url [, headers])`        | `body, status_code` |
| `http_post` | `http_post(url, body [, headers])` | `body, status_code` |

`headers` is an optional table: `{["Key"]="Value", ...}`.

### Steam version compatibility

OpenSteamTool no longer ships byte-pattern signatures inside the DLL. Instead, on each launch it computes the SHA-256 of `steamclient64.dll` and `steamui.dll` on disk and looks up a matching pattern file from the upstream tracker at [`OpenSteam001/steam-monitor`](https://github.com/OpenSteam001/steam-monitor) (`pattern` branch).

Lookup order (every launch):

1. **GitHub raw** — `https://raw.githubusercontent.com/OpenSteam001/steam-monitor/pattern/...`. Canonical source.
2. **jsDelivr CDN** — automatic fallback if GitHub raw is unreachable (connection refused / timeout / 5xx). No configuration required. Useful in regions where `raw.githubusercontent.com` is blocked but jsDelivr is reachable (e.g. mainland China).
3. **Local cache** — `<Steam>\opensteamtool\pattern\<subdir>\<sha256>.toml`. Used **only** when remote is unreachable. The cache is overwritten after every successful remote fetch.

Remote is consulted on every launch so users automatically pick up upstream re-publications (e.g. the bot adding a new signature, or fixing an existing one) without having to clear any cache.

If a step returns **HTTP 404** the mirror loop stops immediately — all mirrors serve the same content, so a 404 means the upstream bot has not yet published a TOML for this Steam build. The code then falls back to the local cache if one exists; otherwise a one-shot popup appears with the unmatched DLL name, its SHA-256, the expected cache path, and the upstream URL. Only the hooks tied to that DLL are disabled — the rest of OpenSteamTool keeps working.

You can also drop a pattern TOML into the cache directory manually if you know the layout for a given build; the file name must be `<sha256>.toml`. The cache fallback will pick it up the next time remote is unreachable.

> A short outbound HTTPS request is performed at every launch (one per DLL: `steamclient64.dll`, `steamui.dll`). The downloaded bodies are tiny (~10 KB each) and the work runs on a worker thread, so it never blocks Steam's loader.

#### Using a different mirror

For most users, the built-in **GitHub -> jsDelivr** fallback is enough. To use a private mirror or intranet server, configure a full URL template. A custom mirror replaces the built-in remote sources; local cache fallback remains available.

The template must include `{channel}`, `{component}`, and `{sha256}`. Channels currently used are `pattern` and `ipc`.

```toml
[remote]
url_template = "https://your.server/{channel}/{component}/{sha256}.toml"
# url_template = "https://fast.jsdelivr.net/gh/OpenSteam001/steam-monitor@{channel}/{component}/{sha256}.toml"
```

### Debug logging

Debug builds write per-module log files under `<Steam>/opensteamtool/`:

| File                | Source                | Content                                                                      |
| ------------------- | --------------------- | ---------------------------------------------------------------------------- |
| `main.log`          | General               | Init, config loading, Lua parsing, utilities                                 |
| `ipc.log`           | `LOG_IPC_*`           | IPC commands, InterfaceCall dispatch, spoofing                               |
| `netpacket.log`     | `LOG_NETPACKET_*`     | Network packet send/recv, eMsg dispatch                                      |
| `manifest.log`      | `LOG_MANIFEST_*`      | Manifest download, `fetch_manifest_code`, manifest binding                   |
| `decryptionkey.log` | `LOG_DECRYPTIONKEY_*` | Depot decryption key injection                                               |
| `keyvalue.log`      | `LOG_KEYVALUE_*`      | KeyValues patching (manifest binding)                                        |
| `misc.log`          | `LOG_MISC_*`          | Engine pointer capture, AppId hints                                          |
| `achievement.log`   | `LOG_ACHIEVEMENT_*`   | UserStats requests/responses, steamid spoofing                               |
| `pics.log`          | `LOG_PICS_*`          | PICS access token injection                                                  |
| `package.log`       | `LOG_PACKAGE_*`       | Package injection, FileWatcher events                                        |
| `onlinefix.log`     | `LOG_ONLINEFIX_*`     | Online fix (480 AppId spoofing)                                              |
| `richpresence.log`  | `LOG_RICHPRESENCE_*`  | Rich Presence packet construction and injection                              |
| `steamui.log`       | `LOG_STEAMUI_*`       | SteamUI hook diagnostics                                                     |
| `pipe.log`          | `LOG_PIPE_*`          | Pipe handshakes, process inspection, Denuvo authorization, library injection |
| `platform.log`      | `LOG_PLATFORM_*`      | Platform helper diagnostics, including remote-process operations             |

The log level is controlled by `[log] level` in `opensteamtool.toml`.

## Build

### Requirements
- Windows 10/11
- CMake 3.20+
- Visual Studio 2022 with MSVC (x64 toolchain)

### Runtime requirements
- Outbound HTTPS access to `raw.githubusercontent.com` on first launch after a Steam update (see [Steam version compatibility](#steam-version-compatibility)). Cached afterwards.

### Quick build
```powershell
build.bat
```

### Output
- Debug: `build/Debug/OpenSteamTool.dll`, `build/Debug/dwmapi.dll`, `build/Debug/xinput1_4.dll`
- Release: `build/Release/OpenSteamTool.dll`, `build/Release/dwmapi.dll`, `build/Release/xinput1_4.dll`

## Disclaimer
This project is provided for research and educational purposes only. You are responsible for complying with local laws, platform terms of service, and software licenses.
