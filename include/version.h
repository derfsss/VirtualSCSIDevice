#ifndef VERSION_H
#define VERSION_H

/* Individual version components */
#define DEVICE_VERSION 1
#define DEVICE_REVISION 8
#define DEVICE_DATE "11.04.2026"
#define DEVICE_TIME "12:00"
#define DEVNAME "virtioscsi.device"

/* Helper macros for stringification */
#define STR(x) #x
#define XSTR(x) STR(x)

/* Map to standard AmigaOS legacy defines */
#define DEVVER DEVICE_VERSION
#define DEVREV DEVICE_REVISION

/*
 * Standard AmigaOS version string: $VER: name version.revision (date)
 * Combined using string literal concatenation.
 */
#define DEVVERSIONSTRING DEVNAME " " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " (" DEVICE_DATE ")"

/*
 * Extended version string for serial debug output at boot.
 * Includes build time for distinguishing debug/release builds.
 */
#define DEVVERSIONSTRING_FULL DEVVERSIONSTRING " [" DEVICE_TIME "]"

#endif /* VERSION_H */
