#include"Boltz/Detail/ServiceImpl.hpp"
#include"Boltz/Detail/create_connection.hpp"
#include"Boltz/Service.hpp"
#include"Boltz/ServiceFactory.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Ev/yield.hpp"
#include"Secp256k1/SignerIF.hpp"
#include"Sqlite3.hpp"
#include"Util/make_unique.hpp"
#include<set>
#include<string>

namespace Boltz {

class ServiceFactory::Impl {
private:
	Ev::ThreadPool& threadpool;
	Sqlite3::Db db;
	Secp256k1::SignerIF& signer;
	Boltz::EnvIF& env;
	std::string proxy;
	bool always_use_proxy;

	/* nullptr if we are currently initializing,
	 * pointer to false if currently uninitialized,
	 * pointer to true if already initialized.  */
	std::unique_ptr<bool> initted_flag;

	Ev::Io<void> try_initialize() {
		return Ev::yield().then([this]() {
			if (!initted_flag)
				/* Busy wait.  */
				return Ev::yield().then([this]() {
					return try_initialize();
				});
			if (*initted_flag)
				/* Do nothing.  */
				return Ev::lift();
			/* Start initializing.  */
			initted_flag = nullptr;
			return do_initialize();
		});
	}
	/* Columns the API v2 code added.  A database from the v1
	 * code gets them in place; the v1 column stays, so that
	 * earlier releases still run on the upgraded database.  */
	static void add_missing_columns(Sqlite3::Tx& tx) {
		auto existing = std::set<std::string>();
		auto fetch = tx.query(R"QRY(
		SELECT name FROM pragma_table_info('BoltzServiceFactory_rsub');
		)QRY").execute();
		for (auto& r : fetch)
			existing.insert(r.get<std::string>(0));
		struct Column { char const* name; char const* type; };
		for (auto const& col : { Column{"claimScript", "TEXT NOT NULL DEFAULT ''"}
				       , Column{"refundScript", "TEXT NOT NULL DEFAULT ''"}
				       , Column{"refundPubkey", "TEXT NOT NULL DEFAULT ''"}
				       , Column{"createdHeight", "INTEGER NOT NULL DEFAULT 0"}
				       , Column{"lockupSearchedHeight", "INTEGER NULL"}
				       }) {
			if (existing.count(col.name))
				continue;
			tx.query_execute( std::string("ALTER TABLE \"BoltzServiceFactory_rsub\" ")
					+ "ADD COLUMN " + col.name
					+ " " + col.type + ";"
					);
		}
	}

