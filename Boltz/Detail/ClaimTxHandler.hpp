#ifndef BOLTZ_DETAIL_CLAIMTXHANDLER_HPP
#define BOLTZ_DETAIL_CLAIMTXHANDLER_HPP
#include"Bitcoin/Tx.hpp"
#include"Bitcoin/TxId.hpp"
#include"Ln/Amount.hpp"
#include"Ln/Preimage.hpp"
#include"Boltz/Detail/MusigImpl.hpp"
#include"Boltz/Detail/initial_claim_tx.hpp"
#include"Secp256k1/PrivKey.hpp"
#include"Secp256k1/PubKey.hpp"
#include"Sha256/Hash.hpp"
#include"Sqlite3/Db.hpp"
#include<cstdint>
#include<functional>
#include<memory>
#include<optional>
#include<string>
#include<vector>

namespace Boltz { class ConnectionIF; }
namespace Boltz { class EnvIF; }
namespace Ev { template<typename a> class Io; }
namespace Secp256k1 { class SignerIF; }

namespace Boltz { namespace Detail {

/** class Boltz::Detail::ClaimTxHandler
 *
 * @brief handles the claim transaction.
 *
 * @desc this is constructed and called at each
 * block for every swap whose lockup the provider
 * reports confirmed, and, once the preimage has
 * been revealed, at each block until our own node
 * sees the claim confirmed, whatever the provider
 * reports.  The claim is cooperative, one
 * aggregate signature on the key path, when the
 * provider co-signs; otherwise it runs the claim
 * leaf with the preimage and our signature alone.
 */
class ClaimTxHandler : public std::enable_shared_from_this<ClaimTxHandler> {
private:
	Secp256k1::SignerIF& signer;
	Sqlite3::Db db;
	Boltz::EnvIF& env;
	std::string api_endpoint;
	Boltz::ConnectionIF& conn;
	Secp256k1::Random& random;
	std::string swapId;
	std::uint32_t blockheight;
	/* The lockup transaction the provider reported; for a swap
	 * whose preimage is already revealed, the stored one.  */
	Bitcoin::Tx lockup_tx;
	Bitcoin::TxId lockup_txid;

	ClaimTxHandler( Secp256k1::SignerIF& signer_
		      , Sqlite3::Db db_
		      , Boltz::EnvIF& env_
		      , std::string const& api_endpoint_
		      , Boltz::ConnectionIF& conn_
		      , Secp256k1::Random& random_
		      , std::string const& swapId_
		      , std::uint32_t blockheight_
		      , Bitcoin::Tx lockup_tx_
		      ) : signer(signer_)
			, db(std::move(db_))
			, env(env_)
			, api_endpoint(api_endpoint_)
			, conn(conn_)
			, random(random_)
			, swapId(swapId_)
			, blockheight(blockheight_)
			, lockup_tx(std::move(lockup_tx_))
			{
		lockup_txid = lockup_tx.get_txid();
	}

public:
	static
	std::shared_ptr<ClaimTxHandler>
	create( Secp256k1::SignerIF& signer
	      , Sqlite3::Db db
	      , Boltz::EnvIF& env
	      , std::string const& api_endpoint
	      , Boltz::ConnectionIF& conn
	      , Secp256k1::Random& random
	      , std::string const& swapId
	      , std::uint32_t blockheight
	      , Bitcoin::Tx lockup_tx
	      ) {
		return std::shared_ptr<ClaimTxHandler>(
			new ClaimTxHandler( signer
					  , std::move(db)
					  , env
					  , api_endpoint
					  , conn
					  , random
					  , swapId
					  , blockheight
					  , std::move(lockup_tx)
					  )
		);
	}

