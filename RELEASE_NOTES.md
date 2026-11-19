## Installation

Fermez OBS, lancez **`obs-nsfw-guard-0.4.0-windows-x64-setup.exe`**, puis relancez OBS. Rien à compiler.
Si une version précédente est installée, l'installeur la met à jour (vos réglages sont conservés).
L'installeur n'est pas signé : Windows SmartScreen peut afficher un avertissement
(« Informations complémentaires » → « Exécuter quand même »).

## Nouveautés de la 0.4.0

- **Mises à jour** : le plugin vérifie une fois par jour si une nouvelle version existe (désactivable). La mise à
  jour se télécharge depuis les réglages du filtre, son empreinte est vérifiée, et elle s'installe **à la
  fermeture d'OBS**, jamais pendant un stream.
- **Installeur** : mise à jour propre par-dessus une version existante, runtime Visual C++ installé si besoin,
  détection d'OBS installé via Steam. **Désinstalleur** accessible aussi depuis le menu Démarrer, avec
  suppression optionnelle des données.
- **Moteur NudeNet optionnel** (depuis la 0.3) : ne masque que la zone détectée. Modèles non fournis, à
  télécharger depuis le filtre.
- **Fiabilité** :
  - plus de plantage d'OBS si ONNX Runtime ne peut pas être chargé ;
  - la source est masquée si l'analyse cesse de répondre ;
  - plus d'anciennes images réaffichées au retour sur une scène ;
  - délai vidéo plus stable ;
  - ligne **État** dans les réglages du filtre.

## English

Close OBS, run the `-setup.exe`, restart OBS. New in 0.4.0:
- built-in update check (daily, can be disabled); updates install when OBS closes, never mid-stream;
- in-place upgrades, bundled VC++ runtime, Start menu uninstaller;
- optional NudeNet detector;
- reliability fixes: no crash if ONNX Runtime fails to load, fail-closed watchdog, no stale frames, steadier delay, status line.
