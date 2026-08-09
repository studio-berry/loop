/****************************************************************************
** Meta object code from reading C++ file 'pdfobjecteditormodel.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../Pdf4QtLibCore/sources/pdfobjecteditormodel.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'pdfobjecteditormodel.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN3pdf28PDFObjectEditorAbstractModelE_t {};
} // unnamed namespace

template <> constexpr inline auto pdf::PDFObjectEditorAbstractModel::qt_create_metaobjectdata<qt_meta_tag_ZN3pdf28PDFObjectEditorAbstractModelE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "pdf::PDFObjectEditorAbstractModel",
        "editedObjectChanged",
        ""
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'editedObjectChanged'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PDFObjectEditorAbstractModel, qt_meta_tag_ZN3pdf28PDFObjectEditorAbstractModelE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject pdf::PDFObjectEditorAbstractModel::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf28PDFObjectEditorAbstractModelE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf28PDFObjectEditorAbstractModelE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN3pdf28PDFObjectEditorAbstractModelE_t>.metaTypes,
    nullptr
} };

void pdf::PDFObjectEditorAbstractModel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PDFObjectEditorAbstractModel *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->editedObjectChanged(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PDFObjectEditorAbstractModel::*)()>(_a, &PDFObjectEditorAbstractModel::editedObjectChanged, 0))
            return;
    }
}

const QMetaObject *pdf::PDFObjectEditorAbstractModel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *pdf::PDFObjectEditorAbstractModel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf28PDFObjectEditorAbstractModelE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int pdf::PDFObjectEditorAbstractModel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 1)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 1;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 1)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 1;
    }
    return _id;
}

// SIGNAL 0
void pdf::PDFObjectEditorAbstractModel::editedObjectChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}
namespace {
struct qt_meta_tag_ZN3pdf31PDFObjectEditorAnnotationsModelE_t {};
} // unnamed namespace

template <> constexpr inline auto pdf::PDFObjectEditorAnnotationsModel::qt_create_metaobjectdata<qt_meta_tag_ZN3pdf31PDFObjectEditorAnnotationsModelE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "pdf::PDFObjectEditorAnnotationsModel"
    };

    QtMocHelpers::UintData qt_methods {
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PDFObjectEditorAnnotationsModel, qt_meta_tag_ZN3pdf31PDFObjectEditorAnnotationsModelE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject pdf::PDFObjectEditorAnnotationsModel::staticMetaObject = { {
    QMetaObject::SuperData::link<PDFObjectEditorAbstractModel::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf31PDFObjectEditorAnnotationsModelE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf31PDFObjectEditorAnnotationsModelE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN3pdf31PDFObjectEditorAnnotationsModelE_t>.metaTypes,
    nullptr
} };

void pdf::PDFObjectEditorAnnotationsModel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PDFObjectEditorAnnotationsModel *>(_o);
    (void)_t;
    (void)_c;
    (void)_id;
    (void)_a;
}

const QMetaObject *pdf::PDFObjectEditorAnnotationsModel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *pdf::PDFObjectEditorAnnotationsModel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf31PDFObjectEditorAnnotationsModelE_t>.strings))
        return static_cast<void*>(this);
    return PDFObjectEditorAbstractModel::qt_metacast(_clname);
}

int pdf::PDFObjectEditorAnnotationsModel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = PDFObjectEditorAbstractModel::qt_metacall(_c, _id, _a);
    return _id;
}
QT_WARNING_POP
