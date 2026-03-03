#ifndef BOLTZ_DETAIL_SERVICEIMPL_HPP
#define BOLTZ_DETAIL_SERVICEIMPL_HPP

#include"Boltz/Connection.hpp"
#include"Boltz/Service.hpp"
#include"Secp256k1/Random.hpp"
#include"Sqlite3/Db.hpp"
#include<cstdint>
#include<functional>
#include<memory>

namespace Bitcoin { class Tx; }
namespace Boltz { class EnvIF; }
namespace Ev { class ThreadPool; }
namespace Secp256k1 { class SignerIF; }

namespace Boltz { namespace Detail {

/** class Boltz::Detail::ServiceImpl
 *
 * @brief actual implementation of `Boltz::Service`
 * returned by `Boltz::ServiceFactory`.
 */
class ServiceImpl : public Service {
private:
	std::string label;
	std::unique_ptr<Boltz::ConnectionIF> conn;
	Sqlite3::Db db;
	Secp256k1::SignerIF& signer;
	Boltz::EnvIF& env;
	Secp256k1::Random random;

public:
	ServiceImpl() =delete;
	ServiceImpl(ServiceImpl&&) =delete;
	ServiceImpl(ServiceImpl const&) =delete;

	ServiceImpl( Sqlite3::Db db_
		   , Secp256k1::SignerIF& signer_
		   , Boltz::EnvIF& env_
		   , std::string label_
		   , std::unique_ptr<Boltz::ConnectionIF> conn_
		   ) : label(std::move(label_))
		     , conn(std::move(conn_))
		     , db(std::move(db_))
		     , signer(signer_)
		     , env(env_)
		     { }

	Ev::Io<void> on_block(std::uint32_t) override;
	Ev::Io<std::unique_ptr<Ln::Amount>>
	get_quotation(Ln::Amount) override;
	Ev::Io<SwapInfo>
	swap( Ln::Amount
	    , std::string onchain_address
	    , std::uint32_t current_blockheight
	    ) override;

private:
	/* Logging.  */
	Ev::Io<void> logd(std::string);
	Ev::Io<void> loge(std::string);
	std::string prefixlog(std::string);

	/* Called for each ongoing swap, when a new block arrives.  */
	Ev::Io<void> swap_on_block( std::uint32_t blockheight
				  , std::string swapId
				  );
	/* Called when the server thinks the swap will no longer push
	 * through.  */
	Ev::Io<void> swap_errored(std::shared_ptr<std::string> swapId);
	/* Called when the server says the transaction has confirmed.  */
	Ev::Io<void> swap_onchain( std::shared_ptr<std::string> swapId
				 , std::uint32_t blockheight
				 , std::shared_ptr<Bitcoin::Tx> tx
				 );
	/* Called when we think the swap is completed.  */
	Ev::Io<void> swap_finished(std::shared_ptr<std::string> swapId);

	/* Delete the swap.  */
	Ev::Io<void> delete_swap(std::shared_ptr<std::string> swapId);
};

}}

#endif /* !defined(BOLTZ_DETAIL_SERVICEIMPL_HPP) */
