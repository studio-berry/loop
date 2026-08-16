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

#include "preflightprofileresolver.h"

#include "preflightengine.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pdf
{

namespace
{

constexpr qint64 MAX_PROFILE_BYTES = 4LL * 1024 * 1024;
constexpr int MAX_SELECTOR_FIELDS = 16;
constexpr int MAX_CONTEXT_ATTRIBUTES = 64;

QString normalizeId(const QString& value)
{
    return value.normalized(QString::NormalizationForm_C).trimmed().toCaseFolded();
}

QString contextValue(const PreflightJobContext& context, const QString& key)
{
    if (key == QStringLiteral("client_id"))
        return context.clientId;
    if (key == QStringLiteral("product_id"))
        return context.productId;
    if (key == QStringLiteral("job_type"))
        return context.jobType;
    if (key == QStringLiteral("press_id"))
        return context.pressId;
    if (key == QStringLiteral("stock_id"))
        return context.stockId;
    if (key == QStringLiteral("finishing_id"))
        return context.finishingId;
    return {};
}

QJsonValue normalizedAttributeValue(const QJsonValue& value)
{
    if (value.isString())
    {
        return normalizeId(value.toString());
    }

    if (value.isArray())
    {
        QJsonArray result;
        for (const QJsonValue& item : value.toArray())
        {
            result.append(normalizedAttributeValue(item));
        }
        return result;
    }

    return value;
}

bool isEqualJson(const QJsonValue& left, const QJsonValue& right)
{
    return canonicalPreflightJson(left) == canonicalPreflightJson(right);
}

bool parseIdValue(const QJsonObject& object, const QStringList& keys, QString& target, QString& errorMessage)
{
    for (const QString& key : keys)
    {
        if (!object.contains(key))
        {
            continue;
        }

        const QJsonValue value = object.value(key);
        if (!value.isString())
        {
            errorMessage = QStringLiteral("Context field '%1' must be a string.").arg(key);
            return false;
        }

        target = normalizeId(value.toString());
        if (target.isEmpty())
        {
            errorMessage = QStringLiteral("Context field '%1' must not be empty.").arg(key);
            return false;
        }
        break;
    }

    return true;
}

bool validateSelectorValue(const QJsonValue& value, const QString& path, QString& errorMessage)
{
    if (value.isString())
    {
        if (normalizeId(value.toString()).isEmpty())
        {
            errorMessage = QStringLiteral("Selector '%1' must not contain an empty value.").arg(path);
            return false;
        }
        return true;
    }

    if (!value.isArray() || value.toArray().isEmpty())
    {
        errorMessage = QStringLiteral("Selector '%1' must be a string or non-empty string array.").arg(path);
        return false;
    }

    for (const QJsonValue& item : value.toArray())
    {
        if (!item.isString() || normalizeId(item.toString()).isEmpty())
        {
            errorMessage = QStringLiteral("Selector '%1' contains an invalid value.").arg(path);
            return false;
        }
    }

    return true;
}

bool validateSelector(const QJsonObject& selector, QString& errorMessage)
{
    if (selector.size() > MAX_SELECTOR_FIELDS)
    {
        errorMessage = QStringLiteral("Profile selector contains too many fields.");
        return false;
    }

    static const QStringList allowedKeys = {
        QStringLiteral("client_id"),
        QStringLiteral("product_id"),
        QStringLiteral("job_type"),
        QStringLiteral("press_id"),
        QStringLiteral("stock_id"),
        QStringLiteral("finishing_id"),
        QStringLiteral("attributes")
    };

    for (auto iterator = selector.constBegin(); iterator != selector.constEnd(); ++iterator)
    {
        if (!allowedKeys.contains(iterator.key()))
        {
            errorMessage = QStringLiteral("Selector field '%1' is not supported.").arg(iterator.key());
            return false;
        }

        if (iterator.key() == QStringLiteral("attributes"))
        {
            if (!iterator.value().isObject() || iterator.value().toObject().size() > MAX_CONTEXT_ATTRIBUTES)
            {
                errorMessage = QStringLiteral("Selector attributes must be a bounded object.");
                return false;
            }
            for (auto attribute = iterator.value().toObject().constBegin();
                 attribute != iterator.value().toObject().constEnd(); ++attribute)
            {
                if (!validateSelectorValue(attribute.value(), QStringLiteral("attributes.%1").arg(attribute.key()), errorMessage))
                {
                    return false;
                }
            }
        }
        else if (!validateSelectorValue(iterator.value(), iterator.key(), errorMessage))
        {
            return false;
        }
    }

    return true;
}

bool profileSourceFromJson(const QJsonObject& profileObject,
                           const QString& fallbackId,
                           PreflightProfileSource& source,
                           QString& errorMessage)
{
    if (profileObject.isEmpty())
    {
        errorMessage = QStringLiteral("Profile source must be a non-empty JSON object.");
        return false;
    }

    source = PreflightProfileSource();
    source.profile = profileObject;
    source.id = profileObject.value(QStringLiteral("id")).toString().trimmed();
    if (source.id.isEmpty())
    {
        source.id = profileObject.value(QStringLiteral("profile_id")).toString().trimmed();
    }
    if (source.id.isEmpty())
    {
        source.id = fallbackId;
    }
    if (source.id.isEmpty())
    {
        errorMessage = QStringLiteral("Profile source is missing a stable profile_id.");
        return false;
    }
    source.version = profileObject.value(QStringLiteral("version")).toString();
    if (source.version.isEmpty())
    {
        source.version = profileObject.value(QStringLiteral("profile_version")).toString();
    }
    if (source.version.isEmpty())
    {
        source.version = QString::number(profileObject.value(QStringLiteral("schema_version")).toInt(1));
    }
    source.priority = profileObject.value(QStringLiteral("priority")).toInt(0);
    source.selector = profileObject.value(QStringLiteral("selector")).toObject();
    if (profileObject.contains(QStringLiteral("selector")) && !profileObject.value(QStringLiteral("selector")).isObject())
    {
        errorMessage = QStringLiteral("Profile field 'selector' must be an object.");
        return false;
    }
    if (!validateSelector(source.selector, errorMessage))
    {
        return false;
    }

    PreflightEngine validator(nullptr);
    PreflightProfileData parsedProfile;
    if (!validator.parseProfile(profileObject, parsedProfile, errorMessage))
    {
        // Empty checks are classified by PreflightEngine::run(), not as a resolver error.
        const bool isEmptyChecksOnly = !profileObject.value(QStringLiteral("name")).toString().isEmpty() && profileObject.value(QStringLiteral("checks")).toArray().isEmpty();
        if (!isEmptyChecksOnly)
        {
            return false;
        }
    }

    source.contentHash = QCryptographicHash::hash(canonicalPreflightJson(profileObject), QCryptographicHash::Sha256).toHex();
    return true;
}

QJsonObject profilePolicy(const QJsonObject& sourceProfile)
{
    QJsonObject result = sourceProfile;
    result.remove(QStringLiteral("profile_id"));
    result.remove(QStringLiteral("profile_version"));
    result.remove(QStringLiteral("priority"));
    result.remove(QStringLiteral("selector"));
    // Digest identifies an authored source file, not a merged effective profile.
    result.remove(QStringLiteral("digest"));
    return result;
}

struct RankedSource
{
    PreflightProfileSource source;
    int specificity = 0;
};

struct Contribution
{
    QJsonValue value;
    QString sourceId;
    int priority = 0;
    int specificity = 0;
};

bool isHigherAuthority(const RankedSource& candidate, const Contribution& current)
{
    return candidate.source.priority > current.priority || (candidate.source.priority == current.priority && candidate.specificity > current.specificity);
}

QString sourceLabel(const RankedSource& source)
{
    return source.source.id + QStringLiteral("@") + source.source.version;
}

bool mergeValue(QJsonObject& target,
                const QString& key,
                const QJsonValue& incoming,
                const RankedSource& source,
                const QString& path,
                QMap<QString, Contribution>& contributions,
                QList<PreflightResolutionDecision>& decisions,
                QString& errorMessage);

bool mergeObject(QJsonObject& target,
                 const QJsonObject& incoming,
                 const RankedSource& source,
                 const QString& basePath,
                 QMap<QString, Contribution>& contributions,
                 QList<PreflightResolutionDecision>& decisions,
                 QString& errorMessage)
{
    const QStringList keys = incoming.keys();
    for (const QString& key : keys)
    {
        const QString path = basePath + QLatin1Char('/') + key;
        if (!mergeValue(target, key, incoming.value(key), source, path, contributions, decisions, errorMessage))
        {
            return false;
        }
    }
    return true;
}

bool mergeValue(QJsonObject& target,
                const QString& key,
                const QJsonValue& incoming,
                const RankedSource& source,
                const QString& path,
                QMap<QString, Contribution>& contributions,
                QList<PreflightResolutionDecision>& decisions,
                QString& errorMessage)
{
    if (incoming.isObject() && !incoming.toObject().isEmpty() && (!target.contains(key) || target.value(key).isObject()))
    {
        QJsonObject child = target.value(key).toObject();
        if (!mergeObject(child, incoming.toObject(), source, path, contributions, decisions, errorMessage))
        {
            return false;
        }
        target.insert(key, child);
        return true;
    }

    const auto existing = contributions.constFind(path);
    if (existing == contributions.constEnd())
    {
        target.insert(key, incoming);
        contributions.insert(path, Contribution{ incoming, source.source.id, source.source.priority, source.specificity });
        decisions.append(PreflightResolutionDecision{ path, incoming, source.source.id, {}, QStringLiteral("supplied") });
        return true;
    }

    const Contribution& previous = existing.value();
    const bool sameAuthority = previous.priority == source.source.priority && previous.specificity == source.specificity;
    if (sameAuthority && !isEqualJson(previous.value, incoming))
    {
        errorMessage = QStringLiteral("Ambiguous profile resolution at '%1': '%2' and '%3' have equal authority.")
                           .arg(path, previous.sourceId, sourceLabel(source));
        return false;
    }

    if (isHigherAuthority(source, previous))
    {
        target.insert(key, incoming);
        contributions.insert(path, Contribution{ incoming, source.source.id, source.source.priority, source.specificity });
        decisions.append(PreflightResolutionDecision{ path, incoming, source.source.id, previous.sourceId, QStringLiteral("higher-precedence override") });
    }

    return true;
}

bool matchesSelectorValue(const QJsonValue& selectorValue, const QString& contextValueText)
{
    const QString normalizedContext = normalizeId(contextValueText);
    if (selectorValue.isString())
    {
        return normalizeId(selectorValue.toString()) == normalizedContext;
    }

    for (const QJsonValue& item : selectorValue.toArray())
    {
        if (normalizeId(item.toString()) == normalizedContext)
        {
            return true;
        }
    }
    return false;
}

bool sourceMatches(const PreflightProfileSource& source,
                   const PreflightJobContext& context,
                   int& specificity,
                   QString& errorMessage)
{
    specificity = 0;
    if (!validateSelector(source.selector, errorMessage))
    {
        return false;
    }

    for (auto iterator = source.selector.constBegin(); iterator != source.selector.constEnd(); ++iterator)
    {
        if (iterator.key() == QStringLiteral("attributes"))
        {
            const QJsonObject attributes = iterator.value().toObject();
            for (auto attribute = attributes.constBegin(); attribute != attributes.constEnd(); ++attribute)
            {
                const auto contextAttribute = context.attributes.constFind(attribute.key());
                if (contextAttribute == context.attributes.constEnd() || !contextAttribute.value().isString() || !matchesSelectorValue(attribute.value(), contextAttribute.value().toString()))
                {
                    return false;
                }
                ++specificity;
            }
            continue;
        }

        const QString value = contextValue(context, iterator.key());
        if (value.isEmpty() || !matchesSelectorValue(iterator.value(), value))
        {
            return false;
        }
        ++specificity;
    }

    return true;
}

bool mergeIdentifiedArray(QJsonObject& target,
                          const QString& key,
                          const QList<RankedSource>& sources,
                          QMap<QString, Contribution>& contributions,
                          QList<PreflightResolutionDecision>& decisions,
                          QString& errorMessage)
{
    // QMap iterates in key-sorted (alphabetical id) order, which would silently
    // reorder the profile's declared check/fixup order. Track first-appearance
    // order separately so the merged array reads in the same order a human
    // reading the source profile(s) would expect.
    QMap<QString, QJsonObject> items;
    QStringList order;
    for (const RankedSource& source : sources)
    {
        const QJsonArray array = source.source.profile.value(key).toArray();
        for (const QJsonValue& value : array)
        {
            if (!value.isObject() || value.toObject().value(QStringLiteral("id")).toString().isEmpty())
            {
                errorMessage = QStringLiteral("Profile '%1' contains an invalid %2 entry.").arg(sourceLabel(source), key);
                return false;
            }

            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            if (!items.contains(id))
            {
                order.append(id);
            }
            QJsonObject current = items.value(id);
            if (!mergeObject(current, value.toObject(), source,
                             QStringLiteral("/%1/%2").arg(key, id), contributions, decisions, errorMessage))
            {
                return false;
            }
            items.insert(id, current);
        }
    }

    QJsonArray result;
    for (const QString& id : order)
    {
        result.append(items.value(id));
    }
    target.insert(key, result);
    return true;
}

PreflightResolvedProfile resolveMatched(const QList<RankedSource>& matched,
                                        const PreflightJobContext& normalizedContext,
                                        const QString& mode)
{
    PreflightResolvedProfile result;
    result.resolutionMode = mode;
    result.normalizedContext = normalizedContext.toJson();
    result.matchedSources.reserve(matched.size());
    for (const RankedSource& source : matched)
    {
        result.matchedSources.append(source.source);
    }

    QJsonObject effective;
    QMap<QString, Contribution> contributions;
    QString errorMessage;
    if (!matched.isEmpty())
    {
        for (const RankedSource& source : matched)
        {
            const QJsonObject policy = profilePolicy(source.source.profile);
            for (const QString& key : policy.keys())
            {
                if (key == QStringLiteral("checks") || key == QStringLiteral("fixups"))
                {
                    continue;
                }
                if (key == QStringLiteral("job_types"))
                {
                    QSet<QString> values;
                    for (const RankedSource& existing : matched)
                    {
                        for (const QJsonValue& value : existing.source.profile.value(key).toArray())
                        {
                            values.insert(value.toString());
                        }
                    }
                    QJsonArray jobTypes;
                    QStringList sortedValues = values.values();
                    sortedValues.sort(Qt::CaseSensitive);
                    for (const QString& value : sortedValues)
                    {
                        jobTypes.append(value);
                    }
                    effective.insert(key, jobTypes);
                    continue;
                }
                if (!mergeValue(effective, key, policy.value(key), source, QStringLiteral("/") + key,
                                contributions, result.decisions, errorMessage))
                {
                    result.errorCode = QStringLiteral("ambiguous-profile");
                    result.errorMessage = errorMessage;
                    return result;
                }
            }
        }

        if (!mergeIdentifiedArray(effective, QStringLiteral("checks"), matched, contributions, result.decisions, errorMessage) || !mergeIdentifiedArray(effective, QStringLiteral("fixups"), matched, contributions, result.decisions, errorMessage))
        {
            result.errorCode = QStringLiteral("invalid-profile-merge");
            result.errorMessage = errorMessage;
            return result;
        }
    }

    PreflightEngine validator(nullptr);
    PreflightProfileData parsed;
    if (!validator.parseProfile(effective, parsed, errorMessage))
    {
        // Empty checks are classified by PreflightEngine::run(), not as a resolver error.
        const bool isEmptyChecksOnly = !effective.value(QStringLiteral("name")).toString().isEmpty() && effective.value(QStringLiteral("checks")).toArray().isEmpty();
        if (!isEmptyChecksOnly)
        {
            result.errorCode = QStringLiteral("invalid-effective-profile");
            result.errorMessage = errorMessage;
            return result;
        }
    }

    result.effectiveProfile = effective;
    result.effectiveHash = QCryptographicHash::hash(canonicalPreflightJson(effective), QCryptographicHash::Sha256).toHex();
    std::sort(result.decisions.begin(), result.decisions.end(), [](const auto& left, const auto& right)
              {
        if (left.jsonPointer != right.jsonPointer) return left.jsonPointer < right.jsonPointer;
        if (left.sourceId != right.sourceId) return left.sourceId < right.sourceId;
        return left.overriddenSourceId < right.overriddenSourceId; });
    result.ok = true;
    return result;
}

}   // namespace

QJsonValue canonicalizePreflightJson(const QJsonValue& value)
{
    if (value.isObject())
    {
        QJsonObject sorted;
        const QStringList keys = value.toObject().keys();
        for (const QString& key : keys)
        {
            sorted.insert(key, canonicalizePreflightJson(value.toObject().value(key)));
        }
        return sorted;
    }

    if (value.isArray())
    {
        QJsonArray array;
        for (const QJsonValue& item : value.toArray())
        {
            array.append(canonicalizePreflightJson(item));
        }
        return array;
    }

    return value;
}

QByteArray canonicalPreflightJson(const QJsonValue& value)
{
    if (value.isObject())
    {
        return QJsonDocument(canonicalizePreflightJson(value).toObject()).toJson(QJsonDocument::Compact);
    }
    if (value.isArray())
    {
        return QJsonDocument(canonicalizePreflightJson(value).toArray()).toJson(QJsonDocument::Compact);
    }
    return QJsonDocument(QJsonArray{ canonicalizePreflightJson(value) }).toJson(QJsonDocument::Compact);
}

QJsonObject PreflightJobContext::toJson() const
{
    QJsonObject object;
    if (!clientId.isEmpty())
        object.insert(QStringLiteral("client_id"), clientId);
    if (!productId.isEmpty())
        object.insert(QStringLiteral("product_id"), productId);
    if (!jobType.isEmpty())
        object.insert(QStringLiteral("job_type"), jobType);
    if (!pressId.isEmpty())
        object.insert(QStringLiteral("press_id"), pressId);
    if (!stockId.isEmpty())
        object.insert(QStringLiteral("stock_id"), stockId);
    if (!finishingId.isEmpty())
        object.insert(QStringLiteral("finishing_id"), finishingId);
    if (!attributes.isEmpty())
    {
        QJsonObject attributeObject;
        for (auto iterator = attributes.constBegin(); iterator != attributes.constEnd(); ++iterator)
        {
            attributeObject.insert(iterator.key(), normalizedAttributeValue(iterator.value()));
        }
        object.insert(QStringLiteral("attributes"), attributeObject);
    }
    return object;
}

bool PreflightJobContext::fromJson(const QJsonObject& object,
                                   PreflightJobContext& context,
                                   QString& errorMessage)
{
    context = PreflightJobContext();
    if (object.size() > 32)
    {
        errorMessage = QStringLiteral("Job context contains too many fields.");
        return false;
    }

    static const QStringList allowedKeys = {
        QStringLiteral("client_id"), QStringLiteral("client"),
        QStringLiteral("product_id"), QStringLiteral("product"),
        QStringLiteral("job_type"), QStringLiteral("job-type"),
        QStringLiteral("press_id"), QStringLiteral("press"),
        QStringLiteral("stock_id"), QStringLiteral("stock"),
        QStringLiteral("finishing_id"), QStringLiteral("finishing"),
        QStringLiteral("attributes")
    };
    for (const QString& key : object.keys())
    {
        if (!allowedKeys.contains(key))
        {
            errorMessage = QStringLiteral("Job context field '%1' is not supported.").arg(key);
            return false;
        }
    }

    if (!parseIdValue(object, { QStringLiteral("client_id"), QStringLiteral("client") }, context.clientId, errorMessage) || !parseIdValue(object, { QStringLiteral("product_id"), QStringLiteral("product") }, context.productId, errorMessage) || !parseIdValue(object, { QStringLiteral("job_type"), QStringLiteral("job-type") }, context.jobType, errorMessage) || !parseIdValue(object, { QStringLiteral("press_id"), QStringLiteral("press") }, context.pressId, errorMessage) || !parseIdValue(object, { QStringLiteral("stock_id"), QStringLiteral("stock") }, context.stockId, errorMessage) || !parseIdValue(object, { QStringLiteral("finishing_id"), QStringLiteral("finishing") }, context.finishingId, errorMessage))
    {
        return false;
    }

    if (object.contains(QStringLiteral("attributes")))
    {
        const QJsonValue value = object.value(QStringLiteral("attributes"));
        if (!value.isObject() || value.toObject().size() > MAX_CONTEXT_ATTRIBUTES)
        {
            errorMessage = QStringLiteral("Job context attributes must be a bounded object.");
            return false;
        }
        for (auto iterator = value.toObject().constBegin(); iterator != value.toObject().constEnd(); ++iterator)
        {
            if (iterator.key().size() > 128 || (iterator.value().isString() && iterator.value().toString().size() > 512))
            {
                errorMessage = QStringLiteral("Job context attribute '%1' is too large.").arg(iterator.key());
                return false;
            }
            context.attributes.insert(normalizeId(iterator.key()), normalizedAttributeValue(iterator.value()));
        }
    }

    return true;
}

QJsonObject PreflightProfileSource::toJson() const
{
    return QJsonObject{
        { QStringLiteral("id"), id },
        { QStringLiteral("version"), version },
        { QStringLiteral("hash"), QString::fromLatin1(contentHash) },
        { QStringLiteral("priority"), priority }
    };
}

QJsonObject PreflightResolutionDecision::toJson() const
{
    QJsonObject object{
        { QStringLiteral("path"), jsonPointer },
        { QStringLiteral("source"), sourceId },
        { QStringLiteral("reason"), reason }
    };
    if (!overriddenSourceId.isEmpty())
    {
        object.insert(QStringLiteral("overrode"), overriddenSourceId);
    }
    object.insert(QStringLiteral("value"), value);
    return object;
}

QJsonObject PreflightResolvedProfile::provenance() const
{
    QJsonArray matched;
    for (const PreflightProfileSource& source : matchedSources)
    {
        matched.append(source.toJson());
    }

    QJsonArray decisionsJson;
    for (const PreflightResolutionDecision& decision : decisions)
    {
        decisionsJson.append(decision.toJson());
    }

    QJsonObject object{
        { QStringLiteral("resolver_version"), resolverVersion },
        { QStringLiteral("mode"), resolutionMode },
        { QStringLiteral("context"), normalizedContext },
        { QStringLiteral("matched_sources"), matched },
        { QStringLiteral("effective_profile"), QJsonObject{
                                                   { QStringLiteral("id"), QStringLiteral("resolved") },
                                                   { QStringLiteral("hash"), QString::fromLatin1(effectiveHash) } } },
        { QStringLiteral("decisions"), decisionsJson }
    };
    if (!ok)
    {
        object.insert(QStringLiteral("error"), QJsonObject{
                                                   { QStringLiteral("code"), errorCode },
                                                   { QStringLiteral("message"), errorMessage } });
    }
    return object;
}

bool PreflightProfileStore::loadDirectory(const QString& directoryPath,
                                          PreflightProfileSnapshot& snapshot,
                                          QString& errorMessage)
{
    snapshot = PreflightProfileSnapshot();
    const QFileInfo directoryInfo(directoryPath);
    if (!directoryInfo.isDir())
    {
        errorMessage = QStringLiteral("Profile store '%1' is not a directory.").arg(directoryPath);
        return false;
    }

    const QFileInfoList files = QDir(directoryPath).entryInfoList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
    if (files.isEmpty())
    {
        errorMessage = QStringLiteral("Profile store '%1' contains no JSON profiles.").arg(directoryPath);
        return false;
    }

    QMap<QString, QByteArray> identityHashes;
    for (const QFileInfo& fileInfo : files)
    {
        if (fileInfo.size() > MAX_PROFILE_BYTES)
        {
            errorMessage = QStringLiteral("Profile '%1' exceeds the maximum supported size.").arg(fileInfo.fileName());
            return false;
        }

        QFile file(fileInfo.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
        {
            errorMessage = QStringLiteral("Cannot open profile '%1'.").arg(fileInfo.fileName());
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            errorMessage = QStringLiteral("Invalid profile JSON in '%1': %2").arg(fileInfo.fileName(), parseError.errorString());
            return false;
        }

        PreflightProfileSource source;
        if (!profileSourceFromJson(document.object(), fileInfo.completeBaseName(), source, errorMessage))
        {
            errorMessage = QStringLiteral("Profile '%1': %2").arg(fileInfo.fileName(), errorMessage);
            return false;
        }

        const QString identity = source.id + QLatin1Char('@') + source.version;
        if (identityHashes.contains(identity))
        {
            errorMessage = QStringLiteral("Profile store contains duplicate source identity '%1'.").arg(identity);
            return false;
        }
        identityHashes.insert(identity, source.contentHash);
        snapshot.sources.append(source);
    }

    std::sort(snapshot.sources.begin(), snapshot.sources.end(), [](const auto& left, const auto& right)
              {
        if (left.id != right.id) return left.id < right.id;
        return left.version < right.version; });
    return true;
}

PreflightResolvedProfile PreflightProfileResolver::resolve(const PreflightJobContext& context,
                                                           const PreflightProfileSnapshot& snapshot) const
{
    PreflightJobContext normalizedContext = context;
    normalizedContext.clientId = normalizeId(normalizedContext.clientId);
    normalizedContext.productId = normalizeId(normalizedContext.productId);
    normalizedContext.jobType = normalizeId(normalizedContext.jobType);
    normalizedContext.pressId = normalizeId(normalizedContext.pressId);
    normalizedContext.stockId = normalizeId(normalizedContext.stockId);
    normalizedContext.finishingId = normalizeId(normalizedContext.finishingId);

    QList<RankedSource> matched;
    for (const PreflightProfileSource& source : snapshot.sources)
    {
        int specificity = 0;
        QString errorMessage;
        PreflightProfileSource parsedSource;
        if (!profileSourceFromJson(source.profile, source.id, parsedSource, errorMessage))
        {
            PreflightResolvedProfile invalid;
            invalid.normalizedContext = normalizedContext.toJson();
            invalid.errorCode = QStringLiteral("invalid-profile-source");
            invalid.errorMessage = errorMessage;
            return invalid;
        }
        if (sourceMatches(parsedSource, normalizedContext, specificity, errorMessage))
        {
            matched.append(RankedSource{ parsedSource, specificity });
        }
        else if (!errorMessage.isEmpty())
        {
            PreflightResolvedProfile invalid;
            invalid.normalizedContext = normalizedContext.toJson();
            invalid.errorCode = QStringLiteral("invalid-selector");
            invalid.errorMessage = errorMessage;
            return invalid;
        }
    }

    std::sort(matched.begin(), matched.end(), [](const RankedSource& left, const RankedSource& right)
              {
        if (left.source.priority != right.source.priority) return left.source.priority < right.source.priority;
        if (left.specificity != right.specificity) return left.specificity < right.specificity;
        if (left.source.id != right.source.id) return left.source.id < right.source.id;
        return left.source.version < right.source.version; });

    if (matched.isEmpty())
    {
        PreflightResolvedProfile invalid;
        invalid.normalizedContext = normalizedContext.toJson();
        invalid.errorCode = QStringLiteral("no-matching-profile");
        invalid.errorMessage = QStringLiteral("No profile source matches the supplied production context.");
        return invalid;
    }

    return resolveMatched(matched, normalizedContext, QStringLiteral("contextual"));
}

PreflightResolvedProfile PreflightProfileResolver::resolveExplicitProfile(const QJsonObject& profile,
                                                                          const QString& sourceId,
                                                                          const QString& version) const
{
    PreflightProfileSource source;
    QString errorMessage;
    if (!profileSourceFromJson(profile, sourceId, source, errorMessage))
    {
        PreflightResolvedProfile invalid;
        invalid.resolutionMode = QStringLiteral("explicit");
        invalid.errorCode = QStringLiteral("invalid-explicit-profile");
        invalid.errorMessage = errorMessage;
        return invalid;
    }
    source.id = sourceId.isEmpty() ? source.id : sourceId;
    source.version = version.isEmpty() ? source.version : version;
    source.selector = {};
    PreflightJobContext emptyContext;
    PreflightResolvedProfile result = resolveMatched({ RankedSource{ source, 0 } }, emptyContext, QStringLiteral("explicit"));
    result.resolutionMode = QStringLiteral("explicit");
    return result;
}

namespace
{

QJsonObject materializeCheckDefaults(QJsonObject check)
{
    if (!check.contains(QStringLiteral("enabled")))
    {
        check.insert(QStringLiteral("enabled"), true);
    }
    if (!check.contains(QStringLiteral("severity")))
    {
        check.insert(QStringLiteral("severity"), QStringLiteral("error"));
    }

    const QString id = check.value(QStringLiteral("id")).toString();
    const auto insertDefault = [&check](const QString& key, const QJsonValue& value)
    {
        if (!check.contains(key))
        {
            check.insert(key, value);
        }
    };

    if (id == QLatin1String("content-bleed") || id == QLatin1String("ink-coverage") || id == QLatin1String("color-inventory"))
    {
        insertDefault(QStringLiteral("probe_dpi"), 150);
    }
    if (id == QLatin1String("content-bleed"))
    {
        insertDefault(QStringLiteral("raster_confirm"), false);
        insertDefault(QStringLiteral("probe_threshold"), 16);
    }
    if (id == QLatin1String("color-inventory"))
    {
        insertDefault(QStringLiteral("rich_black_k_percent"), 10);
    }
    if (id == QLatin1String("ink-coverage"))
    {
        insertDefault(QStringLiteral("min_region_area_pct"), 0.05);
        insertDefault(QStringLiteral("max_regions_per_page"), 20);
        insertDefault(QStringLiteral("max_raster_pixels"), 250000000);
        insertDefault(QStringLiteral("analysis_box"), QStringLiteral("bleed"));
    }
    if (id == QLatin1String("output-intent"))
    {
        insertDefault(QStringLiteral("require_embedded_profile"), true);
        insertDefault(QStringLiteral("allow_multiple"), true);
    }
    if (id == QLatin1String("thin-strokes") || id == QLatin1String("thin-parts"))
    {
        insertDefault(QStringLiteral("zero_width_epsilon_pt"), 0.000001);
    }
    if (id == QLatin1String("thin-parts") && !check.contains(QStringLiteral("classes")))
    {
        check.insert(QStringLiteral("classes"), QJsonArray{ QStringLiteral("thin-stroke"), QStringLiteral("thin-fill") });
    }
    return check;
}

bool isValidProfileId(const QString& id)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z][a-z0-9-]*$"));
    return pattern.match(id).hasMatch();
}

bool isValidProfileVersion(const QString& version)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+$"));
    return pattern.match(version).hasMatch();
}

