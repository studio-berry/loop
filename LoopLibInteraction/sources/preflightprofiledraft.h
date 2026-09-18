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

#ifndef PREFLIGHTPROFILEDRAFT_H
#define PREFLIGHTPROFILEDRAFT_H

#include "preflightprofileresolver.h"

#include <QJsonObject>
#include <QVariantList>

namespace pdfinteraction
{

class PreflightProfileDraft
{
public:
    bool load(const QJsonObject& parentProfile, const pdf::PreflightProfileIdentity& parentIdentity);
    void clear();

    bool isActive() const { return m_active; }
    bool isDirty() const { return m_dirty; }
    const QJsonObject& parentProfile() const { return m_parentProfile; }
    const QJsonObject& draftProfile() const { return m_draftProfile; }
    const pdf::PreflightProfileIdentity& parentIdentity() const { return m_parentIdentity; }
    QString suggestedNextVersion() const;

    QVariantList editableChecks() const;
    bool setCheckField(const QString& checkId, const QString& field, const QVariant& value);
    QJsonObject commit(const QString& newVersion) const;

private:
    bool m_active = false;
    bool m_dirty = false;
    QJsonObject m_parentProfile;
    QJsonObject m_draftProfile;
    pdf::PreflightProfileIdentity m_parentIdentity;
};

}   // namespace pdfinteraction

#endif   // PREFLIGHTPROFILEDRAFT_H
