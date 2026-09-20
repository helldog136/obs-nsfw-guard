# NSFW Guard — filtre OBS

Un filtre vidéo pour [OBS Studio](https://obsproject.com) qui **masque une source dès que l'IA y détecte du
contenu sensible** (noir, pixelisation ou flou gaussien). Il s'ajoute à n'importe quelle source : capture
d'écran, capture de fenêtre, navigateur, etc.

*English summary below.*

## Fonctionnement

- Chaque image de la source est réduite à 224×224 et analysée par un petit classifieur
  ([MobileNetV4](https://huggingface.co/taufiqdp/mobilenetv4_conv_small.e2400_r224_in1k_nsfw_classifier), 9,5 Mo)
  exécuté par ONNX Runtime dans un thread dédié : le rendu d'OBS n'est jamais bloqué.
- **Délai de rendu intégré** : quelques images sont mises en tampon pour que le masque arrive *avant* l'image
  détectée, sans avoir besoin d'un filtre « Délai de rendu » séparé. Le délai est calculé automatiquement
  à partir du temps d'analyse mesuré.
- Le filtre ne touche pas à l'audio. Il n'ajoute un délai vidéo qu'à la source où il est placé.

Sur un Ryzen 7 5800X, l'inférence prend environ 1,3 ms par image sur CPU (0,6 ms avec DirectML sur une
Radeon RX 6900 XT). C'est une mesure sur une machine, pas une garantie.

## Installation (Windows 64 bits, OBS 32.x)

1. Fermez OBS.
2. Téléchargez `obs-nsfw-guard-*-windows-x64-setup.exe` depuis les [Releases](../../releases) et lancez-le.
3. Relancez OBS, puis : clic droit sur une source → **Filtres** → **+** → **NSFW Guard**.

Désinstallation : « Applications installées » de Windows (ou `unins000.exe` dans le dossier du plugin).

Installation manuelle : extrayez plutôt `obs-nsfw-guard-*-windows-x64.zip` dans
`C:\ProgramData\obs-studio\plugins` (le dossier `obs-nsfw-guard` doit se retrouver dedans).
L'installeur n'est pas signé : Windows SmartScreen peut afficher un avertissement
(« Informations complémentaires » → « Exécuter quand même »).

## Réglages

| Réglage | Rôle |
|---|---|
| Seuil de sensibilité | Score à partir duquel une image est jugée sensible. Plus bas = plus strict. |
| Poids de la classe « sexy » | Part du contenu limite (maillots, lingerie) dans le score. 0 = seul le contenu explicite compte. |
| Détections consécutives | Analyses positives à la suite avant de masquer. Supprime les faux positifs isolés. |
| Maintenir le masque | Durée du masque après la dernière détection. |
| Style du masque | Noir, pixelisé (taille de bloc réglable) ou flou gaussien (intensité réglable). |
| Retarder l'image | Tampon qui laisse le temps à l'analyse. Automatique ou en nombre d'images. |
| Analyser une image sur | Fréquence d'analyse (1 = chaque image). |
| GPU (DirectML) | Utilise le GPU au lieu du CPU. Le CPU suffit et ne concurrence pas le jeu. |
| Masquer tant que le modèle n'est pas prêt | La source reste masquée pendant le chargement du modèle. |
| Journaliser scores et temps | Écrit score, temps d'inférence, latence et délai dans le journal d'OBS pour calibrer le seuil. |

## Limites

- **Ce n'est pas une garantie.** Un classifieur fait des faux positifs et des faux négatifs. Ne comptez pas
  dessus comme seule protection pour respecter les règles d'une plateforme.
- La réduction en 224×224 se fait sans recadrage (pour couvrir tout l'écran) et de façon simple : de petits
  détails sur un grand écran peuvent échapper à l'analyse.
- Le délai laisse passer au plus quelques images avant le masque si l'option « Retarder l'image » est
  désactivée.
- Windows 64 bits uniquement pour l'instant. Développé et testé avec OBS 32.2.2.
- Sources semi-transparentes : la composition alpha n'est pas gérée avec précision.

## Compilation

Prérequis : Visual Studio 2022 ou 2026 (ou Build Tools) avec la charge de travail C++, Git, PowerShell,
et OBS Studio installé.

```powershell
scripts\fetch-deps.ps1   # en-têtes OBS, obs.lib, ONNX Runtime, modèle
scripts\build.ps1        # compile dans stage\obs-nsfw-guard
scripts\install.ps1      # copie dans C:\ProgramData\obs-studio\plugins (OBS fermé)
scripts\package.ps1      # crée dist\obs-nsfw-guard-<version>-windows-x64.zip
scripts\installer.ps1    # crée l'installeur .exe (nécessite Inno Setup 6)
```

## Licence

GPL-2.0-or-later (voir [LICENSE](LICENSE)), comme OBS Studio. Composants tiers : voir
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## English summary

**NSFW Guard** is a per-source OBS Studio video filter that hides a source (black, pixelate or Gaussian blur)
when a small on-device ONNX classifier detects sensitive content. It has a built-in frame buffer so the mask
lands *before* the detected frame is shown, no separate Render Delay filter needed. Inference runs on a
worker thread (about 1.3 ms/frame on a Ryzen 7 5800X CPU) and everything stays local.

Windows x64 only, developed against OBS 32.2.2. Run the `-setup.exe` from the latest release (or unzip the
zip into `C:\ProgramData\obs-studio\plugins`), restart OBS, and add **NSFW Guard** from a source's Filters dialog. It is not a guarantee: classifiers make
mistakes. Licensed GPL-2.0-or-later.
