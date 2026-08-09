/****************************************************************************
** Meta object code from reading C++ file 'pdfapplicationtranslator.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../Pdf4QtLibCore/sources/pdfapplicationtranslator.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'pdfapplicationtranslator.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN3pdf24PDFApplicationTranslatorE_t {};
} // unnamed namespace

template <> constexpr inline auto pdf::PDFApplicationTranslator::qt_create_metaobjectdata<qt_meta_tag_ZN3pdf24PDFApplicationTranslatorE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "pdf::PDFApplicationTranslator",
        "ELanguage",
        "E_LANGUAGE_AUTOMATIC_SELECTION",
        "E_LANGUAGE_ENGLISH",
        "E_LANGUAGE_CZECH",
        "E_LANGUAGE_GERMAN",
        "E_LANGUAGE_KOREAN",
        "E_LANGUAGE_SPANISH",
        "E_LANGUAGE_CHINESE_SIMPLIFIED",
        "E_LANGUAGE_CHINESE_TRADITIONAL",
        "E_LANGUAGE_FRENCH",
        "E_LANGUAGE_TURKISH",
        "E_LANGUAGE_RUSSIAN"
    };

    QtMocHelpers::UintData qt_methods {
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
        // enum 'ELanguage'
        QtMocHelpers::EnumData<enum ELanguage>(1, 1, QMC::EnumFlags{}).add({
            {    2, ELanguage::E_LANGUAGE_AUTOMATIC_SELECTION },
            {    3, ELanguage::E_LANGUAGE_ENGLISH },
            {    4, ELanguage::E_LANGUAGE_CZECH },
            {    5, ELanguage::E_LANGUAGE_GERMAN },
            {    6, ELanguage::E_LANGUAGE_KOREAN },
            {    7, ELanguage::E_LANGUAGE_SPANISH },
            {    8, ELanguage::E_LANGUAGE_CHINESE_SIMPLIFIED },
            {    9, ELanguage::E_LANGUAGE_CHINESE_TRADITIONAL },
            {   10, ELanguage::E_LANGUAGE_FRENCH },
            {   11, ELanguage::E_LANGUAGE_TURKISH },
            {   12, ELanguage::E_LANGUAGE_RUSSIAN },
        }),
    };
    return QtMocHelpers::metaObjectData<PDFApplicationTranslator, qt_meta_tag_ZN3pdf24PDFApplicationTranslatorE_t>(QMC::PropertyAccessInStaticMetaCall, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject pdf::PDFApplicationTranslator::staticMetaObject = { {
    nullptr,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf24PDFApplicationTranslatorE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf24PDFApplicationTranslatorE_t>.data,
    nullptr,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN3pdf24PDFApplicationTranslatorE_t>.metaTypes,
    nullptr
} };

QT_WARNING_POP