QString stemFromSourcePath(const QString& sourcePath)
{
    if (sourcePath.isEmpty())
    {
        return QStringLiteral("unnamed-profile");
    }
    QString stem = QFileInfo(sourcePath).completeBaseName().trimmed().toLower();
    stem.replace(QRegularExpression(QStringLiteral("[^a-z0-9-]+")), QStringLiteral("-"));
    stem.remove(QRegularExpression(QStringLiteral("^-+")));
    stem.remove(QRegularExpression(QStringLiteral("-+$")));
    if (stem.isEmpty() || !stem[0].isLetter())
    {
        stem = QStringLiteral("profile-") + stem;
    }
    return stem;
}

struct VariableDecl
{
    QString name;
    QString type;
    QJsonValue defaultValue;
    bool hasMin = false;
    bool hasMax = false;
    double min = 0.0;
    double max = 0.0;
    bool required = false;
};

bool parseVariableDeclarations(const QJsonObject& profile, QMap<QString, VariableDecl>& declarations, QString& errorMessage)
{
    declarations.clear();
    if (!profile.contains(QStringLiteral("variables")))
    {
        return true;
    }
    const QJsonValue variablesValue = profile.value(QStringLiteral("variables"));
    if (!variablesValue.isObject())
    {
        errorMessage = QStringLiteral("Profile field 'variables' must be an object.");
        return false;
    }

    const QJsonObject variables = variablesValue.toObject();
    for (auto it = variables.constBegin(); it != variables.constEnd(); ++it)
    {
        if (!it.value().isObject())
        {
            errorMessage = QStringLiteral("Variable '%1' must be an object.").arg(it.key());
            return false;
        }
        const QJsonObject object = it.value().toObject();
        VariableDecl decl;
        decl.name = it.key();
        decl.type = object.value(QStringLiteral("type")).toString();
        if (decl.type != QLatin1String("string") && decl.type != QLatin1String("number") && decl.type != QLatin1String("integer") && decl.type != QLatin1String("boolean"))
        {
            errorMessage = QStringLiteral("Variable '%1' has unsupported type '%2'.").arg(decl.name, decl.type);
            return false;
        }
        decl.defaultValue = object.value(QStringLiteral("default"));
        decl.required = object.value(QStringLiteral("required")).toBool(false);
        if (object.contains(QStringLiteral("min")))
        {
            decl.hasMin = true;
            decl.min = object.value(QStringLiteral("min")).toDouble();
        }
        if (object.contains(QStringLiteral("max")))
        {
            decl.hasMax = true;
            decl.max = object.value(QStringLiteral("max")).toDouble();
        }
        declarations.insert(decl.name, decl);
    }
    return true;
}

