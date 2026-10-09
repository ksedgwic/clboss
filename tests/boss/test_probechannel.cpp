#undef NDEBUG
#include"Boss/Mod/ProbeChannel.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/Amount.hpp"
#include"Ln/Scid.hpp"
#include<assert.h>
#include<sstream>
#include<string>

/* probe_channel picks the peer's CHANNELD_NORMAL channel with the most to
 * spend.  The probe is sent over that one channel by name, so with several
 * channels to a peer the amount must fit the channel, not the sum (#352).  */

namespace {

Jsmn::Object parse(std::string const& text) {
	auto is = std::stringstream(text);
	auto rv = Jsmn::Object();
	is >> rv;
	return rv;
}

}

int main() {
	/* The larger channel is listed second: it is still chosen.  */
	{
		auto cs = parse(R"JSON(
		[ { "state": "CHANNELD_NORMAL"
		  , "short_channel_id": "100x1x0"
		  , "spendable_msat": "900000000msat"
		  }
		, { "state": "CHANNELD_NORMAL"
		  , "short_channel_id": "200x2x0"
		  , "spendable_msat": "4500000000msat"
		  }
		]
		)JSON");
		auto pc = Boss::Mod::probe_channel(cs);
		assert(pc.scid == Ln::Scid(std::string("200x2x0")));
		assert(pc.spendable == Ln::Amount::msat(4500000000));
	}
	/* Only CHANNELD_NORMAL with a short channel id and spendable_msat
	 * counts; a larger channel in another state is skipped.  */
	{
		auto cs = parse(R"JSON(
		[ { "state": "CHANNELD_AWAITING_SPLICE"
		  , "short_channel_id": "300x3x0"
		  , "spendable_msat": "9000000000msat"
		  }
		, { "state": "CHANNELD_NORMAL"
		  , "spendable_msat": "8000000000msat"
		  }
		, { "state": "CHANNELD_NORMAL"
		  , "short_channel_id": "100x1x0"
		  , "spendable_msat": "700000000msat"
		  }
		, { "state": "ONCHAIN"
		  , "short_channel_id": "400x4x0"
		  , "spendable_msat": "7000000000msat"
		  }
		]
		)JSON");
		auto pc = Boss::Mod::probe_channel(cs);
		assert(pc.scid == Ln::Scid(std::string("100x1x0")));
		assert(pc.spendable == Ln::Amount::msat(700000000));
	}
	/* No usable channel: no scid.  */
	{
		auto cs = parse(R"JSON(
		[ { "state": "CHANNELD_SHUTTING_DOWN"
		  , "short_channel_id": "100x1x0"
		  , "spendable_msat": "700000000msat"
		  }
		]
		)JSON");
		auto pc = Boss::Mod::probe_channel(cs);
		assert(!pc.scid);
	}
	return 0;
}