	Ev::Io<void> run();

private:
	Ev::Io<void> core_run();
	/* Loads the row; ends the run if there is none to claim.  */
	Ev::Io<void> load_row();
	/* Builds the MuSig2 session and the lockup script from the
	 * row's keys and leaves.  */
	Ev::Io<void> prepare_keys();
	/* Finds the lockup in our own node's blocks and waits for
	 * its depth, or ends the run without revealing anything.  */
	Ev::Io<void> confirm_lockup();
	/* Builds the unsigned claim at the given feerate for the
	 * given path and returns the sighash that path signs, or
	 * nothing when the fee would consume the lockup.  */
	Ev::Io<std::optional<Sha256::Hash>> build_claim( std::uint32_t feerate
						       , ClaimPath path
						       );
	/* Replaces the recorded claim with one at the current
	 * feerate when that is higher.  False when the recorded
	 * claim is kept.  */
	Ev::Io<bool> bump_claim(std::uint32_t feerate);
	/* Records that the preimage is about to leave the node.  */
	Ev::Io<void> persist_reveal();
	/* Claims the lockup: cooperatively with the key-path
	 * sighash of the claim already built, or, when the provider
	 * does not co-sign, through the script path at the given
	 * feerate.  */
	Ev::Io<void> claim(Sha256::Hash keypath_sighash, std::uint32_t feerate);
	/* Asks the provider to co-sign, completes the signature,
	 * broadcasts and records the claim.  False when the
	 * provider's part failed and nothing was broadcast.  */
	Ev::Io<bool> cooperative_claim(Sha256::Hash sighash);
	/* Rebuilds the claim for the script path, signs it with our
	 * key, adds the preimage, the leaf and its control block,
	 * broadcasts and records it.  */
	Ev::Io<void> script_path_claim(std::uint32_t feerate);
	/* Broadcasts the finished claim and records it for
	 * rebroadcast; ends the run if the node rejects it.  */
	Ev::Io<void> broadcast_and_record(std::string const& how);
	/* For a revealed swap: finishes it once our node has the
	 * claim confirmed, or once a spend of the lockup shows in
	 * our blocks; else keeps the claim in mempools, or tries
	 * the claim again if none was ever broadcast.  */
	Ev::Io<void> track_claim();
	/* A transaction found spending the lockup output, and the
	 * block it is in.  */
	struct LockupSpend {
		std::uint32_t height;
		Bitcoin::Tx tx;
	};
	/* Searches our node's blocks, from where the last search
	 * stopped, for a spend of the lockup output.  */
	Ev::Io<std::unique_ptr<LockupSpend>> find_lockup_spend();
	/* Removes the swap's row: the swap is over.  */
	Ev::Io<void> remove_row();

	/* Wrappers.  */
	Ev::Io<void> logd(std::string);
	Ev::Io<void> loge(std::string);
	std::string logprefix(std::string);

	/* Data from the row.  */
	Secp256k1::PrivKey tweak;
	Ln::Preimage preimage;
	Ln::Preimage real_preimage;
	std::string destinationAddress;
	std::vector<std::uint8_t> claimScript;
	std::vector<std::uint8_t> refundScript;
	Secp256k1::PubKey refund_pubkey;
	std::uint32_t timeoutBlockheight;
	Ln::Amount onchainAmount;
	std::uint32_t createdHeight;
	/* 0 until the lockup is found in one of our node's blocks.  */
	std::uint32_t lockupConfirmedHeight;
	/* 0 until a block has been searched for it.  */
	std::uint32_t lockupSearchedHeight;
	/* 0 until a block has been searched for a spend of the
	 * lockup output.  */
	std::uint32_t spendSearchedHeight;
	/* Set once the preimage has been sent to the provider.  */
	bool preimageRevealed;
	/* The lockup as stored at the reveal, hex; "" before.  */
	std::string stored_lockup_tx;
	/* The last signed claim, hex, and its feerate; "" and 0
	 * before a claim was broadcast.  Its fee is in
	 * lockupClaimFees below.  */
	std::string claimTxHex;
	std::uint32_t claimFeerate;

	/* Data we generate.  */
	std::vector<std::uint8_t> redeemScript;
	Boltz::MusigSession musigsession;
	std::string local_pubnonce;
	/* Data we will store into the table.  */
	std::size_t lockupOut;
	Ln::Amount lockupClaimFees;
	/* Actual claim transaction.  */
	Bitcoin::Tx claim_tx;
};

}}

#endif /* !defined(BOLTZ_DETAIL_CLAIMTXHANDLER_HPP) */
