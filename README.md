# λ Harness C3

A low-cost USB companion display for the
[Harness](https://github.com/autonomous-ai/openharness) daemon, ported from
the reference ESP32-S3 dial to an **ESP32-C3-MINI-1U + GC9A01 240×240 round
panel**. Plug it into the computer running the daemon and it shows, at a
glance, what each coding agent is doing — working, waiting for an answer,
done — and lets you read and answer agent questions from the device.

*Section française plus bas.* 🇫🇷

## What it does

- Round 240×240 dark UI: a status ring per agent (grey idle / blue running /
  amber waiting / green done / red error), agent name, engine + machine, last
  summary line, and a badge with the account-wide fleet total.
- Two buttons: **A short** = next agent (sends `focus`), **A long** =
  `agent.open`, **B** = back / dismiss.
- Question screens: when an agent stops to ask (AskUserQuestion), the dial
  beeps, shows the options, and your pick goes back as a verbatim `answer`.
- Speaks the upstream **cable** protocol (binary framing + JSON vocabulary)
  over the ESP32-C3's native USB — no Wi-Fi, no account, no pairing.

## Hardware

| Item | Value |
|---|---|
| MCU | ESP32-C3-MINI-1U (native USB-Serial-JTAG on GPIO18/19) |
| Display | GC9A01 round 240×240 IPS, SPI write-only, RGB565 |
| Input | 2 push buttons, active-low |
| Optional | passive buzzer (GPIO1, `-1` in menuconfig disables) |

### Wiring (default pin map, all overridable in menuconfig)

| Signal | GPIO | Signal | GPIO |
|---|---|---|---|
| SPI SCK | 4 | LCD BL | 6 |
| SPI MOSI (SDA) | 5 | BTN_A | 7 |
| LCD CS | 2 | BTN_B | 8 |
| LCD DC | 3 | Buzzer | 1 |
| LCD RST | 10 | USB D− / D+ | 18 / 19 (fixed) |

## Flashing

**Web flasher (easiest):** open
`https://silexemple.github.io/harness-c3/webflasher/` in Chrome/Edge, click
*Connect & Flash*. Requires WebSerial (HTTPS or localhost).

> [!NOTE]
> **One-time setup for the Pages site:** repo *Settings → Pages → Deploy from
> branch → `main` / `docs`*. One click; CI keeps `docs/webflasher/bin` and the
> manifest up to date on every push to `main`.

**esptool:** grab `harness-c3-merged.bin` from
[Releases](https://github.com/Silexemple/harness-c3/releases):

```sh
esptool.py --chip esp32c3 write_flash 0x0 harness-c3-merged.bin
```

**From source (ESP-IDF v5.5):**

```sh
cd firmware
idf.py set-target esp32c3
idf.py build flash monitor
```

## Development

- `firmware/` — ESP-IDF project (target `esp32c3`, dual-OTA 4 MB).
- `firmware/test/host/` — protocol tests that run on your laptop, no device:
  ```sh
  bash firmware/test/host/run_tests.sh
  ```
  Runs every shared framing vector (`test/vectors/cable_frame.txt`, regenerated
  byte-identical by `scripts/gen_cable_vectors.py --check`) plus golden JSON
  tests of every message the firmware emits or parses.
- `PROTOCOL.md` — the normative cable protocol spec (extracted from upstream).
- `SPEC.md` — this port's architecture and acceptance criteria.

## v1 limits

- No voice (no mic), no touch scrollpad, no machine wheel / swarms / model
  picker — the carousel covers the active tab only.
- **`fw.offer` from the daemon is never answered** (anti-brick, SPEC §9):
  upstream images target ESP32-S3. Dual-OTA partitions and rollback plumbing
  are in place for a future C3-aware updater; until then, update via the web
  flasher or esptool.

## Credits

Protocol implementation, framing code, test vectors and the protocol
specification are adapted from
[autonomous-ai/openharness](https://github.com/autonomous-ai/openharness)
(MIT) — see `LICENSE` for the full attribution.

---

# λ Harness C3 — en français

## C'est quoi ?

Un petit écran rond USB compagnon pour le daemon
[Harness](https://github.com/autonomous-ai/openharness) : un
**ESP32-C3-MINI-1U + écran rond GC9A01 240×240**. Branché sur l'ordinateur qui
fait tourner le daemon, il montre d'un coup d'œil ce que fait chaque agent de
code — en cours, en attente d'une réponse, terminé — et permet de lire et de
répondre aux questions des agents directement depuis l'appareil.

- Anneau de statut coloré par agent (gris inactif / bleu en cours / ambre en
  attente / vert terminé / rouge erreur), nom de l'agent, moteur + machine,
  dernière ligne de résumé, badge avec le total de la flotte.
- **Bouton A court** = agent suivant, **A long** = ouvrir l'agent, **B** =
  retour / annuler.
- Quand un agent pose une question, l'appareil bippe, affiche les options, et
  votre choix repart tel quel au daemon.
- Protocole **cable** d'origine (trames binaires + vocabulaire JSON) sur l'USB
  natif du C3 — pas de Wi-Fi, pas de compte, pas d'appairage.

## Matériel requis

- ESP32-C3-MINI-1U (USB natif sur GPIO18/19).
- Écran rond GC9A01 240×240 (SPI, sans MISO).
- 2 boutons poussoirs (actifs à l'état bas), buzzer passif optionnel.
- Câblage : voir le tableau de pins ci-dessus (modifiable dans
  *menuconfig → Harness C3 Configuration*).

## Flasher

- **Web flasher** : ouvrir `https://silexemple.github.io/harness-c3/webflasher/`
  dans Chrome/Edge → *Connect & Flash*. Nécessite WebSerial (HTTPS ou
  localhost). Activation GitHub Pages : *Settings → Pages → Deploy from branch
  → `main` / `docs`* (un clic, une seule fois).
- **esptool** : `esptool.py --chip esp32c3 write_flash 0x0 harness-c3-merged.bin`
  (binaire sur la page Releases).
- **Depuis les sources** : `cd firmware && idf.py set-target esp32c3 && idf.py
  build flash monitor` (ESP-IDF v5.5).

## Limites de la v1

- Pas de voix (pas de micro), pas de tactile, pas de molette machines / swarms
  / sélecteur de modèle.
- Le daemon propose des mises à jour firmware pour ESP32-S3 : elles sont
  **toujours ignorées** (risque de brick — SPEC §9). La mise à jour se fait
  via le web flasher ou esptool.

## Crédits

Le protocole, le code de tramage, les vecteurs de test et la spécification
sont adaptés de
[autonomous-ai/openharness](https://github.com/autonomous-ai/openharness)
(MIT) — voir `LICENSE`.
