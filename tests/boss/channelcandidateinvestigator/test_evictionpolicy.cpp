#undef NDEBUG
#include"Boss/Mod/ChannelCandidateInvestigator/EvictionPolicy.hpp"
#include"Boss/Msg/TrackRecord.hpp"
#include"Ln/NodeId.hpp"
#include<assert.h>
#include<map>
#include<set>
#include<string.h>
#include<string>
#include<vector>

namespace {

/* Distinct valid node ids.  */
auto const keeper = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
auto const under = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");
auto const norec_plain = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000003");
auto const norec_splice = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000004");
auto const unknown = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000005");

using Boss::Msg::TrackRecordVerdict;
namespace EvictionPolicy = Boss::Mod::ChannelCandidateInvestigator::EvictionPolicy;

std::map<Ln::NodeId, TrackRecordVerdict> verdicts() {
	auto rv = std::map<Ln::NodeId, TrackRecordVerdict>();
	rv[keeper] = TrackRecordVerdict::Keeper;
	rv[under] = TrackRecordVerdict::Underperformer;
	rv[norec_plain] = TrackRecordVerdict::NoRecord;
	rv[norec_splice] = TrackRecordVerdict::NoRecord;
	return rv;
}

}

int main() {
	auto spliceable = std::set<Ln::NodeId>{norec_splice};

	/* An underperformer is always the first choice.  */
	{
		auto pool = std::vector<Ln::NodeId>{
			keeper, norec_splice, under, norec_plain
		};
		auto r = EvictionPolicy::pick(pool, verdicts(), spliceable);
		assert(r.first == under);
		assert(0 == strcmp(r.second, "underperformer"));
	}

	/* Without an underperformer, a non-spliceable no-record
	 * candidate goes before a spliceable one.  */
	{
		auto pool = std::vector<Ln::NodeId>{
			keeper, norec_splice, norec_plain
		};
		auto r = EvictionPolicy::pick(pool, verdicts(), spliceable);
		assert(r.first == norec_plain);
		assert(0 == strcmp(r.second, "no-record"));
	}

	/* A node absent from the verdicts map counts as no-record.  */
	{
		auto pool = std::vector<Ln::NodeId>{
			keeper, norec_splice, unknown
		};
		auto r = EvictionPolicy::pick(pool, verdicts(), spliceable);
		assert(r.first == unknown);
		assert(0 == strcmp(r.second, "no-record"));
	}

	/* All remaining no-records spliceable: one of them still goes,
	 * before any keeper.  */
	{
		auto pool = std::vector<Ln::NodeId>{keeper, norec_splice};
		auto r = EvictionPolicy::pick(pool, verdicts(), spliceable);
		assert(r.first == norec_splice);
		assert(0 == strcmp(r.second, "no-record, spliceable"));
	}

	/* A pool of only keepers still evicts.  */
	{
		auto pool = std::vector<Ln::NodeId>{keeper};
		auto r = EvictionPolicy::pick(pool, verdicts(), spliceable);
		assert(r.first == keeper);
		assert(0 == strcmp(r.second, "keeper"));
	}

	return 0;
}
