# AmpliVision

Amplificateur de visibilité pour jeux sombres. Windows 10 2004+ / 11, x64.
Natif C, Direct3D 11, capture par Desktop Duplication. Aucune injection,
aucune lecture mémoire de jeu.

## Compilation

1. Installer les Visual Studio Build Tools (charge « Développement Desktop en C++ »).
2. Double-cliquer `build.bat`.
3. Récupérer `build\AmpliVision.exe`.

## Utilisation

1. Lancer le jeu en **fenêtré sans bordure**.
2. Lancer `AmpliVision.exe` (les réglages démarrent sur le profil « Plein jour »).
3. **F8** pour afficher ou masquer l'overlay. **F9** ouvre le panneau.
4. **F7** maintenu : comparaison avant/après avec rideau.
5. **1 à 6** : profils rapides.

L'overlay est toujours au premier plan, traverse les clics et ne vole jamais le
focus. Le jeu en dessous garde clavier et souris.

## Profils par jeu

Choisis « Nouveau depuis réglages », nomme le profil : il est automatiquement
lié au jeu au premier plan. Au lancement suivant, les réglages s'appliquent
seuls dès que ce jeu passe devant.

## Réglages

Tout est dans `%APPDATA%\AmpliVision\settings.ini`. Les raccourcis sont
reconfigurables depuis le panneau. Journal d'erreurs dans le même dossier.

## Dépannage

| Symptôme | Cause | Solution |
|---|---|---|
| Image figée | Sortie DXGI perdue | Changer d'écran cible, F8 deux fois |
| Overlay visible dans sa propre capture | `WDA_EXCLUDEFROMCAPTURE` refusé | Décocher l'exclusion, jeu sur un 2e écran |
| Capture noire | Jeu en plein écran exclusif | Passer en fenêtré sans bordure |
| Raccourci sans effet | Touche déjà réservée | Reconfigurer dans « Raccourcis » |
| Flou général | Résolution source réduite | Désactiver la mise à l'échelle du jeu |