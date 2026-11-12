/*
 * Relios Mirror's version: the one place where it is written. The program uses it (the log, Settings > Program),
 * and so does the version information of the exe files (app.rc). Only preprocessor lines here: the resource
 * compiler reads this file too.
 *
 * For a new version: change both lines, commit, then tag the commit "v<version>" and push the tag; the build on
 * GitHub then makes a draft release with the files (see GELISTIRME.txt).
 */
#define APP_VERSION "1.0.0"
#define APP_VERSION_NUMBERS 1, 0, 0, 0
