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

#ifndef PDFOVERLAYCOMPOSITOR_P_H
#define PDFOVERLAYCOMPOSITOR_P_H

#include "pdfdocumentdrawinterface.h"

#include <map>
#include <set>
#include <vector>

class QPainter;

namespace pdf
{

/// Orders and invokes transient overlay providers without making the page
/// layout controller own provider discovery or painter clipping policy.
class PDFOverlayCompositor final
{
public:
    struct Provider
    {
        IDocumentOverlayInterface* interface = nullptr;
        PDFOverlayLayer layer = PDFOverlayLayer::Findings;
        quint64 registrationOrder = 0;
    };

    static std::vector<Provider> collect(
        const std::set<IDocumentDrawInterface*>& drawInterfaces,
        const std::map<IDocumentDrawInterface*, quint64>& registrationOrder);

    static void draw(QPainter* painter,
                     const QRect& clipRect,
                     const PDFOverlayContext& context,
                     const std::vector<Provider>& providers);
};

}   // namespace pdf

#endif   // PDFOVERLAYCOMPOSITOR_P_H