bool coerceBinding(const VariableDecl& decl, QJsonValue incoming, QJsonValue& typed, QString& errorMessage)
{
    if (decl.type == QLatin1String("boolean"))
    {
        if (incoming.isBool())
        {
            typed = incoming;
            return true;
        }
        if (incoming.isString())
        {
            const QString text = incoming.toString().trimmed().toLower();
            if (text == QLatin1String("true") || text == QLatin1String("false"))
            {
                typed = text == QLatin1String("true");
                return true;
            }
        }
        errorMessage = QStringLiteral("Variable '%1' must be a boolean.").arg(decl.name);
        return false;
    }
    if (decl.type == QLatin1String("string"))
    {
        if (!incoming.isString())
        {
            errorMessage = QStringLiteral("Variable '%1' must be a string.").arg(decl.name);
            return false;
        }
        typed = incoming;
        return true;
    }

    double number = incoming.toDouble();
    bool ok = incoming.isDouble();
    if (incoming.isString())
    {
        number = incoming.toString().toDouble(&ok);
    }
    if (!ok || !std::isfinite(number))
    {
        errorMessage = QStringLiteral("Variable '%1' must be a number.").arg(decl.name);
        return false;
    }
    if (decl.type == QLatin1String("integer"))
    {
        if (std::floor(number) != number)
        {
            errorMessage = QStringLiteral("Variable '%1' must be an integer.").arg(decl.name);
            return false;
        }
    }
    if (decl.hasMin && number < decl.min)
    {
        errorMessage = QStringLiteral("Variable '%1' is below the declared minimum.").arg(decl.name);
        return false;
    }
    if (decl.hasMax && number > decl.max)
    {
        errorMessage = QStringLiteral("Variable '%1' is above the declared maximum.").arg(decl.name);
        return false;
    }
    typed = decl.type == QLatin1String("integer") ? QJsonValue(static_cast<int>(number)) : QJsonValue(number);
    return true;
}

