/****************************************************************************
** Meta object code from reading C++ file 'pdfdocumentsanitizer.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../Pdf4QtLibCore/sources/pdfdocumentsanitizer.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'pdfdocumentsanitizer.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN3pdf20PDFDocumentSanitizerE_t {};
} // unnamed namespace

template <> constexpr inline auto pdf::PDFDocumentSanitizer::qt_create_metaobjectdata<qt_meta_tag_ZN3pdf20PDFDocumentSanitizerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "pdf::PDFDocumentSanitizer",
        "sanitizationStarted",
        "",
        "sanitizationProgress",
        "progressText",
        "sanitizationFinished"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'sanitizationStarted'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'sanitizationProgress'
        QtMocHelpers::SignalData<void(QString)>(3, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 4 },
        }}),
        // Signal 'sanitizationFinished'
        QtMocHelpers::SignalData<void()>(5, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PDFDocumentSanitizer, qt_meta_tag_ZN3pdf20PDFDocumentSanitizerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject pdf::PDFDocumentSanitizer::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf20PDFDocumentSanitizerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf20PDFDocumentSanitizerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN3pdf20PDFDocumentSanitizerE_t>.metaTypes,
    nullptr
} };

void pdf::PDFDocumentSanitizer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PDFDocumentSanitizer *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->sanitizationStarted(); break;
        case 1: _t->sanitizationProgress((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 2: _t->sanitizationFinished(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PDFDocumentSanitizer::*)()>(_a, &PDFDocumentSanitizer::sanitizationStarted, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (PDFDocumentSanitizer::*)(QString )>(_a, &PDFDocumentSanitizer::sanitizationProgress, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (PDFDocumentSanitizer::*)()>(_a, &PDFDocumentSanitizer::sanitizationFinished, 2))
            return;
    }
}

const QMetaObject *pdf::PDFDocumentSanitizer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *pdf::PDFDocumentSanitizer::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf20PDFDocumentSanitizerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int pdf::PDFDocumentSanitizer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 3)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 3;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 3)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 3;
    }
    return _id;
}

// SIGNAL 0
void pdf::PDFDocumentSanitizer::sanitizationStarted()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void pdf::PDFDocumentSanitizer::sanitizationProgress(QString _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void pdf::PDFDocumentSanitizer::sanitizationFinished()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}
QT_WARNING_POP
