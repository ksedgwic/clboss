#include"Boss/Mod/ChannelCandidateInvestigator/Secretary.hpp"
#include"Boss/Msg/ProposeChannelCandidates.hpp"
#include"Boss/random_engine.hpp"
#include"Ln/NodeId.hpp"
#include"Sqlite3.hpp"
#include<algorithm>
#include<sstream>

namespace Boss { namespace Mod { namespace ChannelCandidateInvestigator {

void Secretary::initialize(Sqlite3::Tx& tx) {
	tx.query_execute(R"QRY(
		CREATE TABLE IF NOT EXISTS "ChannelCandidateInvestigator"
			( proposal TEXT PRIMARY KEY NOT NULL
			, patron TEXT NOT NULL
			, score INTEGER NOT NULL
			);
	)QRY");
}

void Secretary::add_candidate( Sqlite3::Tx& tx
			     , Msg::ProposeChannelCandidates const& candidate
			     ) {
	tx.query(R"QRY(
		INSERT OR IGNORE
		  INTO "ChannelCandidateInvestigator"
		VALUES  ( :proposal
			, :patron
			, 0
			);
	)QRY")
		.bind(":proposal", std::string(candidate.proposal))
		.bind(":patron", std::string(candidate.patron))
		.execute()
		;
}
std::size_t Secretary::get_nonnegative_candidates_count(Sqlite3::Tx& tx) {
	auto res = tx.query(R"QRY(
		SELECT COUNT(proposal)
		  FROM "ChannelCandidateInvestigator"
		 WHERE score >= 0;
	)QRY").execute();
	for (auto& r : res) {
		return r.get<std::size_t>(0);
	}
	return std::size_t();
}

std::vector<Ln::NodeId>
Secretary::get_for_investigation( Sqlite3::Tx& tx
				, std::size_t max_candidates
				) {
	auto res = tx.query(R"QRY(
		SELECT proposal
		  FROM "ChannelCandidateInvestigator";
	)QRY").execute();
	auto proposals = std::vector<Ln::NodeId>();
	for (auto& r : res) {
		proposals.emplace_back(r.get<std::string>(0));
	}

	/* If fewer than max candidates, return it now.  */
	if (proposals.size() <= max_candidates)
		return proposals;

	/* Shuffle, then trim to the first max_candidates nodes.  */
	std::shuffle(proposals.begin(), proposals.end(), Boss::random_engine);
	proposals.erase(proposals.begin() + max_candidates, proposals.end());

	return proposals;
}

void Secretary::update_score( Sqlite3::Tx& tx
			    , Ln::NodeId const& node
			    , std::int64_t delta_score
			    , std::int64_t minimum_score
			    , std::int64_t maximum_score
			    ) {
	auto proposal_s = std::string(node);
	/* Update score.  */
	tx.query(R"QRY(
		UPDATE "ChannelCandidateInvestigator"
		   SET score = MIN(:maximum_score, score + :delta_score)
		 WHERE proposal = :proposal
		     ;
	)QRY")
		.bind(":maximum_score", maximum_score)
		.bind(":delta_score", delta_score)
		.bind(":proposal", proposal_s)
		.execute();

	/* Delete it if below minimum_score.  */
	tx.query(R"QRY(
		DELETE
		  FROM "ChannelCandidateInvestigator"
		 WHERE proposal = :proposal
		   AND score < :minimum_score
		     ;
	)QRY")
		.bind(":proposal", proposal_s)
		.bind(":minimum_score", minimum_score)
		.execute();
}


std::vector<Msg::ProposeChannelCandidates>
Secretary::get_for_channeling(Sqlite3::Tx& tx) {
	auto rv = std::vector<Msg::ProposeChannelCandidates>();

	auto res1 = tx.query(R"QRY(
		SELECT proposal, patron
		  FROM "ChannelCandidateInvestigator"
		 WHERE score > 0
		 ORDER BY score DESC
		     ;
	)QRY").execute();
	for (auto& r : res1)
		rv.emplace_back(Msg::ProposeChannelCandidates{
			Ln::NodeId(r.get<std::string>(0)),
			Ln::NodeId(r.get<std::string>(1))
		});
	if (rv.size() > 0)
		return rv;

	/* Fall back if no proposals.  */
	auto res2 = tx.query(R"QRY(
		SELECT proposal, patron
		  FROM "ChannelCandidateInvestigator"
		 ORDER BY score DESC
		     ;
	)QRY").execute();
	for (auto& r : res2)
		rv.emplace_back(Msg::ProposeChannelCandidates{
			Ln::NodeId(r.get<std::string>(0)),
			Ln::NodeId(r.get<std::string>(1))
		});
	return rv;
}

void
Secretary::remove_candidate(Sqlite3::Tx& tx, Ln::NodeId const& node) {
	tx.query(R"QRY(
		DELETE
		  FROM "ChannelCandidateInvestigator"
		 WHERE proposal = :proposal
		     ;
	)QRY")
		.bind(":proposal", std::string(node))
		.execute();
}

std::string
Secretary::report(Sqlite3::Tx& tx) {
	auto os = std::ostringstream();

	auto res = tx.query(R"QRY(
		SELECT proposal, score
		  FROM "ChannelCandidateInvestigator"
		 ORDER BY score DESC
	)QRY").execute();

	auto first = true;
	auto count = std::size_t(0);
	for (auto& r : res) {
		++count;
		if (first) {
			os << "Best candidates: "
			   << r.get<std::string>(0)
			   << "(" << r.get<std::size_t>(1) << ")"
			   ;
			first = false;
		} else if (count < 8) {
			os << ", "
			   << r.get<std::string>(0)
			   << "(" << r.get<std::size_t>(1) << ")"
			   ;
		}
	}
	if (count == 0)
		os << "No candidates.";
	if (count >= 8)
		os << " ... " << count << " candidates.";

	return os.str();
}
std::vector<std::pair<Ln::NodeId, std::int64_t>>
Secretary::get_all(Sqlite3::Tx& tx) {
	auto rv = std::vector<std::pair<Ln::NodeId, std::int64_t>>();
	auto res = tx.query(R"QRY(
		SELECT proposal, score
		  FROM "ChannelCandidateInvestigator"
		 ORDER BY score DESC
		     ;
	)QRY").execute();
	for (auto& r : res) {
		rv.emplace_back( Ln::NodeId(r.get<std::string>(0))
			       , r.get<std::int64_t>(1)
			       );
	}

	return rv;
}
std::vector<std::pair<Ln::NodeId, Ln::NodeId>>
Secretary::get_all_with_patrons(Sqlite3::Tx& tx) {
	auto rv = std::vector<std::pair<Ln::NodeId, Ln::NodeId>>();
	auto res = tx.query(R"QRY(
		SELECT proposal, patron
		  FROM "ChannelCandidateInvestigator"
		 ORDER BY score DESC
		     ;
	)QRY").execute();
	for (auto& r : res) {
		rv.emplace_back( Ln::NodeId(r.get<std::string>(0))
			       , Ln::NodeId(r.get<std::string>(1))
			       );
	}

	return rv;
}
bool
Secretary::is_candidate( Sqlite3::Tx& tx
		       , Ln::NodeId const& node
		       ) {
	auto res = tx.query(R"QRY(
		SELECT COUNT(proposal)
		  FROM "ChannelCandidateInvestigator"
		 WHERE proposal = :node
		     ;
	)QRY")
		.bind(":node", std::string(node))
		.execute();
	auto found = false;
	for (auto& r : res)
		found = (r.get<std::uint64_t>(0) != 0);

	return found;
}

}}}