QString jsonValueAsText(const QJsonValue& value)
{
    if (value.isString())
    {
        return value.toString();
    }
    if (value.isBool())
    {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    if (value.isDouble())
    {
        return QString::fromUtf8(QJsonDocument(QJsonArray{ value }).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
    }
    if (value.isNull())
    {
        return QStringLiteral("null");
    }
    return QString::fromUtf8(canonicalPreflightJson(value));
}

struct SubstituteContext
{
    const QMap<QString, QJsonValue>* values = nullptr;
    const QMap<QString, VariableDecl>* declarations = nullptr;
    QString errorCode;
    QString errorMessage;
};

QJsonValue substituteValue(const QJsonValue& value, SubstituteContext& context)
{
    if (!context.errorCode.isEmpty())
    {
        return value;
    }

    if (value.isObject())
    {
        QJsonObject object;
        const QJsonObject source = value.toObject();
        for (auto it = source.constBegin(); it != source.constEnd(); ++it)
        {
            object.insert(it.key(), substituteValue(it.value(), context));
        }
        return object;
    }
    if (value.isArray())
    {
        QJsonArray array;
        for (const QJsonValue& item : value.toArray())
        {
            array.append(substituteValue(item, context));
        }
        return array;
    }
    if (!value.isString())
    {
        return value;
    }

    const QString text = value.toString();
    static const QRegularExpression whole(QStringLiteral("^\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}$"));
    static const QRegularExpression embedded(QStringLiteral("\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}"));
    const QRegularExpressionMatch wholeMatch = whole.match(text);
    if (wholeMatch.hasMatch())
    {
        const QString name = wholeMatch.captured(1);
        if (!context.declarations->contains(name))
        {
            context.errorCode = QStringLiteral("unresolved-variable");
            context.errorMessage = QStringLiteral("Profile references undeclared variable '%1'.").arg(name);
            return value;
        }
        return context.values->value(name);
    }

    QString result = text;
    QRegularExpressionMatchIterator iterator = embedded.globalMatch(text);
    while (iterator.hasNext())
    {
        const QRegularExpressionMatch match = iterator.next();
        const QString name = match.captured(1);
        if (!context.declarations->contains(name))
        {
            context.errorCode = QStringLiteral("unresolved-variable");
            context.errorMessage = QStringLiteral("Profile references undeclared variable '%1'.").arg(name);
            return value;
        }
        result.replace(match.captured(0), jsonValueAsText(context.values->value(name)));
    }
    return result;
}

}   // namespace

QJsonObject materializePreflightProfileDefaults(const QJsonObject& profile)
{
    QJsonObject materialized = profile;
    if (materialized.contains(QStringLiteral("checks")) && materialized.value(QStringLiteral("checks")).isArray())
    {
        QJsonArray checks;
        for (const QJsonValue& checkValue : materialized.value(QStringLiteral("checks")).toArray())
        {
            checks.append(checkValue.isObject() ? materializeCheckDefaults(checkValue.toObject()) : checkValue);
        }
        materialized.insert(QStringLiteral("checks"), checks);
    }
    if (materialized.contains(QStringLiteral("fixups")) && materialized.value(QStringLiteral("fixups")).isArray())
    {
        QJsonArray fixups;
        for (const QJsonValue& fixupValue : materialized.value(QStringLiteral("fixups")).toArray())
        {
            if (!fixupValue.isObject())
            {
                fixups.append(fixupValue);
                continue;
            }
            QJsonObject fixup = fixupValue.toObject();
            if (!fixup.contains(QStringLiteral("confirm")))
            {
                fixup.insert(QStringLiteral("confirm"), true);
            }
            fixups.append(fixup);
        }
        materialized.insert(QStringLiteral("fixups"), fixups);
    }
    return materialized;
}

QString computeProfileDigest(const QJsonObject& profile)
{
    QJsonObject canonical = materializePreflightProfileDefaults(profile);
    canonical.remove(QStringLiteral("description"));
    canonical.remove(QStringLiteral("authored"));
    canonical.remove(QStringLiteral("digest"));
    return QString::fromLatin1(QCryptographicHash::hash(canonicalPreflightJson(canonical), QCryptographicHash::Sha256).toHex());
}

QJsonObject PreflightProfileIdentity::toJson() const
{
    QJsonObject object{
        { QStringLiteral("id"), id },
        { QStringLiteral("version"), version },
        { QStringLiteral("name"), name },
        { QStringLiteral("digest"), digest },
        { QStringLiteral("effective_digest"), effectiveDigest.isEmpty() ? digest : effectiveDigest },
        { QStringLiteral("provisional"), provisional }
    };
    if (!sourcePath.isEmpty())
    {
        object.insert(QStringLiteral("source_path"), sourcePath);
    }
    if (!authored.isEmpty())
    {
        object.insert(QStringLiteral("authored"), authored);
    }
    if (!derivedFrom.isEmpty())
    {
        object.insert(QStringLiteral("derived_from"), derivedFrom);
    }
    return object;
}

PreflightProfileIdentity identifyPreflightProfile(const QJsonObject& profile, const QString& sourcePath)
{
    PreflightProfileIdentity identity;
    identity.sourcePath = sourcePath;
    identity.name = profile.value(QStringLiteral("name")).toString();
    identity.authored = profile.value(QStringLiteral("authored")).toObject();
    identity.derivedFrom = profile.value(QStringLiteral("derived_from")).toObject();
    identity.id = profile.value(QStringLiteral("id")).toString().trimmed();
    identity.version = profile.value(QStringLiteral("version")).toString().trimmed();
    if (identity.id.isEmpty())
    {
        identity.id = stemFromSourcePath(sourcePath);
        identity.provisional = true;
    }
    if (identity.version.isEmpty())
    {
        identity.version = QStringLiteral("0.0.0");
        identity.provisional = true;
    }
    identity.digest = computeProfileDigest(profile);
    identity.effectiveDigest = identity.digest;
    return identity;
}

PreflightProfileImportResult importPreflightProfile(const QJsonObject& profile, const QString& sourcePath)
{
    PreflightProfileImportResult result;
    result.profile = profile;
    result.identity = identifyPreflightProfile(profile, sourcePath);
    if (!profile.value(QStringLiteral("id")).toString().trimmed().isEmpty() && !isValidProfileId(profile.value(QStringLiteral("id")).toString().trimmed()))
    {
        result.errorCode = QStringLiteral("profile-invalid");
        result.errorMessage = QStringLiteral("Profile id must match ^[a-z][a-z0-9-]*$.");
        return result;
    }
    if (!profile.value(QStringLiteral("version")).toString().trimmed().isEmpty() && !isValidProfileVersion(profile.value(QStringLiteral("version")).toString().trimmed()))
    {
        result.errorCode = QStringLiteral("profile-invalid");
        result.errorMessage = QStringLiteral("Profile version must be MAJOR.MINOR.PATCH.");
        return result;
    }
    const QString committed = profile.value(QStringLiteral("digest")).toString().trimmed().toLower();
    if (!committed.isEmpty() && committed != result.identity.digest)
    {
        result.errorCode = QStringLiteral("profile-digest-mismatch");
        result.errorMessage = QStringLiteral("Profile digest does not match the semantic content and was not repaired.");
        return result;
    }
    result.ok = true;
    return result;
}

QJsonObject exportPreflightProfile(const QJsonObject& profile)
{
    QJsonObject exported = materializePreflightProfileDefaults(profile);
    const PreflightProfileIdentity provisional = identifyPreflightProfile(exported, QString());
    if (!exported.contains(QStringLiteral("id")))
    {
        exported.insert(QStringLiteral("id"), provisional.id);
    }
    if (!exported.contains(QStringLiteral("version")))
    {
        exported.insert(QStringLiteral("version"), provisional.version);
    }
    exported.insert(QStringLiteral("digest"), computeProfileDigest(exported));
    const QJsonDocument document(canonicalizePreflightJson(exported).toObject());
    return QJsonDocument::fromJson(document.toJson(QJsonDocument::Compact)).object();
}

QJsonObject forkPreflightProfile(const QJsonObject& parent, const QString& newId, const QString& newVersion)
{
    QJsonObject forked = parent;
    const PreflightProfileIdentity parentIdentity = identifyPreflightProfile(parent);
    forked.insert(QStringLiteral("id"), newId);
    forked.insert(QStringLiteral("version"), newVersion);
    forked.insert(QStringLiteral("derived_from"), QJsonObject{
                                                      { QStringLiteral("id"), parentIdentity.id },
                                                      { QStringLiteral("version"), parentIdentity.version },
                                                      { QStringLiteral("digest"), parentIdentity.digest } });
    forked.remove(QStringLiteral("digest"));
    return exportPreflightProfile(forked);
}

PreflightVariableBindResult bindPreflightProfileVariables(const QJsonObject& profile,
                                                          const QJsonObject& jobSpecBindings,
                                                          const QJsonObject& cliBindings)
{
    PreflightVariableBindResult result;
    result.profile = profile;
    QMap<QString, VariableDecl> declarations;
    if (!parseVariableDeclarations(profile, declarations, result.errorMessage))
    {
        result.errorCode = QStringLiteral("profile-invalid");
        return result;
    }

    QMap<QString, QJsonValue> values;
    QMap<QString, QString> sources;
    for (auto it = declarations.constBegin(); it != declarations.constEnd(); ++it)
    {
        if (!it.value().defaultValue.isUndefined())
        {
            QJsonValue typed;
            if (!coerceBinding(it.value(), it.value().defaultValue, typed, result.errorMessage))
            {
                result.errorCode = QStringLiteral("unresolved-variable");
                return result;
            }
            values.insert(it.key(), typed);
            sources.insert(it.key(), QStringLiteral("default"));
        }
    }

    const auto applyOverlay = [&](const QJsonObject& overlay, const QString& sourceName) -> bool
    {
        for (auto it = overlay.constBegin(); it != overlay.constEnd(); ++it)
        {
            if (!declarations.contains(it.key()))
            {
                result.errorCode = QStringLiteral("unresolved-variable");
                result.errorMessage = QStringLiteral("Binding '%1' is not declared by the profile.").arg(it.key());
                return false;
            }
            QJsonValue typed;
            if (!coerceBinding(declarations.value(it.key()), it.value(), typed, result.errorMessage))
            {
                result.errorCode = QStringLiteral("unresolved-variable");
                return false;
            }
            values.insert(it.key(), typed);
            sources.insert(it.key(), sourceName);
        }
        return true;
    };
    if (!applyOverlay(jobSpecBindings, QStringLiteral("job-spec")) || !applyOverlay(cliBindings, QStringLiteral("cli")))
    {
        return result;
    }

    for (auto it = declarations.constBegin(); it != declarations.constEnd(); ++it)
    {
        if (!values.contains(it.key()) && it.value().required)
        {
            result.errorCode = QStringLiteral("unresolved-variable");
            result.errorMessage = QStringLiteral("Required profile variable '%1' has no value.").arg(it.key());
            return result;
        }
        if (!values.contains(it.key()) && it.value().defaultValue.isUndefined())
        {
            result.errorCode = QStringLiteral("unresolved-variable");
            result.errorMessage = QStringLiteral("Profile variable '%1' has no value.").arg(it.key());
            return result;
        }
    }

    QJsonObject substitutable = profile;
    substitutable.remove(QStringLiteral("variables"));
    SubstituteContext context;
    context.values = &values;
    context.declarations = &declarations;
    const QJsonValue substituted = substituteValue(substitutable, context);
    if (!context.errorCode.isEmpty())
    {
        result.errorCode = context.errorCode;
        result.errorMessage = context.errorMessage;
        return result;
    }

    QJsonObject bound = substituted.toObject();
    if (profile.contains(QStringLiteral("variables")))
    {
        bound.insert(QStringLiteral("variables"), profile.value(QStringLiteral("variables")));
    }
    result.profile = bound;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
    {
        result.bindings.append(QJsonObject{
            { QStringLiteral("name"), it.key() },
            { QStringLiteral("value"), it.value() },
            { QStringLiteral("source"), sources.value(it.key()) } });
    }
    result.ok = true;
    return result;
}

}   // namespace pdf
