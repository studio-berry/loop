
#ifndef PDF4QTLIBCORESHARED_EXPORT_H
#define PDF4QTLIBCORESHARED_EXPORT_H

#ifdef PDF4QTLIBCORE_STATIC_DEFINE
#  define PDF4QTLIBCORESHARED_EXPORT
#  define PDF4QTLIBCORE_NO_EXPORT
#else
#  ifndef PDF4QTLIBCORESHARED_EXPORT
#    ifdef Pdf4QtLibCore_EXPORTS
        /* We are building this library */
#      define PDF4QTLIBCORESHARED_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define PDF4QTLIBCORESHARED_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef PDF4QTLIBCORE_NO_EXPORT
#    define PDF4QTLIBCORE_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef PDF4QTLIBCORE_DEPRECATED
#  define PDF4QTLIBCORE_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef PDF4QTLIBCORE_DEPRECATED_EXPORT
#  define PDF4QTLIBCORE_DEPRECATED_EXPORT PDF4QTLIBCORESHARED_EXPORT PDF4QTLIBCORE_DEPRECATED
#endif

#ifndef PDF4QTLIBCORE_DEPRECATED_NO_EXPORT
#  define PDF4QTLIBCORE_DEPRECATED_NO_EXPORT PDF4QTLIBCORE_NO_EXPORT PDF4QTLIBCORE_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef PDF4QTLIBCORE_NO_DEPRECATED
#    define PDF4QTLIBCORE_NO_DEPRECATED
#  endif
#endif

#endif /* PDF4QTLIBCORESHARED_EXPORT_H */
