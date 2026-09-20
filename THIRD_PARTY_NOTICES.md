# Composants tiers / Third-party components

NSFW Guard est distribué sous licence GPL-2.0-or-later (voir `LICENSE`).
Les archives binaires incluent les composants suivants, qui ont leurs propres licences.

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

## OBS Studio — GPL-2.0-or-later

Le plugin est compilé contre les en-têtes de `libobs` (OBS Studio 32.2.2), <https://obsproject.com>.
