#include<string.h>
#include<memory>
#include<type_traits>
#include<secp256k1.h>
#include"Secp256k1/G.hpp"
#include"Secp256k1/PubKey.hpp"

/* FIXME: Load G as a constant directly.  */
/* G = 0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798 */
namespace {

std::uint8_t g[33] = {
	0x02, /* <-- this byte encodes (even|odd)ness for 33 byte keys only */
	0x79, 0xBE, 0x66, 0x7E,
	0xF9, 0xDC, 0xBB, 0xAC,
	0x55, 0xA0, 0x62, 0x95,
	0xCE, 0x87, 0x0B, 0x07,
	0x02, 0x9B, 0xFC, 0xDB,
	0x2D, 0xCE, 0x28, 0xD9,
	0x59, 0xF2, 0x81, 0x5B,
	0x16, 0xF8, 0x17, 0x98
};

template <typename P>
P make_g() {
	/* Create a temporary context for ourself; we cannot be
	 * certain that Secp256k1::Detail::context has been
	 * properly initialized yet!
	 */
	auto ctx = secp256k1_context_create( SECP256K1_CONTEXT_SIGN
					   | SECP256K1_CONTEXT_VERIFY
					   );
	/* Auto-frees the context later.  */
	auto handler = std::shared_ptr<secp256k1_context_struct>( ctx
								, &secp256k1_context_destroy
								);
	bool tiebreaker;
	if constexpr (std::is_same_v<P, Secp256k1::PubKey>)
		tiebreaker = true;
	else if (std::is_same_v<P, Secp256k1::XonlyPubKey>)
		tiebreaker = false;
	else
		throw Secp256k1::InvalidPubKey();

	/* the 0x02 zero-th byte is not part of the value, it indicates
	 * whether a point is even or odd for 33 byte keys (described
	 * in the bips as the "tie-breaker" byte). we discard it
	 * for 32 byte x-only keys, which are always even.  */
	return P::from_buffer_with_context( handler.get()
									  , &g[ (tiebreaker ? 0 : 1) ]
									  );
}

}

namespace Secp256k1 {

PubKey G_tied = make_g<Secp256k1::PubKey>();
XonlyPubKey G_xcoord = make_g<Secp256k1::XonlyPubKey>();

}
