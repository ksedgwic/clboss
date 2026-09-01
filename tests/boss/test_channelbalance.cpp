#undef NDEBUG
#include"Boss/Mod/ChannelBalance.hpp"
#include"Jsmn/Object.hpp"
#include"Ln/Amount.hpp"
#include<assert.h>
#include<sstream>
#include<string>

/* channel_balance deducts a pending splice-out from a
 * listpeerchannels channel, as channeld does for HTLC admission, and
 * leaves everything else alone.  */

namespace {

Jsmn::Object parse(std::string const& text) {
	auto is = std::stringstream(text);
	auto rv = Jsmn::Object();
	is >> rv;
	return rv;
}

}

int main() {
	/* No splice pending: the plain numbers.  */
	{
		auto c = parse(R"JSON(
		{ "state": "CHANNELD_NORMAL"
		, "to_us_msat": "12031817269msat"
		, "total_msat": "13314982000msat"
		}
		)JSON");
		auto bal = Boss::Mod::channel_balance(c);
		assert(bal.to_us == Ln::Amount::msat(12031817269));
		assert(bal.total == Ln::Amount::msat(13314982000));
	}
	/* An empty inflight list: the same.  */
	{
		auto c = parse(R"JSON(
		{ "state": "CHANNELD_AWAITING_SPLICE"
		, "to_us_msat": "12031817269msat"
		, "total_msat": "13314982000msat"
		, "inflight": [ ]
		}
		)JSON");
		auto bal = Boss::Mod::channel_balance(c);
		assert(bal.to_us == Ln::Amount::msat(12031817269));
		assert(bal.total == Ln::Amount::msat(13314982000));
	}
	/* A splice-out of 9,100,000 sat plus 199 sat fee: the
	 * post-splice numbers (a mainnet case).  */
	{
		auto c = parse(R"JSON(
		{ "state": "CHANNELD_AWAITING_SPLICE"
		, "to_us_msat": "12031817269msat"
		, "total_msat": "13314982000msat"
		, "inflight":
		  [ { "funding_txid": "17ce0b5edf582c9a0a4070567791f6c0564952f525213ef28c051150a26141c6"
		    , "funding_outnum": 1
		    , "feerate": "253000perkw"
		    , "total_funding_msat": "4214783000msat"
		    , "splice_amount": -9100199
		    , "our_funding_msat": "2931618269msat"
		    }
		  ]
		}
		)JSON");
		auto bal = Boss::Mod::channel_balance(c);
		assert(bal.to_us == Ln::Amount::msat(2931618269));
		assert(bal.total == Ln::Amount::msat(4214783000));
	}
	/* A splice-in is not credited until lock-in.  */
	{
		auto c = parse(R"JSON(
		{ "state": "CHANNELD_AWAITING_SPLICE"
		, "to_us_msat": "1000000000msat"
		, "total_msat": "2000000000msat"
		, "inflight":
		  [ { "total_funding_msat": "3000000000msat"
		    , "splice_amount": 1000000
		    }
		  ]
		}
		)JSON");
		auto bal = Boss::Mod::channel_balance(c);
		assert(bal.to_us == Ln::Amount::msat(1000000000));
		assert(bal.total == Ln::Amount::msat(2000000000));
	}
	/* Several inflight fundings: the lowest splice_amount wins.  */
	{
		auto c = parse(R"JSON(
		{ "state": "CHANNELD_AWAITING_SPLICE"
		, "to_us_msat": "1000000000msat"
		, "total_msat": "2000000000msat"
		, "inflight":
		  [ { "total_funding_msat": "1900000000msat"
		    , "splice_amount": -100000
		    }
		  , { "total_funding_msat": "1800000000msat"
		    , "splice_amount": -200000
		    }
		  ]
		}
		)JSON");
		auto bal = Boss::Mod::channel_balance(c);
		assert(bal.to_us == Ln::Amount::msat(800000000));
		assert(bal.total == Ln::Amount::msat(1800000000));
	}
	/* No total_funding_msat: the capacity shrinks by the same
	 * amount; a splice-out larger than our balance floors at zero.  */
	{
		auto c = parse(R"JSON(
		{ "state": "CHANNELD_AWAITING_SPLICE"
		, "to_us_msat": "100000000msat"
		, "total_msat": "2000000000msat"
		, "inflight": [ { "splice_amount": -150000 } ]
		}
		)JSON");
		auto bal = Boss::Mod::channel_balance(c);
		assert(bal.to_us == Ln::Amount::msat(0));
		assert(bal.total == Ln::Amount::msat(1850000000));
	}
	/* An inflight without splice_amount is ignored.  */
	{
		auto c = parse(R"JSON(
		{ "state": "CHANNELD_AWAITING_SPLICE"
		, "to_us_msat": "1000000000msat"
		, "total_msat": "2000000000msat"
		, "inflight": [ { "total_funding_msat": "2000000000msat" } ]
		}
		)JSON");
		auto bal = Boss::Mod::channel_balance(c);
		assert(bal.to_us == Ln::Amount::msat(1000000000));
		assert(bal.total == Ln::Amount::msat(2000000000));
	}
	return 0;
}
