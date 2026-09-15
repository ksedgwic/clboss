#undef NDEBUG
#include"Boss/Mod/ChanneledPeers.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/NodeId.hpp"
#include<assert.h>
#include<sstream>
#include<string>

/* channeled_state counts opening and open channels, not closing or
 * closed ones, and channeled_peers collects the peers that have one
 * from a listpeerchannels result (#332).  */

namespace {

auto const A = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000000");
auto const B = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000001");
auto const C = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000002");
auto const D = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000003");
auto const E = Ln::NodeId("020000000000000000000000000000000000000000000000000000000000000004");

Jsmn::Object parse(std::string const& text) {
	auto is = std::stringstream(text);
	auto rv = Jsmn::Object();
	is >> rv;
	return rv;
}

}

int main() {
	using Boss::Mod::channeled_state;
	using Boss::Mod::channeled_peers;

	/* Opening and open.  */
	assert(channeled_state("OPENINGD"));
	assert(channeled_state("CHANNELD_AWAITING_LOCKIN"));
	assert(channeled_state("CHANNELD_NORMAL"));
	assert(channeled_state("CHANNELD_AWAITING_SPLICE"));
	assert(channeled_state("CHANNELD_SHUTTING_DOWN"));
	assert(channeled_state("DUALOPEND_OPEN_INIT"));
	assert(channeled_state("DUALOPEND_OPEN_COMMITTED"));
	assert(channeled_state("DUALOPEND_AWAITING_LOCKIN"));
	/* Closing and closed.  */
	assert(!channeled_state("CLOSINGD_SIGEXCHANGE"));
	assert(!channeled_state("CLOSINGD_COMPLETE"));
	assert(!channeled_state("AWAITING_UNILATERAL"));
	assert(!channeled_state("FUNDING_SPEND_SEEN"));
	/* Shorter than the prefixes that count.  */
	assert(!channeled_state("ONCHAIN"));
	assert(!channeled_state("CHANNEL"));
	assert(!channeled_state(""));

	/* A: open.  B: dual-funded open in progress.  C: closing.
	 * D: closed and, in a second channel, open again.  E: no
	 * readable state.  */
	auto res = parse(R"JSON(
	{ "channels":
	  [ { "peer_id": "020000000000000000000000000000000000000000000000000000000000000000"
	    , "state": "CHANNELD_NORMAL" }
	  , { "peer_id": "020000000000000000000000000000000000000000000000000000000000000001"
	    , "state": "DUALOPEND_AWAITING_LOCKIN" }
	  , { "peer_id": "020000000000000000000000000000000000000000000000000000000000000002"
	    , "state": "CLOSINGD_COMPLETE" }
	  , { "peer_id": "020000000000000000000000000000000000000000000000000000000000000003"
	    , "state": "ONCHAIN" }
	  , { "peer_id": "020000000000000000000000000000000000000000000000000000000000000003"
	    , "state": "CHANNELD_NORMAL" }
	  , { "peer_id": "020000000000000000000000000000000000000000000000000000000000000004" }
	  ]
	}
	)JSON");
	auto peers = channeled_peers(res);
	assert(peers.size() == 3);
	assert(peers.count(A) == 1);
	assert(peers.count(B) == 1);
	assert(peers.count(C) == 0);
	assert(peers.count(D) == 1);
	assert(peers.count(E) == 0);

	/* Not a listpeerchannels result at all.  */
	assert(channeled_peers(parse("{}")).empty());
	assert(channeled_peers(parse(R"JSON({"channels": 5})JSON")).empty());

	return 0;
}
