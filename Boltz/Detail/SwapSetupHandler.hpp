#ifndef BOLTZ_DETAIL_SWAPSETUPHANDLER_HPP
#define BOLTZ_DETAIL_SWAPSETUPHANDLER_HPP

#include"Ln/Amount.hpp"
#include"Ln/Preimage.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Ripemd160/Hash.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3/Db.hpp"
#include<cstdint>
#include<memory>
#include<string>
#include<utility>
#include<vector>

namespace Boltz { class ConnectionIF; }
namespace Boltz { class EnvIF; }
namespace Boltz { struct SwapInfo; }
namespace Ev { template<typename a> class Io; }
namespace Secp256k1 { class SignerIF; }

namespace Boltz { namespace Detail {

/** class Boltz::Detail::SwapSetupHandler
 *
 * @brief sets up a reverse submarine swap with
 * the given server, updating the database
 * appropriately.
 */
class SwapSetupHandler
	: public std::enable_shared_from_this<SwapSetupHandler> {
private:
	Secp256k1::SignerIF& signer;
	Sqlite3::Db db;
	Boltz::EnvIF& env;
	std::string api_endpoint;
	Boltz::ConnectionIF& conn;
	Secp256k1::Random& random;
	std::string destinationAddress;
	Ln::Amount offchainAmount;
	std::uint32_t current_blockheight;

	SwapSetupHandler( Secp256k1::SignerIF& signer_
			, Sqlite3::Db db_
			, Boltz::EnvIF& env_
			, std::string const& api_endpoint_
			, Boltz::ConnectionIF& conn_
			, Secp256k1::Random& random_
			, std::string const& destinationAddress_
			, Ln::Amount offchainAmount_
			, std::uint32_t current_blockheight_
			) : signer(signer_)
			  , db(std::move(db_))
			  , env(env_)
			  , api_endpoint(api_endpoint_)
			  , conn(conn_)
			  , random(random_)
			  , destinationAddress(destinationAddress_)
			  , offchainAmount(offchainAmount_)
			  , current_blockheight(current_blockheight_)

			  , tweak(random)
			  , preimage(random)
			  { }

public:
	static
	std::shared_ptr<SwapSetupHandler>
	create( Secp256k1::SignerIF& signer
	      , Sqlite3::Db db
	      , Boltz::EnvIF& env
	      , std::string const& api_endpoint
	      , Boltz::ConnectionIF& conn
	      , Secp256k1::Random& random
	      , std::string const& destinationAddress
	      , Ln::Amount offchainAmount
	      , std::uint32_t current_blockheight
	      ) {
		return std::shared_ptr<SwapSetupHandler>(
			new SwapSetupHandler( signer
					    , std::move(db)
					    , env
					    , api_endpoint
					    , conn
					    , random
					    , destinationAddress
					    , offchainAmount
					    , current_blockheight
					    )
		);
	}

	/* Returns the invoice and the absolute timeout of the
	 * swap.  */
	Ev::Io<SwapInfo> run();
private:
	Ev::Io<void> core_run();

	Ev::Io<void> logd(std::string);
	Ev::Io<void> loge(std::string);
	std::string prefixlog(std::string);

	/* Data we generate.  */
	Secp256k1::PrivKey tweak;
	Ln::Preimage preimage;
	Ln::Preimage real_preimage;
	/* Derived from above.  */
	Secp256k1::PubKey tweakPubKey;
	Sha256::Hash preimageHash;
	Ripemd160::Hash preimageHash160;

	/* Data from server.  */
	std::unique_ptr<std::string> swapId;
	std::string refundPubkey;
	std::vector<std::uint8_t> claimScript;
	std::vector<std::uint8_t> refundScript;
	std::uint32_t timeoutBlockheight;
	Ln::Amount onchainAmount;

	/* The invoice to pay, or empty string on failure.  */
	std::string invoice;
};

}}

#endif /* !defined(BOLTZ_DETAIL_SWAPSETUPHANDLER_HPP) */
