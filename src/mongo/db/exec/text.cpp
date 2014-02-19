/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/text.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/internal_plans.h"

namespace mongo {

    TextStage::TextStage(const TextStageParams& params,
                         WorkingSet* ws,
                         const MatchExpression* filter)
        : _params(params),
          _ftsMatcher(params.query, params.spec),
          _ws(ws),
          _filter(filter),
          _internalState(INIT_SCANS),
          _currentIndexScanner(0) {

        _scoreIterator = _scores.end();
    }

    TextStage::~TextStage() { }

    bool TextStage::isEOF() {
        return _internalState == DONE;
    }

    PlanStage::StageState TextStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (isEOF()) { return PlanStage::IS_EOF; }

        switch (_internalState) {
        case INIT_SCANS:
            return initScans(out);
        case READING_TERMS:
            return readFromSubScanners(out);
        case RETURNING_RESULTS:
            return returnResults(out);
        case DONE:
            return PlanStage::IS_EOF;
        }

        // Not reached.
        return PlanStage::IS_EOF;
    }

    void TextStage::prepareToYield() {
        ++_commonStats.yields;

        for (size_t i = 0; i < _scanners.size(); ++i) {
            _scanners.mutableVector()[i]->prepareToYield();
        }
    }

    void TextStage::recoverFromYield() {
        ++_commonStats.unyields;

        for (size_t i = 0; i < _scanners.size(); ++i) {
            _scanners.mutableVector()[i]->recoverFromYield();
        }
    }

    void TextStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;

        // Propagate invalidate to children.
        for (size_t i = 0; i < _scanners.size(); ++i) {
            _scanners.mutableVector()[i]->invalidate(dl, type);
        }

        // We store the score keyed by DiskLoc.  We have to toss out our state when the DiskLoc
        // changes.
        // TODO: If we're RETURNING_RESULTS we could somehow buffer the object.
        ScoreMap::iterator scoreIt = _scores.find(dl);
        if (scoreIt != _scores.end()) {
            if (scoreIt == _scoreIterator) {
                _scoreIterator++;
            }
            _scores.erase(scoreIt);
        }
    }

    PlanStageStats* TextStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_TEXT));
        ret->specific.reset(new TextStats(_specificStats));
        return ret.release();
    }

    PlanStage::StageState TextStage::initScans(WorkingSetID* out) {
        invariant(0 == _scanners.size());

        // Get all the index scans for each term in our query.
        for (size_t i = 0; i < _params.query.getTerms().size(); i++) {
            const string& term = _params.query.getTerms()[i];
            IndexScanParams params;
            params.bounds.startKey = FTSIndexFormat::getIndexKey(MAX_WEIGHT,
                                                                 term,
                                                                 _params.indexPrefix,
                                                                 _params.spec.getTextIndexVersion());
            params.bounds.endKey = FTSIndexFormat::getIndexKey(0,
                                                               term,
                                                                _params.indexPrefix,
                                                               _params.spec.getTextIndexVersion());
            params.bounds.endKeyInclusive = true;
            params.bounds.isSimpleRange = true;
            params.descriptor = _params.index;
            params.direction = -1;
            _scanners.mutableVector().push_back(new IndexScan(params, _ws, NULL));
        }

        // If we have no terms we go right to EOF.
        if (0 == _scanners.size()) {
            _internalState = DONE;
            return PlanStage::IS_EOF;
        }

        // Transition to the next state.
        _internalState = READING_TERMS;
        return PlanStage::NEED_TIME;
    }

    PlanStage::StageState TextStage::readFromSubScanners(WorkingSetID* out) {
        // This should be checked before we get here.
        invariant(_currentIndexScanner < _scanners.size());

        // Read the next result from our current scanner.
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState childState = _scanners.vector()[_currentIndexScanner]->work(&id);

        if (PlanStage::ADVANCED == childState) {
            WorkingSetMember* wsm = _ws->get(id);
            invariant(1 == wsm->keyData.size());
            invariant(wsm->hasLoc());
            IndexKeyDatum& keyDatum = wsm->keyData.back();
            addTerm(keyDatum.keyData, wsm->loc);
            _ws->free(id);
            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::IS_EOF == childState) {
            // Done with this scan.
            ++_currentIndexScanner;

            if (_currentIndexScanner < _scanners.size()) {
                // We have another scan to read from.
                return PlanStage::NEED_TIME;
            }

            // If we're here we are done reading results.  Move to the next state.
            _scoreIterator = _scores.begin();
            _internalState = RETURNING_RESULTS;

            // Don't need to keep these around.
            _scanners.clear();
            return PlanStage::NEED_TIME;
        }
        else {
            if (PlanStage::FAILURE == childState) {
                // Propagate failure from below.
                *out = id;
            }
            return childState;
        }
    }

    PlanStage::StageState TextStage::returnResults(WorkingSetID* out) {
        if (_scoreIterator == _scores.end()) {
            _internalState = DONE;
            return PlanStage::IS_EOF;
        }

        // Filter for phrases and negative terms, score and truncate.
        DiskLoc loc = _scoreIterator->first;
        double score = _scoreIterator->second;
        _scoreIterator++;

        // Ignore non-matched documents.
        if (score < 0) {
            return PlanStage::NEED_TIME;
        }

        // Filter for phrases and negated terms
        if (_params.query.hasNonTermPieces()) {
            if (!_ftsMatcher.matchesNonTerm(loc.obj())) {
                return PlanStage::NEED_TIME;
            }
        }

        *out = _ws->allocate();
        WorkingSetMember* member = _ws->get(*out);
        member->loc = loc;
        member->obj = member->loc.obj();
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
        member->addComputed(new TextScoreComputedData(score));
        return PlanStage::ADVANCED;
    }

    class TextMatchableDocument : public MatchableDocument {
    public:
        TextMatchableDocument(const BSONObj& keyPattern,
                              const BSONObj& key,
                              DiskLoc loc,
                              bool *fetched)
            : _keyPattern(keyPattern),
              _key(key),
              _loc(loc),
              _fetched(fetched) { }

        BSONObj toBSON() const {
            *_fetched = true;
            return _loc.obj();
        }

        virtual ElementIterator* allocateIterator(const ElementPath* path) const {
            BSONObjIterator keyPatternIt(_keyPattern);
            BSONObjIterator keyDataIt(_key);

            // Look in the key.
            while (keyPatternIt.more()) {
                BSONElement keyPatternElt = keyPatternIt.next();
                verify(keyDataIt.more());
                BSONElement keyDataElt = keyDataIt.next();

                if (path->fieldRef().equalsDottedField(keyPatternElt.fieldName())) {
                    if (Array == keyDataElt.type()) {
                        return new SimpleArrayElementIterator(keyDataElt, true);
                    }
                    else {
                        return new SingleElementElementIterator(keyDataElt);
                    }
                }
            }

            // All else fails, fetch.
            *_fetched = true;
            return new BSONElementIterator(path, _loc.obj());
        }

        virtual void releaseIterator( ElementIterator* iterator ) const {
            delete iterator;
        }

    private:
        BSONObj _keyPattern;
        BSONObj _key;
        DiskLoc _loc;
        bool* _fetched;
    };

    void TextStage::addTerm(const BSONObj& key, const DiskLoc& loc) {
        double *documentAggregateScore = &_scores[loc];

        ++_specificStats.keysExamined;

        // Locate score within possibly compound key: {prefix,term,score,suffix}.
        BSONObjIterator keyIt(key);
        for (unsigned i = 0; i < _params.spec.numExtraBefore(); i++) {
            keyIt.next();
        }

        keyIt.next(); // Skip past 'term'.

        BSONElement scoreElement = keyIt.next();
        double documentTermScore = scoreElement.number();
        
        // Handle filtering.
        if (*documentAggregateScore < 0) {
            // We have already rejected this document.
            return;
        }

        if (*documentAggregateScore == 0) {
            if (_filter) {
                // We have not seen this document before and need to apply a filter.
                bool fetched = false;
                TextMatchableDocument tdoc(_params.index->keyPattern(), key, loc, &fetched);

                if (!_filter->matches(&tdoc)) {
                    // We had to fetch but we're not going to return it.
                    if (fetched) {
                        ++_specificStats.fetches;
                    }
                    *documentAggregateScore = -1;
                    return;
                }
            }
            else {
                // If we're here, we're going to return the doc, and we do a fetch later.
                ++_specificStats.fetches;
            }
        }

        // Aggregate relevance score, term keys.
        *documentAggregateScore += documentTermScore;
    }

}  // namespace mongo