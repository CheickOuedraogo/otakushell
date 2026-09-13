# otakuShell

Un shell desktop **ultra-léger** pour **Hyprland / Wayland**, écrit en **C++20**, conçu autour d'une architecture **entièrement modulaire** : les barres, les dashboards et le lockscreen ne sont rien de plus que des *frames* qui instancient des *modules* isolés, communiquant par **mémoire partagée**.

## Vision

Le shell n'est PAS un gros monolithe. C'est un **framework** léger + un ensemble de **modules** optionnels.

- **Frames** : des surfaces Wayland (`wlr-layer-shell`) qui servent de conteneurs — `navbar-top`, `navbar-bottom`, `edge` (dashboard auto-hide au survol, façon Celestia), `lockscreen` (session-lock).
- **Layouts** : chaque frame décrit ses positions (`left` / `center` / `right` / `top`) et quels modules y apparaissent — déclaré dans `config.toml`.
- **Modules** : des composants indépendants publiant leur état via une **bus mémoire partagée** lock-free (`workspace`, `clock`, `sysinfo`, `audio`, `brightness` — implémentés ; `wifi`, `bluetooth`, `systray`, `app-dock` — planifiés). Chaque module est un **processus séparé** (`otakud-mod`) : un crash ou un blocage n'impacte jamais le shell.
- Un module peut apparaître dans plusieurs frames. Les modules/frames non déclarés ne sont **jamais instanciés** → zéro coût.

```
                ┌──────────────────────────────────────────┐
                │              otakud (daemon)             │
                │   superviseur + boucle Wayland + input    │
                └──────────┬───────────────────────────────┘
                           │            frames (layer-shell)
              ┌────────────┼─────────────┬──────────────┐
       navbar-top    navbar-bottom    edge(dashboard)   lockscreen
              └────────────┼─────────────┴──────────────┘
                           │       layout positions → modules
              ┌────────────┴───────────────────────────────┐
              │      bus mémoire partagée (IPC shm)         │
              └─────────────────────────────────────────────┘
workspace · clock · sysinfo · audio · brightness
                      (produite par otakud-mod, processus séparés)
```

## Commandes

```
otakud               # lance le shell réel (frames ancrées sur le desktop)
otakushell preview   # fenêtre d'aperçu du layout (simulation, données mockées)
otakushell reload    # hot-reload du config.toml
otakushell status    # état des modules/frames actifs
otakushell module enable|disable <nom>
otakushell exec <cmd>
otakud-mod <module> [opts] [--simulate] [--once] [--daemonize]   # test d'un producteur seul
```

### `otakushell preview`

Le mode `preview` lance **exactement le même moteur de rendu, le même framework frames et les vrais producteurs** que le vrai shell, mais monté sur une **fenêtre `xdg-toplevel`** (déplaçable/fermable) au lieu des surfaces ancrées, avec les producteurs en mode `--simulate` (données simulées). Parfait pour itérer sur le design d'un layout sans rien affecter au desktop, et pour développer un module en isolation.

## choix techniques

- **C++20**, build **CMake + Ninja**.
- **Aucun toolkit** (ni Qt ni GTK) → rendu **logiciel** via `cairo`/`pangocairo` dans des buffers `wl_shm` (ARGB8888) avec **damage tracking** (`dirty_rect`) pour rester ultra-léger.
- **TOML** via [toml++](https://github.com/marzer/tomlplusplus) — config déclarative + hot-reload.
- **IPC** par **mémoire partagée** POSIX (`shm_open`/`mmap`) : chaque module est un processus séparé (`otakud-mod`, spawné/supervisé par otakud, argument `OTAKU_MOD_PATH`) qui publie dans un **ring-buffer SPSC lock-free** par module ; `IModule` ne fait que lire le dernier échantillon et le dessiner.
- **Exports systèmes** : les producteurs utilisent les outils du système à la volée — `wpctl` (WirePlumber) pour l'audio, `brightnessctl` pour la luminosité, `hyprctl` (socket Hyprland) pour les workspaces, `/proc` pour sysinfo ; le futur réseau/bluetooth passera par D-Bus natif (`libdbus`).
- Multi-moniteur, `exclusive-zone` sur les barres, auto-hide sur les edges.

## Dépendances

`wayland-client`, `wayland-scanner`, `wayland-protocols`, `cairo`, `pangocairo`, `dbus-1`, `tomlplusplus`, et le runtime Hyprland.

## Inspirations & crédits

otakuShell s'inspire ouvertement de projets formidables :

| Inspiration | Repo / Source | Ce qu'on en reprend |
|---|---|---|
| Daemon superviseur multi-process | [neur0map/ryoku-arch](https://github.com/neur0map/ryoku-arch) | Un daemon central supervise des surfaces/processus indépendants ; crash d'un module ≠ crash du shell |
| Frame + drawers / bar data-driven | [caelestia-dots/shell](https://github.com/caelestia-dots/shell) | Bar construite depuis une liste d'entrées config, drawers par edge (auto-hide) |
| Surface `wlr-layer-shell` | [swaywm/wlroots](https://github.com/swaywm/wlroots) | Exemples de surfaces layer-shell en client Wayland pur |
| Rendu logiciel shm + dirty-rect | [piny4man/tablero](https://github.com/piny4man/tablero) & [vedanshbodkhe21/sway-draw](https://github.com/vedanshbodkhe21/sway-draw) | Rendu CPU dans `wl_shm` ARGB8888, redraw minimal des zones modifiées |
| Mémoire partagée Wayland | [wayland-book.com](https://wayland-book.com/surfaces/shared-memory.html) | Buffers `wl_shm`, pool mémoire partagée partagée avec le compositor |
| Config TOML (header-only) | [marzer/tomlplusplus](https://github.com/marzer/tomlplusplus) | Parsing TOML — utilisée comme dépendance système |
| Ring-buffer SPSC lock-free | [CharlesFrasch/cppcon2023](https://github.com/CharlesFrasch/cppcon2023) | Logique du ring `Fifo4b` (cursors atomiques paginé) adaptée dans `src/shm.cpp` — licence **public domain** vendoredée dans `third_party/LICENSE-unlicense` |
| Barre data-driven multi-process | [yambar](https://codeberg.org/dnkl/yambar) (MIT) | Référence d'architecture pour la séparation producteurs / rendu et le parsing `/proc` (mirroir testé : `ralphptorres/yambar`) |

### Outils système sollicités au runtime

Les modules ne recodent pas les drivers système : ils invoquent les outils du desktop, déjà présents.

| Outil | Projet | Licence | Modules |
|---|---|---|---|
| `wpctl` | [WirePlumber](https://github.com/PipeWire/wireplumber) | Apache-2.0 | audio |
| `brightnessctl` | [brightnessctl](https://github.com/Hummer12007/brightnessctl) | MIT | brightness |
| `hyprctl` | [Hyprland](https://github.com/hyprwm/Hyprland) | BSD-3-Clause | workspace |

## Structure

```
otakushell/
├── protocol/            # protocoles Wayland (xml + code généré)
│   └── wlr-layer-shell-unstable-v1.xml   # vendored (absent du paquet wayland-protocols)
├── include/otaku/       # API publique (config, wayland, render, shm, frame, module, supervisor…)
├── src/                 # otakud (daemon), otakushell (CLI), otakud-mod (producteur de modules)
├── tests/               # tests unitaires (ring SPSC, ctest)
├── third_party/         # licences vendoredées (Unlicense — ring SPSC)
├── config.toml          # config utilisateur
└── CMakeLists.txt
```

## Statut

En développement actif — voir les branches `feat/*`.
