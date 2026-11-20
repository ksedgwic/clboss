#undef NDEBUG
#include"Boltz/Detail/node_auth_message.hpp"
#include<assert.h>

/* The messages the provider expects signed, as boltz-backend builds
 * them for the same request.  */

int main() {
	assert( Boltz::Detail::node_auth_message(
			1700000000, "POST", "/v2/swap/reverse"
		      , "{\"invoiceAmount\":100000}"
		)
	     == "boltz-api-node-auth:1700000000:POST:/v2/swap/reverse:"
		"0ac420b311d4e52afd9b8161769f860fb9a82fc64187a029bd698dd1f87ee5bf"
	      );
	/* A GET has no body; its hash is that of the empty string.  */
	assert( Boltz::Detail::node_auth_message(
			1700000000, "GET", "/v2/swap/reverse", ""
		)
	     == "boltz-api-node-auth:1700000000:GET:/v2/swap/reverse:"
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	      );

	return 0;
}
