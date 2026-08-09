/****************************************************************************
** Meta object code from reading C++ file 'pdfdiff.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../Pdf4QtLibCore/sources/pdfdiff.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'pdfdiff.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN3pdf22PDFDiffResultNavigatorE_t {};
} // unnamed namespace

template <> constexpr inline auto pdf::PDFDiffResultNavigator::qt_create_metaobjectdata<qt_meta_tag_ZN3pdf22PDFDiffResultNavigatorE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "pdf::PDFDiffResultNavigator",
        "selectionChanged",
        "",
        "size_t",
        "currentIndex"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'selectionChanged'
        QtMocHelpers::SignalData<void(size_t)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PDFDiffResultNavigator, qt_meta_tag_ZN3pdf22PDFDiffResultNavigatorE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject pdf::PDFDiffResultNavigator::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf22PDFDiffResultNavigatorE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf22PDFDiffResultNavigatorE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN3pdf22PDFDiffResultNavigatorE_t>.metaTypes,
    nullptr
} };

void pdf::PDFDiffResultNavigator::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PDFDiffResultNavigator *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->selectionChanged((*reinterpret_cast<std::add_pointer_t<size_t>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PDFDiffResultNavigator::*)(size_t )>(_a, &PDFDiffResultNavigator::selectionChanged, 0))
            return;
    }
}

const QMetaObject *pdf::PDFDiffResultNavigator::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *pdf::PDFDiffResultNavigator::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf22PDFDiffResultNavigatorE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int pdf::PDFDiffResultNavigator::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
void pdf::PDFDiffResultNavigator::selectionChanged(size_t _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}
namespace {
struct qt_meta_tag_ZN3pdf7PDFDiffE_t {};
} // unnamed namespace

template <> constexpr inline auto pdf::PDFDiff::qt_create_metaobjectdata<qt_meta_tag_ZN3pdf7PDFDiffE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "pdf::PDFDiff",
        "comparationFinished",
        ""
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'comparationFinished'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PDFDiff, qt_meta_tag_ZN3pdf7PDFDiffE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject pdf::PDFDiff::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf7PDFDiffE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf7PDFDiffE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN3pdf7PDFDiffE_t>.metaTypes,
    nullptr
} };

void pdf::PDFDiff::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PDFDiff *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->comparationFinished(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PDFDiff::*)()>(_a, &PDFDiff::comparationFinished, 0))
            return;
    }
}

const QMetaObject *pdf::PDFDiff::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *pdf::PDFDiff::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN3pdf7PDFDiffE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int pdf::PDFDiff::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
void pdf::PDFDiff::comparationFinished()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}
QT_WARNING_POP
