#include "merge.h"
#include "docwordspace.h"
#include <set>
#include <text.h>

void Trinity::MergeCandidatesCollection::commit()
{
        std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
                return b.gen < a.gen;
        });

        map.clear();
        all.clear();

        for (const auto &c : candidates)
        {
                const auto &ud = c.maskedDocuments;

                map.push_back({c, all.size()});
                if (ud)
                        all.push_back(ud);
        }
}

std::unique_ptr<Trinity::masked_documents_registry> Trinity::MergeCandidatesCollection::scanner_registry_for(const uint16_t idx)
{
        const auto n = map[idx].second;

        return masked_documents_registry::make(all.data(), n);
}


// Make sure you have commited first
// Unlike with e.g SegmentIndexSession where the order of postlists in the index is based on our translation(term=>integer id) and the ascending order of that id
// here the order will match the order the terms are found in `tersm`, because we perform a merge-sort and so we process terms in lexicograpphic order
void Trinity::MergeCandidatesCollection::merge(Trinity::Codecs::IndexSession *is, simple_allocator *allocator, std::vector<std::pair<str8_t, Trinity::term_index_ctx>> *const terms, const uint32_t flushFreq)
{
        static constexpr bool trace{false};

        struct tracked_candidate
        {
                uint16_t idx;
                merge_candidate candidate;
        };

        std::vector<tracked_candidate> all_;

        if (trace)
                SLog("Merging ", candidates.size(), " candidates\n");

        require(candidates.size() < std::numeric_limits<uint16_t>::max());

        for (uint16_t i{0}; i != candidates.size(); ++i)
        {
                if (trace)
                        SLog("Candidate ", i, " gen=", candidates[i].gen, " ", candidates[i].ap->codec_identifier(), "\n");

                if (i)
                        require(candidates[i].gen < candidates[i - 1].gen);

                if (candidates[i].terms && false == candidates[i].terms->done() && candidates[i].ap)
		{
			// ap may be nullptr if we only wanted to e.g mask documents
                        all_.push_back({i, candidates[i]});
		}
        }

        if (all_.empty())
                return;

        auto all = all_.data();
        uint16_t rem = all_.size();
        uint16_t toAdvance[rem];
        const auto isCODEC = is->codec_identifier();
        DocWordsSpace dws{Limits::MaxPosition}; // dummy, for materialize_hits()
        size_t termHitsCapacity{0};
        term_hit *termHitsStorage{nullptr};
        std::vector<Trinity::Codecs::IndexSession::merge_participant> mergeParticipants;
        std::vector<std::pair<Trinity::Codecs::Decoder *, masked_documents_registry *>> decodersV;
        term_index_ctx tctx;
        std::unique_ptr<Trinity::Codecs::Encoder> enc(is->new_encoder());

        Defer(
            {
                    if (termHitsStorage)
                            std::free(termHitsStorage);
            });

        for (;;)
        {
                uint8_t toAdvanceCnt{1};
                const auto pair = all[0].candidate.terms->cur();
                auto selected{pair};
                auto codec = all[0].candidate.ap->codec_identifier();
                bool sameCODEC{true};

                toAdvance[0] = 0;
                for (uint16_t i{1}; i != rem; ++i)
                {
                        const auto pair = all[i].candidate.terms->cur();
                        const auto r = terms_cmp(pair.first.data(), pair.first.size(), selected.first.data(), selected.first.size());

                        if (r < 0)
                        {
                                toAdvanceCnt = 1;
                                toAdvance[0] = i;
                                selected = pair;
                                sameCODEC = true;
                                codec = all[i].candidate.ap->codec_identifier();
                        }
                        else if (r == 0)
                        {
                                if (sameCODEC)
                                {
                                        auto c = all[i].candidate.ap->codec_identifier();

                                        if (c != codec)
                                                sameCODEC = false;
                                }

                                toAdvance[toAdvanceCnt++] = i;
                        }
                }

                const str8_t outTerm(allocator->CopyOf(selected.first.data(), selected.first.size()), selected.first.size());
              	[[maybe_unused]] const bool fastPath = sameCODEC && codec == isCODEC;
                static constexpr bool trace{false};
                //const bool trace = selected.first.Eq(_S("ANNEX"));

                if (trace)
                        SLog("TERM [", selected.first, "], toAdvanceCnt = ", toAdvanceCnt, ", sameCODEC = ", sameCODEC, ", first = ", toAdvance[0], ", fastPath = ", fastPath, "\n");

                if (toAdvanceCnt == 1)
                {
                        auto c = all[toAdvance[0]].candidate;
                        auto maskedDocsReg = scanner_registry_for(all[toAdvance[0]].idx);

                        if (fastPath && maskedDocsReg->empty())
                        {
                                if (likely(selected.second.documents))
                                {
                                        // See comments below for why this is possible
                                        const auto chunk = is->append_index_chunk(c.ap, selected.second);

                                        terms->push_back({outTerm, {selected.second.documents, chunk}});
                                }
                                else if (trace)
                                        SLog("No documents\n");
                        }
                        else
                        {
                                if (unlikely(0 == selected.second.documents))
                                {
                                        // It's possible, however unlikely (check your implementation)
                                        // that you have e.g indexed a term, but indexed no documents for that term
                                        // in which case, it will be 0 documents.
					//
                                        // We will just skip this here altogether(doing the same for other branches)
					//
					// Note that SegmentIndexSession and this merge() method explicitly drop terms with no documents associated with them, so
					// the only real way to get a term with no document is to use the various Trinity segment constructs directly.
                                        if (trace)
                                                Print("0 documents for TERM [", selected.first, "]\n");
                                }
                                else
                                {
                                        std::unique_ptr<Trinity::Codecs::Decoder> dec(c.ap->new_decoder(selected.second));

                                        dec->begin();
                                        enc->begin_term();

                                        do
                                        {
                                                const auto docID = dec->curDocument.id;
                                                const auto freq = dec->curDocument.freq;

                                                require(docID != MaxDocIDValue); // sanity check

						if (trace)
							SLog("docID = ", docID, ", masked = ", maskedDocsReg->test(docID), "\n");

                                                if (!maskedDocsReg->test(docID))
                                                {
                                                        if (freq > termHitsCapacity)
                                                        {
                                                                if (termHitsStorage)
                                                                        std::free(termHitsStorage);

                                                                termHitsCapacity = freq + 128;
                                                                termHitsStorage = (term_hit *)malloc(sizeof(term_hit) * termHitsCapacity);
                                                        }

                                                        enc->begin_document(docID);
                                                        dec->materialize_hits(1 /* dummy */, &dws /* dummy */, termHitsStorage);

                                                        for (uint32_t i{0}; i != freq; ++i)
                                                        {
                                                                const auto &th = termHitsStorage[i];
                                                                const auto bytes = (uint8_t *)&th.payload;

                                                                enc->new_hit(th.pos, {bytes, th.payloadLen});
                                                        }

                                                        enc->end_document();
                                                }

                                        } while (dec->next());

                                        enc->end_term(&tctx);

                                        if (tctx.documents)
					{
						// This means that we may end up e.g storing some meta-data specific to this term, and/or a skiplist
						// in the index/other index session data files in between enc->begin_term() .. enc->end_term(), which could
						// have been set even if no documents were indexed for this term.
						// That's fine though -- will ignore them in a future merge op.
                                                terms->push_back({outTerm, tctx});
					}

                                        if (trace)
                                                SLog("Indexed Term\n");
                                }
                        }
                }
                else
                {
                        if (fastPath)
                        {
                                mergeParticipants.clear();

                                for (uint16_t i{0}; i != toAdvanceCnt; ++i)
                                {
                                        const auto idx = toAdvance[i];

                                        if (likely(all[idx].candidate.terms->cur().second.documents))
                                        {
                                                // See comments earliert for why this is possible

                                                mergeParticipants.push_back(
                                                    {all[idx].candidate.ap,
                                                     all[idx].candidate.terms->cur().second,
                                                     scanner_registry_for(all[idx].idx).release()});
                                        }
                                        else if (trace)
                                                SLog("No documents for candidate ", i, "\n");
                                }

                                if (mergeParticipants.size())
                                {
                                        enc->begin_term();
                                        is->merge(mergeParticipants.data(), mergeParticipants.size(), enc.get());
                                        enc->end_term(&tctx);

                                        if (tctx.documents)
                                                terms->push_back({outTerm, tctx});

                                        for (uint16_t i{0}; i != mergeParticipants.size(); ++i)
                                                delete mergeParticipants[i].maskedDocsReg;
                                }
                        }
                        else
                        {
                                // we got to merge-sort across different codecs and output to an encoder of a different, potentially, codec
                                for (uint16_t i{0}; i != toAdvanceCnt; ++i)
                                {
                                        const auto idx = toAdvance[i];

                                        if (likely(all[idx].candidate.terms->cur().second.documents))
                                        {
                                                // see earlier comments for why this is possible
                                                auto ap = all[idx].candidate.ap;
                                                auto dec = ap->new_decoder(all[idx].candidate.terms->cur().second);
                                                auto reg = scanner_registry_for(all[idx].idx).release();

                                                require(reg);
                                                dec->begin();
                                                decodersV.push_back({dec, reg});
                                        }
                                        else if (trace)
                                                SLog("No documents for candidate ", i, "\n");
                                }

                                if (uint16_t rem = decodersV.size())
                                {
                                        auto decoders = decodersV.data();
                                        uint16_t toAdvance[128];

                                        require(sizeof_array(toAdvance) >= decodersV.size());

                                        enc->begin_term();
                                        for (;;)
                                        {
                                                uint16_t toAdvanceCnt{1};
                                                auto lowestDID = decoders[0].first->curDocument.id;

                                                toAdvance[0] = 0;
                                                for (uint16_t i{1}; i != rem; ++i)
                                                {
                                                        const auto id = decoders[i].first->curDocument.id;

                                                        if (id < lowestDID)
                                                        {
                                                                lowestDID = id;
                                                                toAdvanceCnt = 1;
                                                                toAdvance[0] = i;
                                                        }
                                                        else if (id == lowestDID)
                                                                toAdvance[toAdvanceCnt++] = i;
                                                }

                                                // always choose the first because they are always sorted by gen DESC

						if (trace)
							SLog("Lowest = ", lowestDID, ", masked = ", decoders[toAdvance[0]].second->test(lowestDID), "\n");

                                                if (!decoders[toAdvance[0]].second->test(lowestDID))
                                                {
                                                        auto dec = decoders[toAdvance[0]].first;
                                                        const auto freq = dec->curDocument.freq;

                                                        if (freq > termHitsCapacity)
                                                        {
                                                                if (termHitsStorage)
                                                                        std::free(termHitsStorage);

                                                                termHitsCapacity = freq + 128;
                                                                termHitsStorage = (term_hit *)malloc(sizeof(term_hit) * termHitsCapacity);
                                                        }

                                                        enc->begin_document(lowestDID);
                                                        dec->materialize_hits(1 /* dummy */, &dws /* dummy */, termHitsStorage);

                                                        for (uint32_t i{0}; i != freq; ++i)
                                                        {
                                                                const auto &th = termHitsStorage[i];
                                                                const auto bytes = (uint8_t *)&th.payload;

                                                                enc->new_hit(th.pos, {bytes, th.payloadLen});
                                                        }
                                                        enc->end_document();
                                                }

                                                do
                                                {
                                                        const auto idx = toAdvance[--toAdvanceCnt];
                                                        auto dec = decoders[idx].first;

                                                        if (!dec->next())
                                                        {
                                                                delete dec;
                                                                delete decoders[idx].second;

                                                                if (!--rem)
                                                                        goto l10;

                                                                memmove(decoders + idx, decoders + idx + 1, (rem - idx) * sizeof(decoders[0]));
                                                        }
                                                } while (toAdvanceCnt);
                                        }

                                l10:
                                        decodersV.clear();
                                        enc->end_term(&tctx);

                                        if (tctx.documents)
                                                terms->push_back({outTerm, tctx});
                                }
                        }
                }

                if (flushFreq && is->indexOut.size() > flushFreq)
                {
                        // TODO: support pending
                }

                do
                {
                        const auto idx = toAdvance[--toAdvanceCnt];
                        auto terms = all[idx].candidate.terms;

                        terms->next();
                        if (terms->done())
                        {
                                if (!--rem)
                                        goto l1;

                                memmove(all + idx, all + idx + 1, (rem - idx) * sizeof(all[0]));
                        }
                } while (toAdvanceCnt);
        }
l1:;
}

std::vector<std::pair<uint64_t, Trinity::MergeCandidatesCollection::IndexSourceRetention>>
Trinity::MergeCandidatesCollection::consider_tracked_sources(std::vector<uint64_t> trackedSources)
{
        std::set<uint64_t> candidatesGens;
        std::vector<std::pair<uint64_t, IndexSourceRetention>> res;
        const auto cnt = trackedSources.size();
        uint32_t lastNotCandidateIdx{UINT32_MAX};

        std::sort(trackedSources.begin(), trackedSources.end());
        for (const auto &it : candidates)
                candidatesGens.insert(it.gen);

        for (uint32_t i{0}; i != cnt; ++i)
        {
                const auto gen = trackedSources[i];

                if (!candidatesGens.count(gen))
                {
                        lastNotCandidateIdx = i;
                        res.push_back({gen, IndexSourceRetention::RetainAll});
                        continue;
                }
                else if (lastNotCandidateIdx < i)
                {
                        // if there is 1+ other tracked sources, and any of those sources is NOT in candidatesGens, we need to retain the updated documentIDs
                        res.push_back({gen, IndexSourceRetention::RetainDocumentIDsUpdates});
                }
                else
                        res.push_back({gen, IndexSourceRetention::Delete});
        }

        return res;
}
