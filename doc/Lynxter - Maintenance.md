# Maintenance

Ce document aide à assurer la maintenance de SLICER.

## Mise à jour des profils

Les profils sont directement disponibles dans `resources\profiles`.
Dans SuperSlicer, les profils sont hébergés dans des dépôts séparés, que le slicer interroge pour les mises à jour.
Actuellement, aucun système de mise à jour automatique des profils n'a été défini.

### Modification d'un vendor bundle

Les profils « système » accessibles via l'assistant de configuration sont définis dans le « vendor bundle » `Lynxter.ini`.
Vous pouvez vous référer à la documentation « How to create a vendor profile » pour apprendre à en créer et à les modifier (en ignorant la dernière section, « GitHub API »).
En l'absence de mise à jour externe, les nouvelles versions sont accessibles lorsque le logiciel est mis à jour, car la nouvelle version est récupérée en même temps.
Cela veut dire que pour pouvoir recevoir une notification concernant la mise à jour des profils lors du lancement du slicer, il faut :
 * Avoir commité les modifications de `Lynxter.ini` après avoir augmenté son numéro de version.
 * Avoir créé une nouvelle release du slicer.
 * Avoir installé la nouvelle version.
 * Lors du lancement de la nouvelle version, un message doit apparaître en bas à droite pour proposer la mise à jour des profils. Il est également possible d'utiliser « Configuration > Install and upgrade vendor bundle ». Les différentes versions utilisées et accessibles sur l'ordinateur devraient être affichées.
 
 
 ## Modification du SLICER
 
 ### Nightly
 
Si vous avez modifié le code du slicer et poussé les commits sur GitHub, une compilation automatique du slicer est lancée lorsqu'il s'agit d'une branche de test (`nightly_dev`, `nightly_master`). Vous pouvez utiliser les artefacts générés pour partager temporairement votre version du slicer ; la date de compilation devrait alors apparaître à côté du numéro de version.
 
## Release
 
### Mettre à jour le numéro de version
 
