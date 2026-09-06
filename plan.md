# otakuShell — Plan de projet

> **Document de référence du projet.** Ce fichier décrit le projet, les étapes
> à suivre et l'état d'avancement. Les étapes cochées `[x]` sont terminées et
> vérifiées par l'utilisateur. C'est le plan officiel : on le suit étape par étape.

---

## Description du projet

**otakuShell** est un shell desktop ultra-léger pour Hyprland/Wayland, écrit en
C++20. Il dessine des barres et des surfaces (navbar, edge, lockscreen…) en
rendu logiciel **cairo** dans des buffers `wl_shm` ARGB8888, sans aucun toolkit
GTK/Qt. L'architecture est **modulaire** : des *frames* (surfaces Wayland)
contiennent des *modules* isolés qui communiquent par **mémoire partagée**
(IPC `shm_open`/`mmap` lock-free).

### Positionnement
- Cible : **Hyprland / Wayland uniquement** (layer-shell, ext-session-lock).
- Léger : rendu logiciel cairo, dirty-rect, double buffering, pas de busy-loop.
- Modulaire : frames + modules isolés, IPC par mémoire partagée.
- Configurable : fichier **TOML** (`config.toml`), hot-reload.
- Deux binaires : `otakud` (daemon) et `otakushell` (CLI de contrôle / preview).

### CLI (otakushell)
```
otakushell preview              ouvrir une fenêtre simulant la disposition
otakushell reload               recharger la config à chaud
otakushell status               afficher les frames/modules actifs
otakushell module enable|disable <name>
otakushell exec <cmd>
```

### Config (`config.toml`)
Frames déclarées : `navbar-top`, `edge-right`, `navbar-bottom`, `lockscreen`.
Thème : fond `#1e1e2e`, texte `#cdd6f4`, accent `#89b4fa`, surfaced `#6c7086`.
Option `take_over` : otakud désactive un autre shell (waybar/eww/polybar…) au
démarrage via `hyprctl`.

---

## Stack technique

- **Langage** : C++20
- **Build** : CMake + Ninja
- **Rendu** : cairo (software), buffers `wl_shm` ARGB8888, dirty-rect
- **IPC** : `shm_open`/`mmap` lock-free (`src/shm.cpp`)
- **Daemon/system** : D-Bus natif pour les modules système
- **Protocoles Wayland** : `wlr-layer-shell` (vendored dans `protocol/`),
  `xdg-shell`, `xdg-output`, `ext-session-lock` (copiés)
- **TOML** : toml++ (système)
- **Dépendances** : wayland-client 1.26, cairo 1.18, pangocairo, dbus-1 1.16,
  tomlplusplus 3.4
- **Git** : `main` stable, branches `feat/*`, commits conventionnels,
  merge `--no-ff`

---

## Étapes

### Étape 0 — Squelette du projet  ✅ TERMINÉ
- [x] Repo git initialisé, `main`, structure CMake+Ninja buildable.
- [x] Binaire `otakud`, CLI `otakushell` (squelette avec commandes preview/
      reload/status/module/exec).
- [x] `config.toml` (frames, thème, take_over) + parseur TOML.
- [x] Stubs Wayland générés (sed `namespace`→`namespace_` pour C++).
- [x] `.gitignore` exclut `build/`.
- Commit initial : `feat/core-wayland`.

### Étape 1 — Noyau Wayland layer-shell  ✅ TERMINÉ
- [x] `display_connect` : bind globals + énumération des outputs.
- [x] `ISurface` + `LayerSurface` double-buffered `wl_shm` (ARGB8888).
- [x] Le daemon crée **une barre par frame** (navbar-top, edge-right,
      navbar-bottom), render fond opaque + radius, présente via layer-shell.
