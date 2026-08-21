#include"Boss/Mod/ChannelCandidateInvestigator/EvictionPolicy.hpp"
#include"Boss/random_engine.hpp"
#include<assert.h>
#include<random>

namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator {
namespace EvictionPolicy {

std::pair<Ln::NodeId, char const*>
pick( std::vector<Ln::NodeId> const& pool
    , std::map<Ln::NodeId, Boss::Msg::TrackRecordVerdict> const& verdicts
    , std::set<Ln::NodeId> const& spliceable
    ) {
	assert(!pool.empty());

	auto underperformers = std::vector<Ln::NodeId>();
	auto norecord_plain = std::vector<Ln::NodeId>();
	auto norecord_splice = std::vector<Ln::NodeId>();
	auto keepers = std::vector<Ln::NodeId>();

	for (auto const& n : pool) {
		auto verdict = Boss::Msg::TrackRecordVerdict::NoRecord;
		auto it = verdicts.find(n);
		if (it != verdicts.end())
			verdict = it->second;
		switch (verdict) {
		case Boss::Msg::TrackRecordVerdict::Underperformer:
			underperformers.push_back(n);
			break;
		case Boss::Msg::TrackRecordVerdict::Keeper:
			keepers.push_back(n);
			break;
		case Boss::Msg::TrackRecordVerdict::NoRecord:
			if (spliceable.count(n) != 0)
				norecord_splice.push_back(n);
			else
				norecord_plain.push_back(n);
			break;
		}
	}

	auto stratum = &underperformers;
	auto reason = "underperformer";
	if (stratum->empty()) {
		stratum = &norecord_plain;
		reason = "no-record";
	}
	if (stratum->empty()) {
		stratum = &norecord_splice;
		reason = "no-record, spliceable";
	}
	if (stratum->empty()) {
		stratum = &keepers;
		reason = "keeper";
	}

	auto dist = std::uniform_int_distribution<std::size_t>(
		0, stratum->size() - 1
	);
	return std::make_pair( (*stratum)[dist(Boss::random_engine)]
			     , reason
			     );
}

}
}}}
