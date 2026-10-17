# Composants tiers / Third-party components

NSFW Guard est distribué sous licence GPL-2.0-or-later (voir `LICENSE`).
Les archives binaires incluent les composants suivants (sauf mention contraire), qui ont leurs propres licences.

## ONNX Runtime — MIT

`onnxruntime.dll`, `onnxruntime_providers_shared.dll` (paquet NuGet `Microsoft.ML.OnnxRuntime.DirectML`).
Copyright (c) Microsoft Corporation. Licence MIT :
<https://github.com/microsoft/onnxruntime/blob/main/LICENSE>

## Modèle de classification — Apache-2.0

`data/nsfw_model.onnx` est le modèle
[`taufiqdp/mobilenetv4_conv_small.e2400_r224_in1k_nsfw_classifier`](https://huggingface.co/taufiqdp/mobilenetv4_conv_small.e2400_r224_in1k_nsfw_classifier)
(architecture MobileNetV4 Conv Small), publié sous licence Apache-2.0 :
<https://www.apache.org/licenses/LICENSE-2.0>

Classes : `drawings`, `hentai`, `neutral`, `porn`, `sexy`. La fiche du modèle ne détaille ni le jeu
de données d'entraînement ni les performances : ne le considérez pas comme un filtre garanti.

## DirectML

L'option GPU utilise le `DirectML.dll` fourni par Windows. Il n'est **pas** redistribué avec ce plugin.

## NudeNet — NON distribué avec ce plugin (téléchargement optionnel par l'utilisateur)

Le moteur « détecteur » du plugin peut utiliser les modèles
[NudeNet](https://github.com/notAI-tech/NudeNet) (`320n.onnx`, `640m.onnx`, release `v3.4-weights`),
un projet tiers sans lien avec ce plugin. **Ces fichiers ne sont inclus ni dans ce dépôt, ni dans les
archives et installeurs publiés, ni dans la CI.** Ils sont téléchargés par l'utilisateur lui-même,
directement depuis la release officielle de NudeNet (bouton du filtre, après acceptation de l'utilisateur),
ou importés depuis un fichier que l'utilisateur a téléchargé. Le plugin vérifie leur taille et leur empreinte
SHA-256 :

| Fichier | SHA-256 |
|---|---|
| `320n.onnx` | `c15d8273adad2d0a92f014cc69ab2d6c311a06777a55545f2c4eb46f51911f0f` |
| `640m.onnx` | `04fe3d77980780c1f8297dc6d7f942fd5b3abe6942a188f742a85241e4f634eb` |

Le code de NudeNet est publié sous licence **AGPL-3.0** (<https://github.com/notAI-tech/NudeNet/blob/v3/LICENSE>).
Le dépôt ne précise pas la licence des fichiers de modèle, qui sont dérivés de YOLOv8 (Ultralytics, AGPL-3.0).
Ce plugin ne contient aucun code de NudeNet : il charge le modèle avec ONNX Runtime et décode lui-même ses
sorties. **Avant de télécharger ou d'utiliser ces modèles, consultez leur licence et respectez-la pour votre
usage** ; le plugin ne vous en accorde aucun droit. NSFW Guard n'est ni affilié à NudeNet ni approuvé par ses
auteurs.

*NudeNet is a third-party project. Its model files are NOT distributed with this plugin (not in the
repository, the releases, the installer or the CI): users download them themselves from NudeNet's official
release, or import a file they downloaded. NudeNet's code is AGPL-3.0; its repository does not state the
licence of the model files (derived from Ultralytics YOLOv8, AGPL-3.0). Check and comply with their licence for
your use. This plugin is not affiliated with, or endorsed by, NudeNet's authors.*

## OBS Studio — GPL-2.0-or-later

Le plugin est compilé contre les en-têtes de `libobs` (OBS Studio 32.2.2), <https://obsproject.com>.