Pour créer une release, vous devez modifier le fichier version.inc afin de mettre à jour le numéro de version.
Ce sont les lignes 15 à 17 :
 * SLIC3R_VERSION_TAG : s’il s’agit d’une bêta ou d’une alpha, commencez par "-X", où X est la chaîne de caractères que vous désirez (exemple : "-beta"). Ensuite, il faut absolument mettre "+UNKNOWN" pour que le processus de compilation sur GitHub fonctionne correctement (pour une release, vous devez donc avoir `set(SLIC3R_VERSION_TAG "+UNKNOWN")`).
 * SLIC3R_RC_VERSION : le numéro de version au format main.majeur.mineur.patch.
 * SLIC3R_RC_VERSION_DOTS : comme SLIC3R_RC_VERSION, mais avec des '.' à la place des ','.
 
 #### Notes à propos du fichier `version.inc` :
 * SLIC3R_BASED_ON contrôle le texte de la barre des tâches.
 * SLIC3R_DOWNLOAD est utilisé pour ouvrir le navigateur web sur cette page de téléchargement.
 * SLIC3R_DOC_URL est utilisé pour ouvrir le navigateur web sur une page d’aide vis-à-vis d’un paramètre via <SLIC3R_DOC_URL>/<lang>/article/<setting_id> (exemple : https://github.com/LynxterAM/SLICER/wiki/fr/article/perimeters). Cette fonction n’est pas utilisée ; il n’y a plus de lien vers la documentation.
 * SLIC3R_GITHUB est utilisé pour la vérification de nouvelles mises à jour en interrogeant GitHub. Si vous avez implémenté un équivalent de l’API GitHub pour la destination "https://api.github.com/repos/" SLIC3R_GITHUB "/releases", vous pouvez le faire pointer sur votre serveur. Si la chaîne commence par "http", alors "https://api.github.com/repos/" ne sera pas ajouté. SLIC3R_GITHUB est également utilisé pour accéder au site web "https://github.com/" SLIC3R_GITHUB "/release", "/wiki", "/issues/new" via le menu, ainsi qu’à "/releases/tag/%1%" via le menu de mise à jour. Vous devrez rediriger tout cela si ce n’est plus sur GitHub.

### Créer la release

Pour créer la release, il faut pousser le commit avec le nouveau numéro de version sur la branche "rc" (pour release candidate). GitHub va automatiquement lancer des builds.

Une fois tous les builds terminés (on peut ignorer les erreurs sur les plateformes non supportées), vous pouvez lancer l'utilitaire "create_release.py" comme suit :

 * Créez un fichier `githubtoken.ini` à la racine du dépôt local et ajoutez-y votre clé.
   * Allez sur https://github.com/settings/tokens pour en créer une ; il faut les autorisations pour `gist`, `repo` et `workflow`. Pensez à copier la clé.
   * Dans le fichier, collez `[github]` sur la première ligne et `token=ghp_XXXXXXX` sur la deuxième, avec votre clé.
 * Installez Python si ce n'est pas déjà le cas.
 * Lancez, depuis une invite de commandes ouverte dans votre dossier, `python create_release.py`.
 * Attendez que l'exécution se termine (le téléchargement depuis GitHub peut être long).
 * Dans le dossier `./releases`, vous devrez avoir tous les fichiers à utiliser pour la release, avec en plus un dossier préparé pour les tests et les signatures sous Windows.
 * Sous Windows, utilisez les outils fournis par votre prestataire de signature de code pour signer l’exécutable, les bibliothèques DLL et les éventuels autres fichiers exécutables distribués. Effectuez cette étape avant de créer le MSI, afin que celui-ci contienne les fichiers signés.
 * Régénérez le MSI à partir des fichiers signés :
   * Remplacez les fichiers correspondants dans le dossier extrait du package par les versions signées.
   * Installez WiX Toolset v3.14.1, nécessaire au script actuel qui utilise `heat.exe`, `candle.exe` et `light.exe` :
     * Avec Chocolatey, ouvrez PowerShell en tant qu'administrateur et lancez `choco install wixtoolset -y`.
     * Sans Chocolatey, téléchargez puis installez `wix314.exe`, ou extrayez `wix314-binaries.zip`, depuis https://github.com/wixtoolset/wix3/releases/tag/wix3141rtm.
   * Vérifiez que les outils sont accessibles avec `Get-Command heat.exe, candle.exe, light.exe`. S'ils ne sont pas dans le `PATH`, indiquez leur dossier avec `--wix-bin`, par exemple `--wix-bin "C:\Program Files (x86)\WiX Toolset v3.14\bin"`.
   * Depuis la racine du dépôt, lancez `python create_msi.py "<dossier du package extrait>" --version <version MSI> --output "<chemin du MSI>"` ; ajoutez `--wix-bin "<dossier WiX>"` si nécessaire.
   * Le script prépare la licence, génère la liste des fichiers avec `heat.exe`, compile les fichiers WiX avec `candle.exe`, puis crée le MSI avec `light.exe`.
   * Remplacez dans `./releases` l'ancien MSI par celui qui vient d'être généré.
 * Signez ensuite le MSI généré avec les outils du prestataire de signature de code et ajoutez un horodatage. Ne modifiez plus le MSI après cette signature.
 * Si vous souhaitez publier une version portable, recréez l'archive ZIP du dossier Windows après la signature des fichiers. N'utilisez pas l'archive ZIP générée avant la signature, car elle contiendrait les versions non signées.
 * Créez une release sur GitHub.
   * https://github.com/LynxterAM/SLICER/releases/new
   * Sélectionnez `rc` comme branche cible.
   * Cliquez sur « Select tag », saisissez le numéro de version, avec des points, puis cliquez sur « Create new tag ».
   * Renseignez le titre (cela peut être simplement le numéro de version) et la description.
   * Faites glisser les fichiers générés dans `./releases/<version>` vers l'espace « Attach binaries by dropping them here ».
   * S'il s'agit d'une bêta, sélectionnez « Set as pre-release ».
   * Cliquez sur « Publish release ». Si vous voulez la publier à un moment précis dans le futur, utilisez « Save draft » pour ne pas avoir à recommencer.
   * Il est recommandé de faire pointer la branche `master` ou `main` vers `rc` une fois la release publiée.
 * Une fois la release publiée, les utilisateurs des versions précédentes seront notifiés de la nouvelle version au prochain lancement.
 * Si la release est une bêta, seuls les utilisateurs qui utilisent une bêta ou qui ont activé « all » pour le paramètre `notify_release` (dont la valeur par défaut est « release ») seront notifiés.

