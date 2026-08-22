// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

#ifndef PDFINTERACTIONTRACEWIDGET_P_H
#define PDFINTERACTIONTRACEWIDGET_P_H

#include "pdfinteractiontrace_p.h"

class QWidget;
class QObject;

namespace pdf
{

inline constexpr const char* PDFInteractionTraceObjectName = "LoupeInteractionTraceRecorder";

PDFInteractionTraceRecorder* findInteractionTraceRecorder(QObject* widget);

/// Owns the two nested scopes used for a widget input event. Keeping this
/// instrumentation boundary out of PDFDrawWidget leaves the event handlers
/// responsible for input semantics rather than trace plumbing.
class PDFInteractionTraceInputScope final
{
public:
    PDFInteractionTraceInputScope(QObject* widget, PDFInteractionTraceRecorder::InputKind kind);

    PDFInteractionTraceInputScope(const PDFInteractionTraceInputScope&) = delete;
    PDFInteractionTraceInputScope& operator=(const PDFInteractionTraceInputScope&) = delete;

private:
    PDFInteractionTraceRecorder* m_recorder = nullptr;
    PDFInteractionTraceRecorder::InputScope m_inputScope;
    PDFInteractionTraceRecorder::StageScope m_stageScope;
};

void drawInteractionTraceOverlay(QWidget* widget, const QJsonObject& summary);

}   // namespace pdf

#endif   // PDFINTERACTIONTRACEWIDGET_P_H