- [x] `display_wait()` poll bloquant (pas de busy-loop 100 % CPU).
- [x] **Take-over** : `src/hypr.cpp` (dry-run préserve, kill réel tue waybar).
- [x] Fix crash layer-shell (`x == 0 but anchor doesn't have left and right`)
      : anchors complètes par axe (top→TOP|LEFT|RIGHT, right→RIGHT|TOP|BOTTOM)
      + taille selon orientation. `Display::alive` + handler SIGSEGV/SIGABRT
      avec backtrace, `-rdynamic` sur otakud. Build clean sans warnings.
- [x] Fix crash à l'arrêt (Ctrl+C) : `ShmSurface::destroy(wayland_ok)` ne fait
      que `munmap` quand la connexion est morte — plus d'appel
      `wl_buffer_destroy`/`wl_surface_destroy` sur connexion fermée.
- [x] Validé par l'utilisateur (barres affichées + arrêt Ctrl+C propre).
- **Mergé : `merge: feat/core-wayland (step 1 ...)`** (`d5c6f8c`).

### Étape 2 — Preview autonome (saute le compositor)  ✅ TERMINÉ
- [x] Abstraire l'interface `ISurface` pour supporter un `ToplevelSurface`
      (fenêtre `xdg-toplevel`) en plus des `LayerSurface`.
- [x] Implémenter `otakushell preview [frame-id]` : instance standalone sur
      une fenêtre xdg-toplevel, données **mockées**, sans daemon ni barres
      ancrées — le même moteur de rendu et framework `Frame` est réutilisé.
- [x] `display_connect(need_layer_shell=false)` : le preview n'exige que
      xdg-shell (pas wlr-layer-shell), le daemon garde l'exigence layer-shell.
- [x] Fenêtre épinglée aux dimensions bar/edge (min=max size) via
      `xdg_toplevel_set_min_size/max_size` ; fermeture par l'utilisateur →
      callback `close` → sortie propre (code 0).
- [x] Validé par l'utilisateur (fenêtre affichée, fermeture propre, daemon
      intact).

### Étape 3 — Framework frames + config chaude
- [ ] Framework frames générique (chargement des frames depuis la config).
- [ ] Hot-reload de la config (`otakushell reload`).
- [ ] Application du thème (fond/texte/accent/surfaced).
- [ ] Auto-hide pour l'edge (edge-right).

### Étape 4 — Modules & IPC mémoire partagée
- [ ] Plateforme de modules isolés (processus séparés) communiquant par
      `shm_open`/`mmap` lock-free.
- [ ] `module.cpp` : factory de modules (modules de base : horloge, réseau,
      volume, système D-Bus…).
- [ ] Directive `otakushell module enable|disable <name>`.

### Étapes suivantes (à affiner)
- [ ] Lockscreen via `ext-session-lock` (frame `lockscreen`).
- [ ] Modules côté système (D-Bus natif).
- [ ] Tests, packaginf, docs.

---

## État d'avancement

| Étape | Statut  |
|-------|---------|
| 0 — Squelette            | ✅ Terminé |
| 1 — Noyau layer-shell    | ✅ Terminé (mergé dans `main`) |
| 2 — Preview autonome     | ✅ Terminé (branche `feat/preview-toplevel`) |
| 3 — Frames + hot-reload  | ⬜ À venir |
| 4 — Modules & IPC        | ⬜ À venir |
| Lockscreen + suite       | ⬜ À venir |

---

## Workflow

1. On travaille sur une branche `feat/*` par étape.
2. Tu testes sur ton Hyprland ; je fixe jusqu'à validation.
3. Validation → merge `--no-ff` dans `main`.
4. On coche l'étape dans ce fichier, on passe à la suivante.

## Remarques clés / pièges connus
- `wlr-layer-shell` n'est **pas** dans le paquet système → vendored dans
  `protocol/`.
- Les stubs Wayland nécessitent le patch `namespace`→`namespace_` (mot réservé
  C++).
- Ne jamais toucher aux proxys Wayland quand la connexion est morte
  (`Display::alive`) — crash via `wl_proxy_marshal`.
- Pas de compositor dans l'env de dev → l'Étape 2 (preview) rend le travail
  testable localement.
