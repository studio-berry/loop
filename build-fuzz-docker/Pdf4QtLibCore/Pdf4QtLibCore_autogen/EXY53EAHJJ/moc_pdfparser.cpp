/****************************************************************************
** Meta object code from reading C++ file 'pdfparser.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../Pdf4QtLibCore/sources/pdfparser.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'pdfparser.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN3pdf18PDFLexicalAnalyzerE_t {};
} // unnamed namespace

template <> constexpr inline auto pdf::PDFLexicalAnalyzer::qt_create_metaobjectdata<qt_meta_tag_ZN3pdf18PDFLexicalAnalyzerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "pdf::PDFLexicalAnalyzer",
        "TokenType",
        "Boolean",
        "Integer",
        "Real",
        "String",
        "Name",
        "ArrayStart",
        "ArrayEnd",
        "DictionaryStart",
        "DictionaryEnd",
        "Null",
        "Command",
        "EndOfFile"
    };

    QtMocHelpers::UintData qt_methods {
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
        // enum 'TokenType'
        QtMocHelpers::EnumData<enum TokenType>(1, 1, QMC::EnumIsScoped).add({
            {    2, TokenType::Boolean },
            {    3, TokenType::Integer },
            {    4, TokenType::Real },
            {    5, TokenType::String },
            {    6, TokenType::Name },
            {    7, TokenType::ArrayStart },
            {    8, TokenType::ArrayEnd },
            {    9, TokenType::DictionaryStart },
            {   10, TokenType::DictionaryEnd },
            {   11, TokenType::Null },
            {   12, TokenType::Command },
            {   13, TokenType::EndOfFile },
        }),
    };
    return QtMocHelpers::metaObjectData<PDFLexicalAnalyzer, qt_meta_tag_ZN3pdf18PDFLexicalAnalyzerE_t>(QMC::PropertyAccessInStaticMetaCall, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject pdf::PDFLexicalAnalyzer::staticMetaObject = { {
    nullptr,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf18PDFLexicalAnalyzerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf18PDFLexicalAnalyzerE_t>.data,
    nullptr,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN3pdf18PDFLexicalAnalyzerE_t>.metaTypes,
    nullptr
} };

QT_WARNING_POP
