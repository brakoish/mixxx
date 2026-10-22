#pragma once

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <memory>

#include "library/searchquery.h"
#include "util/class.h"

class TrackCollection;
class QueryNode;
class AndNode;

class SearchQueryParser {
  public:
    explicit SearchQueryParser(TrackCollection* pTrackCollection, QStringList searchColumns);

    void setSearchColumns(QStringList searchColumns);

    /// Columns present in the searched table. External libraries (for
    /// example Rekordbox USB exports) lack album_artist and key_id; filters
    /// then use only the columns that exist. Empty (default): all columns.
    void setAvailableColumns(const QStringList& columns);

    std::unique_ptr<QueryNode> parseQuery(
            const QString& query,
            const QString& extraFilter) const;

    /// splits the query into a list of terms
    static QStringList splitQueryIntoWords(const QString& query);
    /// checks if the changed search query is less specific then the original term
    static bool queryIsLessSpecific(const QString& original, const QString& changed);

  private:
    void parseTokens(QStringList tokens,
                     AndNode* pQuery) const;

    std::unique_ptr<AndNode> parseAndNode(const QString& query) const;
    std::unique_ptr<OrNode> parseOrNode(const QString& query) const;

    struct TextArgumentResult {
        QString argument;
        StringMatch mode;
    };

    TextArgumentResult getTextArgument(QString argument,
            QStringList* tokens,
            bool removeLeadingEqualsSign = true) const;

    QStringList sqlColumns(const QString& field) const;
    std::unique_ptr<QueryNode> keyTextNode(
            mixxx::track::io::key::ChromaticKey key, bool fuzzy) const;

    TrackCollection* m_pTrackCollection;
    QSet<QString> m_availableColumns;
    QStringList m_queryColumns;
    bool m_searchCrates;
    QStringList m_textFilters;
    QStringList m_numericFilters;
    QStringList m_specialFilters;
    QHash<QString, QStringList> m_fieldToSqlColumns;

    QRegularExpression m_textFilterMatcher;
    QRegularExpression m_crateFilterMatcher;
    QRegularExpression m_numericFilterMatcher;
    QRegularExpression m_specialFilterMatcher;

    DISALLOW_COPY_AND_ASSIGN(SearchQueryParser);
};
