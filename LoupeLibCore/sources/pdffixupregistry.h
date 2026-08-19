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
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef PDFFIXUPREGISTRY_H
#define PDFFIXUPREGISTRY_H

#include "pdfglobal.h"

#include <QList>
#include <QString>

namespace pdf
{

struct LOUPELIBCORESHARED_EXPORT PDFFixupCapability
{
    QString id;
    bool implemented = false;
    bool destructive = true;
    bool supportsDryRun = false;
    bool supportsReport = false;
};

/// Returns the fixups implemented by the current build.
LOUPELIBCORESHARED_EXPORT QList<PDFFixupCapability> getImplementedFixupCapabilities();

/// Returns true when the ID is backed by an implemented fixup in this build.
LOUPELIBCORESHARED_EXPORT bool isImplementedFixupId(const QString& fixupId);

} // namespace pdf

#endif // PDFFIXUPREGISTRY_H
