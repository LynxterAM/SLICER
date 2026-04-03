# Maintenance

Ce document est là pour aider à la maintenace du SLICER.

## mise à jour des profils.

les profils sont disponible dans \resources\profiles directement.
Dans Superslicer, le sprofils sont hébergés dans des repositories séparé et sont intérogés par le slicer pour les mises à jour.
Actuellement, il n'a pas été décidé d'un système de mise à jour automatique des profils.

### modification d'un vendor bundle

Les profils "système" accessible via le wizard sont défini dans le 'vendor bundle' Lynxter.ini.
Vous pouvez vous référer à la docuemntation 'how to create a vendor profile' pour apprendre à en créer et les modifier (en ignorant la dernière  'Github API').
Du fait de l'abscence de mise à jour externae, les nouvelles versions sont accessible lorsque le logiciel est mis à jour, la nouvelle version étant récupéré avec.
Cela veut dire que pour pouvoir avoir une notification pour la mise à jour des profils lors du lancement du slicer, il faut:
 * avoir commit les changements de lynxter.ini en ayant augmenté le numéro de version.
 * avoir créer une nouvelle release du slicer
 * avoir installé la nouvelle version
 * lors su lancement de la nouvelle version, un message doit apparaitre en bas à droite pour mettre à jour les profils. Il est aussi possible d'utiliser configuration -> install and upgrade vendor bundle, les différentes versions utilisés/accessible sur l'ordinateur devrait être affichés.
 
 
 ## modification du SLICER
 
 ### nightly
 
Si vous avez effectué des modifications du code du slicer, une fois les commits poussés sur github, s'il s'agit d'une branche de test (nightly_dev, nightly_master) une compilation automatique du slicer est lancée. Vous pouvez utiliser les artefacts générés pour partager temporairement votre version du slicer, la date de compilation devrait alors apparaître à côté du numéro de version.
 
## release
 
### mettre à jour e numéro de version
 
Pour créer une release, vous devez modifier le fichier version.inc afin de mettre à jour le numéro de version.
Ce sont les lignes 15 à 17 :
 * SLIC3R_VERSION_TAG : s’il s’agit d’une beta/alpha, commencez par "-X", avec X la chaîne de caractères simple ([A-Za-z_0-9]) que vous désirez (exemple : "-beta"). Ensuite, il faut absolument mettre "+UNKNOWN" pour ne pas casser le processus de compilation sur GitHub (donc pour une release, vous devez avoir set(SLIC3R_VERSION_TAG "+UNKNOWN")).
 * SLIC3R_RC_VERSION : le numéro de version au format main.majeure.mineure.patch.
 * SLIC3R_RC_VERSION_DOTS : comme SLIC3R_RC_VERSION mais avec des ',' à la place des '.'.
 
 #### Notes à propos de version.inc:
 * SLIC3R_BASED_ON contrôle le texte de la barre des tâches.
 * SLIC3R_DOWNLOAD est utilisé pour ouvrir le navigateur web sur cette page de téléchargement.
 * SLIC3R_DOC_URL est utilisé pour ouvrir le navigateur web sur une page d’aide vis-à-vis d’un paramètre via <SLIC3R_DOC_URL>/<lang>/article/<setting_id> (exemple : https://github.com/LynxterAM/SLICER/wiki/fr/article/perimeters). Cette fonction n’est pas utilisée ; il n’y a plus de lien vers la documentation.
 * SLIC3R_GITHUB est utilisé pour la vérification de nouvelles mises à jour en interrogeant GitHub. Si vous avez implémenté un équivalent de l’API GitHub pour la destination "https://api.github.com/repos/" SLIC3R_GITHUB "/releases", vous pouvez le faire pointer sur votre serveur. Si la chaîne commence par "http", alors "https://api.github.com/repos/" ne sera pas ajouté. Également utilisé pour accéder au site web "https://github.com/" SLIC3R_GITHUB "/release", "/wiki", "/issues/new" via le menu, ainsi que "/releases/tag/%1%" via le menu de mise à jour. Vous devrez rediriger tout cela si ce n’est plus sur GitHub.

### créer la release

Pour créer la release, il faut pousser le commit avec le nouveau numéro de version sur la branche "rc" (pour release candidate). GitHub va automatiquement lancer des builds.

Une fois tous les builds terminés (on peut ignorer les erreurs sur les plateformes non supportées), vous pouvez lancer l'utilitaire "create_release.py" comme suit :

 * créez un fichier "githubtoken.ini" à la racine du repository (en local) et mettez-y votre clé
   * allez sur https://github.com/settings/tokens pour en créer une ; il faut les droits pour gist, repo, workflow. Pensez à copier la clé
   * dans le fichier, collez `[github]` sur la première ligne et `token=ghp_XXXXXXX` sur la deuxième, avec votre clé
 * installez Python si ce n'est pas déjà le cas
 * lancez, dans une invite de commande située dans votre dossier, `python create_release.py`
 * attendez que l'exécution se termine (le téléchargement depuis GitHub peut être long)
 * dans le dossier ./releases, vous devrez avoir tous les fichiers à utiliser pour la release, avec en plus un dossier préparé pour tester (sur Windows)
 * créez une release sur GitHub
   * https://github.com/LynxterAM/SLICER/releases/new
   * changez la target sur rc
   * cliquez sur "Select tag", tapez le numéro de version (avec les '.') puis "Create new tag"
   * renseignez le titre (cela peut être simplement le numéro de version) et la description
   * cliquez-déplacez les fichiers générés dans ./releases/<version> dans l'espace "Attach binaries by dropping them here"
   * s'il s'agit d'une beta, sélectionnez "Set as pre-release"
   * cliquez sur "Publish release". Si vous voulez la publier à un moment précis dans le futur, utilisez "Save draft" pour ne pas avoir à recommencer
   * il est de bon ton de déplacer la branche master ou main à l'emplacement de rc une fois publiée
 * une fois la release publiée, les utilisateurs des versions précédentes seront notifiés de la nouvelle version au prochain lancement.
 * si la release est une bêta, seuls ceux étant sur une bêta ou ayant activé "all" sur le paramètre "notify_release" (par défaut sur "release") seront notifiés.

