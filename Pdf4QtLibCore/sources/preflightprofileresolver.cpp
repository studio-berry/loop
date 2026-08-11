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
#include <QSet>

#include <algorithm>

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
    if (key == QStringLiteral("client_id")) return context.clientId;
    if (key == QStringLiteral("product_id")) return context.productId;
    if (key == QStringLiteral("job_type")) return context.jobType;
    if (key == QStringLiteral("press_id")) return context.pressId;
    if (key == QStringLiteral("stock_id")) return context.stockId;
    if (key == QStringLiteral("finishing_id")) return context.finishingId;
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
    source.id = profileObject.value(QStringLiteral("profile_id")).toString().trimmed();
    if (source.id.isEmpty())
    {
        source.id = fallbackId;
    }
    if (source.id.isEmpty())
    {
        errorMessage = QStringLiteral("Profile source is missing a stable profile_id.");
        return false;
    }
    source.version = profileObject.value(QStringLiteral("profile_version")).toString();
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
        // An otherwise well-formed profile with an empty checks array is a semantic
        // authoring problem PreflightEngine::run() itself reports as a document-scope
        // 'profile' finding, not a source-validation failure - accept the source here
        // (resolveMatched() below runs the same exemption for the merged/effective
        // profile) so the real run classifies it downstream instead of this pre-check
        // rejecting it outright as an unconditional resolver error.
        const bool isEmptyChecksOnly = !profileObject.value(QStringLiteral("name")).toString().isEmpty()
            && profileObject.value(QStringLiteral("checks")).toArray().isEmpty();
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
    return candidate.source.priority > current.priority
        || (candidate.source.priority == current.priority && candidate.specificity > current.specificity);
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
    if (incoming.isObject() && !incoming.toObject().isEmpty()
        && (!target.contains(key) || target.value(key).isObject()))
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
    const bool sameAuthority = previous.priority == source.source.priority
        && previous.specificity == source.specificity;
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
                if (contextAttribute == context.attributes.constEnd() || !contextAttribute.value().isString()
                    || !matchesSelectorValue(attribute.value(), contextAttribute.value().toString()))
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
    QMap<QString, QJsonObject> items;
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
    for (auto iterator = items.constBegin(); iterator != items.constEnd(); ++iterator)
    {
        result.append(iterator.value());
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

        if (!mergeIdentifiedArray(effective, QStringLiteral("checks"), matched, contributions, result.decisions, errorMessage)
            || !mergeIdentifiedArray(effective, QStringLiteral("fixups"), matched, contributions, result.decisions, errorMessage))
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
        // An otherwise well-formed profile with an empty checks array is a semantic
        // authoring problem that PreflightEngine::run() itself reports as a
        // document-scope 'profile' finding, not a resolution/merge failure - let
        // resolution succeed so that classification happens once, downstream,
        // instead of being pre-empted here as a resolver error (which always maps
        // to an Error verdict / PreflightError exit code, regardless of reason).
        const bool isEmptyChecksOnly = !effective.value(QStringLiteral("name")).toString().isEmpty()
            && effective.value(QStringLiteral("checks")).toArray().isEmpty();
        if (!isEmptyChecksOnly)
        {
            result.errorCode = QStringLiteral("invalid-effective-profile");
            result.errorMessage = errorMessage;
            return result;
        }
    }

    result.effectiveProfile = effective;
    result.effectiveHash = QCryptographicHash::hash(canonicalPreflightJson(effective), QCryptographicHash::Sha256).toHex();
    std::sort(result.decisions.begin(), result.decisions.end(), [](const auto& left, const auto& right) {
        if (left.jsonPointer != right.jsonPointer) return left.jsonPointer < right.jsonPointer;
        if (left.sourceId != right.sourceId) return left.sourceId < right.sourceId;
        return left.overriddenSourceId < right.overriddenSourceId;
    });
    result.ok = true;
    return result;
}

} // namespace

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
    if (!clientId.isEmpty()) object.insert(QStringLiteral("client_id"), clientId);
    if (!productId.isEmpty()) object.insert(QStringLiteral("product_id"), productId);
    if (!jobType.isEmpty()) object.insert(QStringLiteral("job_type"), jobType);
    if (!pressId.isEmpty()) object.insert(QStringLiteral("press_id"), pressId);
    if (!stockId.isEmpty()) object.insert(QStringLiteral("stock_id"), stockId);
    if (!finishingId.isEmpty()) object.insert(QStringLiteral("finishing_id"), finishingId);
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

    if (!parseIdValue(object, { QStringLiteral("client_id"), QStringLiteral("client") }, context.clientId, errorMessage)
        || !parseIdValue(object, { QStringLiteral("product_id"), QStringLiteral("product") }, context.productId, errorMessage)
        || !parseIdValue(object, { QStringLiteral("job_type"), QStringLiteral("job-type") }, context.jobType, errorMessage)
        || !parseIdValue(object, { QStringLiteral("press_id"), QStringLiteral("press") }, context.pressId, errorMessage)
        || !parseIdValue(object, { QStringLiteral("stock_id"), QStringLiteral("stock") }, context.stockId, errorMessage)
        || !parseIdValue(object, { QStringLiteral("finishing_id"), QStringLiteral("finishing") }, context.finishingId, errorMessage))
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
            { QStringLiteral("hash"), QString::fromLatin1(effectiveHash) }
        } },
        { QStringLiteral("decisions"), decisionsJson }
    };
    if (!ok)
    {
        object.insert(QStringLiteral("error"), QJsonObject{
            { QStringLiteral("code"), errorCode },
            { QStringLiteral("message"), errorMessage }
        });
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

    std::sort(snapshot.sources.begin(), snapshot.sources.end(), [](const auto& left, const auto& right) {
        if (left.id != right.id) return left.id < right.id;
        return left.version < right.version;
    });
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

    std::sort(matched.begin(), matched.end(), [](const RankedSource& left, const RankedSource& right) {
        if (left.source.priority != right.source.priority) return left.source.priority < right.source.priority;
        if (left.specificity != right.specificity) return left.specificity < right.specificity;
        if (left.source.id != right.source.id) return left.source.id < right.source.id;
        return left.source.version < right.source.version;
    });

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

} // namespace pdf
