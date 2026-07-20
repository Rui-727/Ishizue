#ifndef ISHIZUE_VERSION_H
#define ISHIZUE_VERSION_H

#define ISHIZUE_VERSION_MAJOR 1
#define ISHIZUE_VERSION_MINOR 3
#define ISHIZUE_VERSION_PATCH 0

#define ISHIZUE_VERSION_STRING "1.3.0"

#define ISHIZUE_VERSION \
    ((ISHIZUE_VERSION_MAJOR << 16) | \
     (ISHIZUE_VERSION_MINOR <<  8) | \
     (ISHIZUE_VERSION_PATCH))

/* Runtime version query. Returns the ISHIZUE_VERSION_STRING constant.
 * Visibility is set on the definition in src/util/isz_version.c. */
const char *isz_version_string(void);

#endif
