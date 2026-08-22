#undef NDEBUG
#include"Boss/Mod/GetroutesFirstHop.hpp"
#include"Jsmn/Object.hpp"
#include"Jsmn/Parser.hpp"
#include"Ln/Amount.hpp"
#include"Ln/NodeId.hpp"
#include<assert.h>
#include<stdexcept>
#include<string>

namespace {

auto const node_b = std::string
	("020000000000000000000000000000000000000000000000000000000000000002");
auto const node_c = std::string
	("020000000000000000000000000000000000000000000000000000000000000003");

Jsmn::Object parse(std::string const& s) {
	auto parser = Jsmn::Parser();
	auto objs = parser.feed(s + "\n");
	assert(objs.size() == 1);
	return objs[0];
}

}

int main() {
	/* A stock v26.04 route: hops carry only the deprecated trio,
	 * whose amount and delay are in-side values.  Two hops,
	 * 1000msat fee at the first, 500 at the second.  */
	auto v2604 = parse(R"JSON(
	{ "probability_ppm": 900000
	, "amount_msat": 100000
	, "final_cltv": 18
	, "path":
	  [ { "short_channel_id_dir": "100x1x0/1"
	    , "next_node_id": ")JSON" + node_b + R"JSON("
	    , "amount_msat": 101500
	    , "delay": 60
	    }
	  , { "short_channel_id_dir": "200x1x0/0"
	    , "next_node_id": ")JSON" + node_c + R"JSON("
	    , "amount_msat": 100500
	    , "delay": 40
	    }
	  ]
	}
	)JSON");
	{
		/* First hop out-values = second hop's in-values.  */
		auto hop = Boss::Mod::GetroutesFirstHop(v2604);
		assert(hop.node_id_out == Ln::NodeId(node_b));
		assert(hop.amount_out == Ln::Amount::msat(100500));
		assert(hop.cltv_out == 40);
	}

	/* A stock v26.04 single-hop route: out-values = the route's
	 * delivered amount_msat and final_cltv.  */
	auto v2604_single = parse(R"JSON(
	{ "probability_ppm": 900000
	, "amount_msat": 100000
	, "final_cltv": 18
	, "path":
	  [ { "short_channel_id_dir": "100x1x0/1"
	    , "next_node_id": ")JSON" + node_b + R"JSON("
	    , "amount_msat": 101000
	    , "delay": 33
	    }
	  ]
	}
	)JSON");
	{
		auto hop = Boss::Mod::GetroutesFirstHop(v2604_single);
		assert(hop.node_id_out == Ln::NodeId(node_b));
		assert(hop.amount_out == Ln::Amount::msat(100000));
		assert(hop.cltv_out == 18);
	}

	/* A v26.06 route: the out-side fields are read directly.
	 * The deprecated trio is present with in-side values that
	 * differ from the out-side ones; the new fields must win.  */
	auto v2606 = parse(R"JSON(
	{ "probability_ppm": 900000
	, "amount_msat": 100000
	, "final_cltv": 18
	, "path":
	  [ { "short_channel_id_dir": "100x1x0/1"
	    , "next_node_id": ")JSON" + node_b + R"JSON("
	    , "amount_msat": 101500
	    , "delay": 60
	    , "node_id_out": ")JSON" + node_b + R"JSON("
	    , "amount_in_msat": 101500
	    , "cltv_in": 60
	    , "amount_out_msat": 100500
	    , "cltv_out": 40
	    }
	  ]
	}
	)JSON");
	{
		auto hop = Boss::Mod::GetroutesFirstHop(v2606);
		assert(hop.node_id_out == Ln::NodeId(node_b));
		assert(hop.amount_out == Ln::Amount::msat(100500));
		assert(hop.cltv_out == 40);
	}

	/* A hop carrying neither form must throw, not fabricate.  */
	auto malformed = parse(R"JSON(
	{ "probability_ppm": 900000
	, "amount_msat": 100000
	, "final_cltv": 18
	, "path":
	  [ { "short_channel_id_dir": "100x1x0/1" } ]
	}
	)JSON");
	{
		auto thrown = false;
		try {
			auto hop = Boss::Mod::GetroutesFirstHop(malformed);
			(void) hop;
		} catch (std::exception const&) {
			thrown = true;
		}
		assert(thrown);
	}

	return 0;
}
