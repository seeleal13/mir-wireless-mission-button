# MiR Wireless Push Button — Contrôleur Embarqué

> **Système de bouton-poussoir industriel sans fil pour le pilotage des missions du robot mobile autonome MiR100.**  
> L'interaction physique remplace les interfaces logicielles complexes : appuyer sur un bouton suffit à mettre le robot en mouvement.

---

## Table des matières

1. [Vue d'ensemble](#vue-densemble)
2. [Architecture](#architecture)
   - [Modèle 1 — Station autonome (`Main.ino` / `mir_controller_modern.ino`)](#modèle-1--station-autonome)
   - [Modèle 2 — Système distribué maître-esclave (`ESP1` + `ESP2`)](#modèle-2--système-distribué-maître-esclave)
3. [Description des fichiers](#description-des-fichiers)
4. [Matériel](#matériel)
   - [Correspondance des broches](#correspondance-des-broches)
   - [Liste du matériel](#liste-du-matériel)
5. [Configuration](#configuration)
   - [`config.txt` — Modèle 1](#configtxt--modèle-1)
   - [`config.txt` — Modèle 2 (ESP2 Maître)](#configtxt--modèle-2-esp2-maître)
6. [Firmware](#firmware)
   - [Dépendances](#dépendances)
   - [Instructions de flashage](#instructions-de-flashage)
   - [Checklist de mise en service](#checklist-de-mise-en-service)
7. [Logique d'interaction](#logique-dinteraction)
   - [Comportement des boutons](#comportement-des-boutons)
   - [Indicateur LED / Statut](#indicateur-led--statut)
   - [File d'attente des missions](#file-dattente-des-missions)
8. [Intégration API MiR](#intégration-api-mir)
9. [Communication ESP-NOW (Modèle 2)](#communication-esp-now-modèle-2)
10. [Structure du dépôt](#structure-du-dépôt)
11. [Limitations connues & Feuille de route](#limitations-connues--feuille-de-route)
12. [Licence](#licence)

---

## Vue d'ensemble

Ce projet implémente un **contrôleur physique à bouton-poussoir** pour le **robot mobile industriel autonome MiR100**. Il est conçu pour les environnements de production où les opérateurs doivent déclencher des missions robotiques instantanément — sans recourir à un PC, une tablette ou l'interface native du robot.

**Fonctionnalités principales :**

- Un ou deux boutons physiques déclenchent des missions robot via WiFi et l'API REST MiR
- Une file d'attente circulaire logicielle mémorise plusieurs requêtes sans en perdre aucune
- Suivi des missions en temps réel avec retour visuel d'état (indicateurs LED RGB à l'écran)
- Appui court / appui long pour différencier les missions normales et prioritaires
- Tous les paramètres réseau et les définitions de missions sont chargés depuis un **fichier de configuration sur carte SD** — aucune recompilation nécessaire pour modifier les cibles
- Deux architectures de déploiement : **station autonome mono-unité** et **système multi-postes maître-esclave via ESP-NOW**

---

## Architecture

### Modèle 1 — Station autonome

```
┌─────────────────────────────────────────┐
│           ESP32 (Unité unique)          │
│                                         │
│  [BTN_DOWN]  [BTN_SELECT]               │
│       │            │                    │
│  Navigation    Envoi / Priorité         │
│                     │                   │
│        File d'attente logicielle        │
│            (FIFO, 8 slots)              │
│                     │                   │
│         HTTP POST → API REST MiR100     │
│         HTTP GET  ← Polling état mission│
│                     │                   │
│        Écran TFT (ILI9341, 240×320)     │
│        [LED_RED] [LED_YELLOW] [LED_GREEN]│
└─────────────────────────────────────────┘
```

Un seul ESP32 gère l'intégralité du système : entrées utilisateur, affichage, gestion de la file d'attente et communication WiFi/HTTP avec le robot. Toute la configuration est lue au démarrage depuis `/config.txt` sur une carte microSD.

Deux variantes de firmware existent pour ce modèle :

| Fichier | Description |
|---|---|
| `Main.ino` | Version de production, propre et complète. Machine à états pour le suivi des missions, file d'attente prioritaire, indicateurs LED RGB, config SD. |
| `mir_controller_modern.ino` | Version étendue avec thème sombre moderne, icônes de statut par ligne de mission, notifications toast et en-tête avec dégradé. |

---

### Modèle 2 — Système distribué maître-esclave

```
┌──────────────────┐    ESP-NOW (802.11)   ┌──────────────────────────────┐
│  ESP1 — Satellite│ ───────────────────→  │    ESP2 — Station Maître     │
│                  │   ButtonPacket{bool}  │                              │
│  [BUTTON_PIN 4]  │                       │  [BTN_DOWN 22] [BTN_SELECT 27]│
│                  │                       │                              │
│  Sans routeur    │                       │  WiFi ←→ API REST MiR100     │
│  Sans écran      │                       │  Écran TFT ILI9341           │
│  Ultra-simple    │                       │  Config carte SD             │
└──────────────────┘                       └──────────────────────────────┘
```

Lorsqu'une ligne de production nécessite **plusieurs postes opérateur** pilotant la même flotte robotique, le modèle distribué est utilisé. Le satellite (ESP1) n'a ni écran ni connexion au routeur WiFi. Il émet un paquet radio minimal vers le maître (ESP2) via **ESP-NOW** (protocole WiFi pair-à-pair en couche 2, sans routeur). Le maître gère l'ensemble des sessions WiFi, l'affichage et la file d'attente.

---

## Description des fichiers

| Fichier | Modèle | Rôle |
|---|---|---|
| `Main.ino` | 1 — Autonome | Contrôleur autonome de production. Machine à états complète, file prioritaire, indicateurs LED RGB, config SD. |
| `mir_controller_modern.ino` | 1 — Autonome | Variante UI améliorée de `Main.ino`. Ajoute l'en-tête dégradé, les icônes de statut par mission, les notifications toast et le thème sombre moderne. |
| `ESP1_NOW_TEST.ino` | 2 — Satellite | Nœud satellite minimal. Bouton unique → paquet ESP-NOW vers ESP2. Ni routeur WiFi, ni écran. |
| `ESP2_NOW_TEST.ino` | 2 — Maître | Nœud maître. Reçoit les déclenchements ESP-NOW d'ESP1, gère son propre écran TFT et sa file de missions, maintient la session WiFi avec le robot MiR. |

---

## Matériel

### Correspondance des broches

#### Modèle 1 / ESP2 Maître

| Signal | GPIO | Description |
|---|---|---|
| `BTN_DOWN` | 22 | Navigation dans la liste des missions (INPUT_PULLUP) |
| `BTN_SELECT` | 27 | Validation / envoi de mission (INPUT_PULLUP) |
| `TFT_DC` | 2 | Données/commande écran |
| `TFT_CS` | 15 | Chip select écran |
| `TFT_SCK` | 14 | Horloge SPI écran |
| `TFT_MOSI` | 13 | Données SPI sortie écran |
| `TFT_MISO` | 12 | Données SPI entrée écran |
| `TFT_BL` | 21 | Rétroéclairage (mettre à HIGH) |
| `SD_CS` | 5 | Chip select carte SD |
| `SD_SCK` | 18 | Horloge SPI carte SD |
| `SD_MISO` | 19 | Données SPI entrée carte SD |
| `SD_MOSI` | 23 | Données SPI sortie carte SD |

> **Bus SPI :** L'écran utilise le bus **HSPI** (SPI matériel). La carte SD utilise **VSPI** dans `ESP2_NOW_TEST.ino` et **HSPI** dans `Main.ino` / `mir_controller_modern.ino`. Vérifier le câblage SPI en fonction du bus utilisé.

#### ESP1 Satellite

| Signal | GPIO | Description |
|---|---|---|
| `BUTTON_PIN` | 4 | Bouton-poussoir industriel (INPUT_PULLUP) |

---

### Liste du matériel

| Composant | Qté | Remarques |
|---|---|---|
| Carte de développement ESP32 | 1 à 2 | N'importe quel ESP32 DevKit 38 broches standard |
| Écran TFT ILI9341 (240×320) | 1 | Utilisé sur le Modèle 1 et ESP2 uniquement |
| Module carte microSD | 1 | Utilisé sur le Modèle 1 et ESP2 uniquement |
| Bouton-poussoir momentané (industriel) | 2 à 3 | BTN_DOWN + BTN_SELECT (+ bouton optionnel ESP1) |
| Carte microSD (FAT32) | 1 | Pour le fichier `config.txt` |
| Alimentation 3,3 V / USB | — | Alimentation standard ESP32 |

---

## Configuration

L'ensemble des identifiants réseau et des définitions de missions est stocké dans un fichier texte brut sur la carte SD. **Aucune recompilation du firmware n'est nécessaire** pour modifier le WiFi, l'adresse IP du robot ou les missions.

### `config.txt` — Modèle 1

Placer ce fichier à la **racine** de la carte SD sous le chemin `/config.txt` :

```ini
# MiR Controller — config.txt
# Les lignes commençant par # sont ignorées.

wifi_ssid=NomDuReseauWiFi
wifi_password=MotDePasseWiFi

# Adresse IP du robot MiR100 sur le réseau local
robot_ip=192.168.1.100

# Token d'authentification Basic (encodé Base64, obtenu depuis le Fleet Manager MiR)
auth_token=Basic ZGlzdHJpYnV0b3I6NjJmMmYwZjFlZmYxMGQ...

# Missions : format  mission=NomAffiche,GUID
# Jusqu'à 8 missions supportées (MAX_MISSIONS = 8)
mission=Station A,xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
mission=Station B,yyyyyyyy-yyyy-yyyy-yyyy-yyyyyyyyyyyy
mission=Charge,   zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz
```

### `config.txt` — Modèle 2 (ESP2 Maître)

Même format que le Modèle 1, avec une **entrée supplémentaire** pour la mission dédiée au satellite ESP1 :

```ini
wifi_ssid=NomDuReseauWiFi
wifi_password=MotDePasseWiFi
robot_ip=192.168.1.100
auth_token=Basic ZGlzdHJpYnV0b3I6...

# Missions affichées sur l'écran local d'ESP2
mission=Station A,xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
mission=Station B,yyyyyyyy-yyyy-yyyy-yyyy-yyyyyyyyyyyy

# Mission dédiée au bouton satellite ESP1
esp1_guid=aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa
```

> Si `esp1_guid` est absent ou si le chargement de la carte SD échoue, ESP2 utilise en repli la valeur codée en dur `FALLBACK-ESP1-GUID-HERE` définie dans le source.

#### Comment obtenir les GUIDs de mission

Les GUIDs sont attribués par le Fleet Manager MiR. Pour les récupérer :

1. Ouvrir l'interface web MiR (par défaut `http://192.168.1.100`)
2. Naviguer vers **Missions** → sélectionner une mission → copier son GUID depuis l'URL ou le panneau de détails
3. Alternativement, interroger l'API REST : `GET /api/v2.0.0/missions` et repérer le champ `guid` de chaque entrée

---

## Firmware

### Dépendances

À installer via le **Gestionnaire de bibliothèques Arduino** (Outils → Gérer les bibliothèques) :

| Bibliothèque | Usage |
|---|---|
| `Arduino_GFX_Library` | Pilote écran TFT (ILI9341) |
| `SD` (intégrée ESP32) | Accès carte microSD |
| `WiFi` (intégrée ESP32) | Mode station WiFi |
| `HTTPClient` (intégrée ESP32) | Requêtes HTTP POST / GET vers l'API MiR |
| `esp_now.h` (intégrée ESP32) | Protocole radio pair-à-pair ESP-NOW (Modèle 2 uniquement) |

**Carte cible :** Core Arduino ESP32 (≥ 2.x). À installer via le Gestionnaire de cartes → rechercher `esp32` par Espressif Systems.

### Instructions de flashage

#### Modèle 1 — Station autonome

1. Ouvrir `Main.ino` **ou** `mir_controller_modern.ino` dans l'IDE Arduino
2. Sélectionner la carte : **ESP32 Dev Module** (ou la variante correspondante)
3. Vitesse de téléversement : `115200` ou `921600`
4. Préparer la carte SD : formater en FAT32, créer `/config.txt` (voir ci-dessus)
5. Insérer la carte SD, connecter l'ESP32 par USB
6. Cliquer sur **Téléverser**
7. Ouvrir le Moniteur Série à **115200 bauds** pour observer les logs de démarrage

#### Modèle 2 — Système distribué

**Étape 1 — Flasher ESP2 (Maître) en premier :**

1. Ouvrir `ESP2_NOW_TEST.ino`
2. Téléverser sur l'ESP32 maître
3. Ouvrir le Moniteur Série — noter l'adresse MAC affichée au démarrage :
   ```
   ESP2 MAC: 5C:01:3B:69:23:E8
   ```

**Étape 2 — Mettre à jour ESP1 avec le MAC d'ESP2, puis flasher :**

1. Ouvrir `ESP1_NOW_TEST.ino`
2. Modifier la ligne 5 avec l'adresse MAC réelle d'ESP2 :
   ```cpp
   uint8_t esp2MAC[] = { 0x5C, 0x01, 0x3B, 0x69, 0x23, 0xE8 };
   ```
3. Téléverser sur l'ESP32 satellite

**Étape 3 — Vérification :**

- Appuyer sur le bouton ESP1 → le Moniteur Série d'ESP2 doit afficher `ESP-NOW trigger received!`
- L'écran ESP2 doit afficher `ESP1 Sending...` puis `ESP1 OK!`

### Checklist de mise en service

- [ ] Carte SD formatée en FAT32
- [ ] `/config.txt` présent à la racine de la carte SD avec les identifiants WiFi et robot corrects
- [ ] GUIDs de mission copiés correctement depuis le Fleet Manager MiR
- [ ] Adresse MAC d'ESP2 mise à jour dans `ESP1_NOW_TEST.ino` (Modèle 2 uniquement)
- [ ] Les deux unités ESP32 sur le même canal WiFi (canal 1 par défaut — voir `esp_wifi_set_channel`)
- [ ] Le robot MiR100 est joignable depuis le sous-réseau de l'ESP32

---

## Logique d'interaction

### Comportement des boutons

#### Navigation (BTN_DOWN — GPIO 22)

Un appui fait défiler la liste des missions chargées affichée à l'écran TFT. La mission sélectionnée est mise en surbrillance. Anti-rebond : 300 ms.

#### Envoi / Priorité (BTN_SELECT — GPIO 27)

| Type d'appui | Durée | Action |
|---|---|---|
| Appui court | < 600 ms | Ajoute la mission sélectionnée en file avec la **priorité 0** (normale) |
| Appui long | ≥ 600 ms | Ajoute la mission sélectionnée en file avec la **priorité 1** (haute) |

Lors d'un appui long, un indicateur visuel apparaît à l'écran (`!` dans `Main.ino`, badge `PRI!` dans `mir_controller_modern.ino`) pour confirmer que le seuil de haute priorité a été atteint. Il disparaît au relâchement.

Les deux types d'appui ajoutent la mission à la file d'attente logicielle — ils ne bloquent jamais l'interface.

#### Bouton satellite ESP1 (Modèle 2 uniquement — GPIO 4)

Un appui simple envoie un `ButtonPacket` via ESP-NOW à ESP2. ESP2 intercepte le paquet dans son callback d'interruption `onDataRecv`, positionne un drapeau `volatile bool esp1Triggered = true`, et la boucle principale déclenche la mission `esp1_guid` à l'itération suivante. Anti-rebond : 250 ms.

---

### Indicateur LED / Statut

La barre LED à l'écran (trois cercles : rouge, jaune, vert) reflète en temps réel l'état de la mission active :

| État LED | Couleur | Signification |
|---|---|---|
| `LED_NONE` | Tous éteints | Inactif — aucune mission en cours |
| `LED_YELLOW` | Jaune | Mission soumise et en file d'attente, ou en cours d'exécution |
| `LED_GREEN` | Vert | Mission terminée avec succès (`Done`) |
| `LED_RED` | Rouge | Échec de transmission, mission annulée ou erreur API |

Dans `mir_controller_modern.ino`, une **pastille de statut** dans l'en-tête indique également l'état courant sous forme de libellé textuel (`IDLE`, `RUNNING`, `DONE`, `FAULT`). De plus, chaque ligne de mission affiche une icône inline :

| Icône | Signification |
|---|---|
| Point gris | Pas encore déclenchée |
| Trois points jaunes `...` | En cours d'exécution |
| Coche verte `✓` | Dernière exécution réussie |
| Croix rouge `✗` | Dernière exécution échouée |

---

### File d'attente des missions

Le firmware implémente une **file FIFO circulaire logicielle** de 8 slots (`QSIZE = 8`). Chaque slot mémorise l'index de la mission et sa priorité. La file est gérée par deux fonctions :

```
enqueue(missionIdx, priorite)   →  ajoute en queue ; ignore silencieusement si pleine
dequeue(&missionIdx, &priorite) →  retire en tête ; retourne false si vide
```

La file est traitée dans la boucle principale par une **machine à états non bloquante** (`processQueue`) :

```
TRACK_IDLE    →  retire l'entrée suivante → HTTP POST vers l'API MiR (3 tentatives)
                   ├─ POST échoué  → LED_RED, retour à IDLE
                   └─ POST OK      → LED_YELLOW, passage en TRACK_POLLING

TRACK_POLLING →  interroge /mission_queue/{id} toutes les 2 s
                   ├─ Pending / Executing → LED_YELLOW (poursuite du polling)
                   ├─ Done               → LED_GREEN, retour à IDLE
                   └─ Aborted / Failed / erreur → LED_RED, retour à IDLE
```

Cette conception garantit que l'interface (boutons, affichage) reste entièrement réactive même lorsqu'une mission est en vol.

---

## Intégration API MiR

Le contrôleur utilise l'**API REST MiR Fleet v2.0.0** via HTTP.

### POST — Mettre une mission en file d'attente

```
POST http://{robot_ip}/api/v2.0.0/mission_queue
Content-Type: application/json
Authorization: Basic <token_base64>
Accept-Language: en_US

{
  "mission_id": "<GUID>",
  "parameters": [],
  "priority": 0
}
```

En cas de succès (HTTP 2xx), le corps de la réponse contient l'`id` de l'entrée en file, utilisé pour le polling ultérieur.

### GET — Interroger l'état d'une mission

```
GET http://{robot_ip}/api/v2.0.0/mission_queue/{id}
Authorization: Basic <token_base64>
```

Le champ `state` du corps de réponse est analysé et correspond aux états LED décrits ci-dessus. Valeurs possibles : `Pending`, `Executing`, `Done`, `Aborted`, `Failed`.

> Un analyseur JSON minimal (`jsonGetString`) est embarqué dans le firmware pour éviter l'inclusion d'une bibliothèque JSON complète. Il gère les formes `"clé":"valeur"` et `"clé": valeur` (numérique).

---

## Communication ESP-NOW (Modèle 2)

ESP-NOW est un protocole sans connexion propriétaire Espressif opérant au niveau de la couche MAC 802.11. Aucune association à un routeur n'est requise.

**Configuration utilisée dans ce projet :**

| Paramètre | Valeur |
|---|---|
| Canal | 1 (fixé via `esp_wifi_set_channel`) |
| Chiffrement | Désactivé |
| Interface | `WIFI_IF_STA` |
| Paquet | `ButtonPacket { bool trigger }` — 1 octet |

**Important :** ESP-NOW exige que les deux pairs soient sur le **même canal WiFi**. Comme ESP2 se connecte au routeur (qui peut attribuer un canal dynamiquement), le canal est **forcé à 1** sur les deux unités. Si votre routeur utilise un canal différent, mettre à jour `esp_wifi_set_channel(X, WIFI_SECOND_CHAN_NONE)` dans les deux sketches en conséquence.

L'adresse MAC d'ESP2 est **codée en dur** dans `ESP1_NOW_TEST.ino` et doit être mise à jour avant le flashage (voir [Instructions de flashage](#instructions-de-flashage)).

---

## Structure du dépôt

```
.
├── Main.ino                    # Modèle 1 — contrôleur autonome de production
├── mir_controller_modern.ino   # Modèle 1 — variante UI améliorée
├── ESP1_NOW_TEST.ino           # Modèle 2 — nœud satellite (sans écran, sans routeur WiFi)
├── ESP2_NOW_TEST.ino           # Modèle 2 — nœud maître (WiFi + écran + récepteur ESP-NOW)
└── README.md
```

**Carte SD (non versionnée dans le dépôt) :**

```
RACINE_SD/
└── config.txt    # WiFi, IP robot, token d'auth, GUIDs de mission
```

---

## Limitations connues & Feuille de route

**Limitations actuelles :**

- Le canal WiFi est codé en dur à 1 ; si le routeur sélectionne automatiquement un autre canal, ESP-NOW échouera (Modèle 2)
- `ESP2_NOW_TEST.ino` embarque les identifiants WiFi directement dans le source — à migrer vers le fichier `config.txt` de la carte SD (comme c'est déjà le cas dans `Main.ino`)
- Pas de TLS / HTTPS — l'API MiR est accédée en HTTP simple ; acceptable sur un réseau industriel isolé, inadapté à un réseau routé
- L'analyseur JSON minimal peut échouer sur des chaînes imbriquées ou échappées
- Pas d'historique persistant des missions ni de journalisation

**Améliorations envisagées :**

- [ ] Migrer les identifiants WiFi de `ESP2_NOW_TEST.ino` vers `config.txt`
- [ ] Ajouter le support HTTPS via `WiFiClientSecure`
- [ ] Rendre le canal WiFi configurable depuis `config.txt`
- [ ] Ajouter le support de mise à jour OTA (Over-The-Air)
- [ ] Ajouter un troisième bouton ou un appui long sur BTN_DOWN pour annuler l'entrée active en file
- [ ] Remplacer l'analyseur JSON minimal par `ArduinoJson`
- [ ] Ajouter un indicateur de niveau de batterie pour les boîtiers sans fil

---

## Licence

Ce projet est distribué sous licence **MIT**. Voir le fichier `LICENSE` pour les détails.

---

*Développé avec le framework Arduino sur ESP32. Testé avec le logiciel Fleet MiR100 v2.x.*
