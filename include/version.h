#ifndef VERSION_H
#define VERSION_H

/* Individual version components */
#define DEVICE_VERSION 53
#define DEVICE_REVISION 8
#define DEVNAME "virtioscsi.device"

/* Helper macros for stringification */
#define STR(x) #x
#define XSTR(x) STR(x)

/* Map to standard AmigaOS legacy defines */
#define DEVVER DEVICE_VERSION
#define DEVREV DEVICE_REVISION

/*
 * Build date and time: passed from the Makefile via -DBUILD_DATE and
 * -DBUILD_TIME so they reflect the actual build timestamp.  Falls back
 * to the compiler's __DATE__ / __TIME__ if not defined.
 */
#ifndef BUILD_DATE
#define BUILD_DATE __DATE__
#endif
#ifndef BUILD_TIME
#define BUILD_TIME __TIME__
#endif

/*
 * Standard AmigaOS version string: $VER: name version.revision (date)
 * Combined using string literal concatenation.
 */
#define DEVVERSIONSTRING DEVNAME " " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " (" BUILD_DATE ")"

/*
 * Extended version string for serial debug output at boot.
 * Includes build time for distinguishing debug/release builds.
 */
#define DEVVERSIONSTRING_FULL DEVVERSIONSTRING " [" BUILD_TIME "]"

#endif /* VERSION_H */