	Ev::Io<void> do_initialize() {
		/* Perform actual initialization.  */
		return db.transact().then([this](Sqlite3::Tx tx) {
			tx.query_execute(R"QRY(
			-- reverse submarine swaps.
			CREATE TABLE IF NOT EXISTS "BoltzServiceFactory_rsub"
			     ( id INTEGER PRIMARY KEY

			     -- The Boltz implementation that generated
			     -- this row.
			     -- Historically this was the access point,
			     -- but now it is "just" a label naming the
			     -- Boltz implementation.
			     , apiAccess TEXT NOT NULL

			     -- We generated these.
			     -- privkey used to tweak the signer.
			     -- these, and other data, are in hex.
			     , tweak TEXT NOT NULL
			     -- LN preimage.
			     , preimage TEXT NOT NULL
			     -- onchain address the C-Lightning node
			     -- controls.
			     , destinationAddress TEXT NOT NULL

			     -- from exchange, before paying invoice.
			     -- id from exchange.
			     , swapId TEXT NOT NULL
			     -- API v1: the P2WSH script of the swap.
			     -- Empty for v2 swaps; kept so that releases
			     -- before API v2 run on this database.
			     , redeemScript TEXT NOT NULL DEFAULT ''
			     -- API v2: the two tapscript leaves and the
			     -- provider's key, from the swap creation.
			     -- Empty for v1 swaps.
			     , claimScript TEXT NOT NULL DEFAULT ''
			     , refundScript TEXT NOT NULL DEFAULT ''
			     , refundPubkey TEXT NOT NULL DEFAULT ''
			     , timeoutBlockheight INTEGER NOT NULL
			     -- in satoshi.
			     , onchainAmount INTEGER NOT NULL
			     -- The block height when the swap was
			     -- created.  The search of our own node's
			     -- blocks for the lockup transaction starts
			     -- there.
			     , createdHeight INTEGER NOT NULL DEFAULT 0

			     -- whether lockup fields have been set.
			     -- 0 == false, 1 == true
			     , lockedUp INTEGER NOT NULL

			     -- from the exchange, after paying invoice.
			     -- These are the details of the lockup
			     -- transaction output.
			     , lockupTxid TEXT NULL
			     -- The output of the lockupTxid that contains
			     -- the value spendable by the redeem script.
			     , lockupOut INTEGER NULL
			     -- The height of the block that holds the
			     -- lockup transaction, found by searching
			     -- our own node's blocks; NULL until found.
			     , lockupConfirmedHeight INTEGER NULL
			     -- Blocks up to and including this height
			     -- were searched for the lockup transaction
			     -- without finding it; NULL when none were.
			     , lockupSearchedHeight INTEGER NULL

			     -- We generated these on seeing that
			     -- the transaction from the service
			     -- confirmed.
			     -- in satoshi.
			     , lockupClaimFees INTEGER NULL

			     -- additional data we might need.
			     , comment TEXT
			     );
			CREATE INDEX IF NOT EXISTS
			       "BoltzServiceFactory_rsub_swapid"
			    ON "BoltzServiceFactory_rsub"
			     ( swapId
			     , apiAccess
			     );
			)QRY");
			add_missing_columns(tx);
			tx.commit();

			initted_flag = Util::make_unique<bool>(true);

			return Ev::yield();
		});
	}

public:
	Impl() =delete;

	Impl( Ev::ThreadPool& threadpool_
	    , Sqlite3::Db db_
	    , Secp256k1::SignerIF& signer_
	    , Boltz::EnvIF& env_
	    , std::string proxy_
	    , bool always_use_proxy_
	    ) : threadpool(threadpool_)
	      , db(std::move(db_))
	      , signer(signer_)
	      , env(env_)
	      , proxy(std::move(proxy_))
	      , always_use_proxy(always_use_proxy_)
	      , initted_flag(Util::make_unique<bool>(false))
	      { }

	Ev::Io<std::unique_ptr<Service>>
	create_service_detailed( std::string const& label
			       , std::string const& clearnet
			       , std::string const& onion
			       ) {
		return try_initialize().then([this, label, clearnet, onion]() {
			auto ptr = std::unique_ptr<Service>();
			ptr = Util::make_unique<Detail::ServiceImpl>
				( db
				, signer
				, env
				, label
				, Detail::create_connection( threadpool
							   , clearnet
							   , onion
							   , proxy
							   , always_use_proxy
							   )
				);
			return Ev::lift(std::move(ptr));
		});
	}
};

ServiceFactory::ServiceFactory(ServiceFactory&&) =default;
ServiceFactory& ServiceFactory::operator=(ServiceFactory&&) =default;
ServiceFactory::~ServiceFactory() =default;

ServiceFactory::ServiceFactory( Ev::ThreadPool& threadpool
			      , Sqlite3::Db db
			      , Secp256k1::SignerIF& signer
			      , Boltz::EnvIF& env
			      , std::string proxy
			      , bool always_use_proxy
			      ) : pimpl(Util::make_unique<Impl>( threadpool
							       , std::move(db)
							       , signer
							       , env
							       , std::move(proxy)
							       , always_use_proxy
							       ))
				{ }

Ev::Io<std::unique_ptr<Service>>
ServiceFactory::create_service_detailed( std::string const& label
				       , std::string const& clearnet
				       , std::string const& onion
				       ) {
	return pimpl->create_service_detailed(label, clearnet, onion);
}

}
